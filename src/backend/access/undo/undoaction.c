/*-------------------------------------------------------------------------
 *
 * undoaction.c
 *	  execute undo actions
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undoaction.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/undoaction_xlog.h"
#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/xact.h"
#include "nodes/pg_list.h"
#include "postmaster/undoloop.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "utils/relfilenodemap.h"
#include "miscadmin.h"

static void execute_undo_actions_page(List *luur, UndoRecPtr urec_ptr, Oid reloid,
					 TransactionId xid, BlockNumber blkno, bool blk_chain_complete,
					bool nopartial);
static inline void undo_action_insert(Relation rel, Page page, OffsetNumber off,
									  TransactionId xid);

/*
 * execute_undo_actions - Execute the undo actions
 *
 * from_urecptr - undo record pointer from where to start applying undo action.
 * to_urecptr	- undo record pointer upto which point apply undo action.
 * nopartial	- true if rollback is for complete transaction.
 */
void
execute_undo_actions(UndoRecPtr from_urecptr, UndoRecPtr to_urecptr,
					 bool nopartial)
{
	UnpackedUndoRecord *uur = NULL;
	UndoRecPtr	urec_ptr;
	UndoRecPtr	save_urec_ptr;
	Oid			reloid;
	Oid			prev_reloid = InvalidOid;
	ForkNumber	prev_fork = InvalidForkNumber;
	BlockNumber	prev_block = InvalidBlockNumber;
	List	   *luur = NIL;
	bool		more_undo;
	TransactionId xid;

	Assert(from_urecptr != InvalidUndoRecPtr);
	/*
	 * If the location upto which rollback need to be done is not provided,
	 * then rollback the complete transaction.
	 * FIXME: this won't work if undolog crossed the limit of 1TB, because
	 * then from_urecptr and to_urecptr will be from different lognos.
	 */
	if (to_urecptr == InvalidUndoRecPtr)
	{
		UndoLogNumber logno = UndoRecPtrGetLogNo(from_urecptr);
		to_urecptr = UndoLogGetLastXactStartPoint(logno);
	}

	save_urec_ptr = urec_ptr = from_urecptr;

	while (urec_ptr >= to_urecptr)
	{
		uint16	urec_prevlen;

		more_undo = true;

		/* Fetch the undo record for given undo_recptr. */
		uur = UndoFetchRecord(urec_ptr, InvalidBlockNumber,
							  InvalidOffsetNumber, InvalidTransactionId);
		/*
		 * If the record is already discarded by undo worker,
		 * then we cannot fetch record successfully.
		 * Hence, exit quietly.
		 */
		if(uur == NULL)
			return;

		reloid = RelidByRelfilenode(uur->uur_tsid, uur->uur_relfilenode);
		xid = uur->uur_xid;

		/* Collect the undo records that belong to the same page. */
		if (!OidIsValid(prev_reloid) ||
			(prev_reloid == reloid &&
			 prev_fork == uur->uur_fork &&
			 prev_block == uur->uur_block))
		{
			prev_reloid = reloid;
			prev_fork = uur->uur_fork;
			prev_block = uur->uur_block;

			luur = lappend(luur, uur);
			urec_prevlen = uur->uur_prevlen;
			save_urec_ptr = uur->uur_blkprev;

			/* The undo chain must continue till we reach to_urecptr */
			if (urec_prevlen)
			{
				urec_ptr = UndoGetPrevUndoRecptr(urec_ptr, urec_prevlen);
				if (urec_ptr >= to_urecptr)
					continue;
				else
					more_undo = false;
			}
			else
				more_undo = false;
		}
		else
		{
			more_undo = true;
		}

		/*
		 * If no more undo is left to be processed and we are rolling back the
		 * complete transaction, then we can consider that the undo chain for a
		 * block is complete.
		 */
		if (!more_undo && nopartial)
		{
			execute_undo_actions_page(luur, save_urec_ptr, prev_reloid,
									  xid, prev_block, true, nopartial);
		}
		else
		{
			execute_undo_actions_page(luur, save_urec_ptr, prev_reloid,
									  xid, prev_block, false, nopartial);
		}

		/* release the undo records for which action has been replayed */
		while (luur)
		{
			UnpackedUndoRecord *uur = (UnpackedUndoRecord *) linitial(luur);
			UndoRecordRelease(uur);
			luur = list_delete_first(luur);
		}

		/*
		 * There are still more records to process, so keep moving backwards
		 * in the chain.
		 */
		if (more_undo)
		{
			luur = lappend(luur, uur);
			prev_reloid = reloid;
			prev_fork = uur->uur_fork;
			prev_block = uur->uur_block;
			save_urec_ptr = uur->uur_blkprev;

			/*
			 * Continue to process the records if this is not the last undo
			 * record in chain.
			 */
			urec_prevlen = uur->uur_prevlen;
			if (urec_prevlen)
				urec_ptr = UndoGetPrevUndoRecptr(urec_ptr, urec_prevlen);
			else
				break;
		}
		else
			break;
	}

	/* Apply the undo actions for the remaining records. */
	if (list_length(luur))
	{
		execute_undo_actions_page(luur, save_urec_ptr, prev_reloid,
								xid, prev_block, nopartial ? true : false,
								nopartial);

		/* release the undo records for which action has been replayed */
		while (luur)
		{
			UnpackedUndoRecord *uur = (UnpackedUndoRecord *) linitial(luur);
			UndoRecordRelease(uur);
			luur = list_delete_first(luur);
		}
	}

	if (!nopartial)
	{
		 /* Read the prevlen from the first record of this transaction. */
		uur = UndoFetchRecord(to_urecptr, InvalidBlockNumber,
							  InvalidOffsetNumber, InvalidTransactionId);
		Assert(uur != NULL);

		/*
		 * Rewind the insert location to start of this transaction.  This is
		 * to avoid reapplying some intermediate undo. We do not need to wal
		 * log this information here, because if the system crash before we
		 * rewind the insert pointer then after recovery we can identify
		 * whether the undo is already applied or not from the slot undo record
		 * pointer. Also set the correct prevlen value (what we have fetched
		 * from the undo).
		 */
		UndoLogRewind(to_urecptr, uur->uur_prevlen);

		/* set the local prevlen of undorecord.c */
		UndoRecordSetPrevUndoLen(uur->uur_prevlen);
		UndoRecordRelease(uur);
	}
}

/*
 * undo_action_insert - perform the undo action for insert
 *
 *	This will mark the tuple as dead so that the future access to it can't see
 *	this tuple.  We mark it as unused if there is no other index pointing to
 *	it, otherwise mark it as dead.
 */
static inline void
undo_action_insert(Relation rel, Page page, OffsetNumber off,
				   TransactionId xid)
{
	ItemId		lp;
	bool		relhasindex;

	/*
	 * This will mark the tuple as dead so that the future
	 * access to it can't see this tuple.  We mark it as
	 * unused if there is no other index pointing to it,
	 * otherwise mark it as dead.
	*/
	relhasindex = RelationGetForm(rel)->relhasindex;
	lp = PageGetItemId(page, off);
	Assert(ItemIdIsNormal(lp));
	if (relhasindex)
	{
		ItemIdSetDead(lp);
	}
	else
	{
		ItemIdSetUnused(lp);
		/* Set hint bit for ZPageAddItem */
		PageSetHasFreeLinePointers(page);
	}

	ZPageSetPrunable(page, xid);
}

/*
 * execute_undo_actions_page - Execute the undo actions for a page
 *
 *	After applying all the undo actions for a page, we clear the transaction
 *	slot on a page if the undo chain for block is complete, otherwise rewind
 *	the undo pointer to the last record for that block that precedes the last
 *	undo record for which action is replayed.
 *
 *	luur - list of unpacked undo records for which undo action needs to be
 *		   replayed.
 *	urec_ptr - undo record pointer to which we need to rewind.
 *	reloid	- OID of relation on which undo actions needs to be applied.
 *	blkno	- block number on which undo actions needs to be applied.
 *	blk_chain_complete - indicates whether the undo chain for block is
 *						 complete.
 *	nopartial - true if rollback is for complete transaction. If we are not
 *				rolling back the complete transaction then we need to apply the
 *				undo action for UNDO_INVALID_XACT_SLOT also because in such
 *				case we will rewind the insert undo location.
 */
static void
execute_undo_actions_page(List *luur, UndoRecPtr urec_ptr, Oid reloid,
						  TransactionId xid, BlockNumber blkno,
						  bool blk_chain_complete, bool nopartial)
{
	ListCell   *l_iter;
	Relation	rel;
	Buffer		buffer;
	Page		page;
	ZHeapPageOpaque	opaque;
	int			slot_no = 0;

	/*
	 * FIXME: If reloid is not valid then we have nothing to do. In future,
	 * we might want to do it differently for transactions that perform both
	 * DDL and DML operations.
	 */
	if (!OidIsValid(reloid))
	{
		elog(LOG, "ignoring undo for invalid reloid");
		return;
	}

	/*
	 * If the action is executed by backend as a result of rollback, we must
	 * already have an appropriate lock on relation.
	 */
	rel = heap_open(reloid, RowExclusiveLock);

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(page);

	/* Identify the slot number for this transaction */
	while (slot_no < MAX_PAGE_TRANS_INFO_SLOTS &&
		   !(TransactionIdEquals(xid, opaque->transinfo[slot_no].xid)))
		slot_no++;

	/*
	 * If undo action has been already applied for this page then skip
	 * the process altogether.
	 */
	if (opaque->transinfo[slot_no].urec_ptr <= urec_ptr ||
		slot_no == MAX_PAGE_TRANS_INFO_SLOTS)
	{
		UnlockReleaseBuffer(buffer);
		heap_close(rel, NoLock);
		return;
	}

	START_CRIT_SECTION();

	foreach(l_iter, luur)
	{
		UnpackedUndoRecord *uur = (UnpackedUndoRecord *) lfirst(l_iter);

		switch (uur->uur_type)
		{
			case UNDO_INSERT:
				{
					undo_action_insert(rel, page, uur->uur_offset, xid);
				}
				break;
			case UNDO_MULTI_INSERT:
				{
					OffsetNumber	start_offset;
					OffsetNumber	end_offset;
					OffsetNumber	iter_offset;

					start_offset = ((OffsetNumber *) uur->uur_payload.data)[0];
					end_offset = ((OffsetNumber *) uur->uur_payload.data)[1];

					for (iter_offset = start_offset;
						 iter_offset <= end_offset;
						 iter_offset++)
					{
						undo_action_insert(rel, page, iter_offset, xid);
					}
				}
				break;
			case UNDO_DELETE:
			case UNDO_UPDATE:
			case UNDO_INPLACE_UPDATE:
				{
					ItemId		lp;
					ZHeapTupleHeader zhtup;
					Size		offset = 0;
					uint32		undo_tup_len;

					/* Copy the entire tuple from undo. */
					lp = PageGetItemId(page, uur->uur_offset);
					Assert(ItemIdIsNormal(lp));
					zhtup = (ZHeapTupleHeader) PageGetItem(page, lp);

					undo_tup_len = *((uint32 *) &uur->uur_tuple.data[offset]);
					ItemIdChangeLen(lp, undo_tup_len);
					/* skip ctid and tableoid stored in undo tuple */
					offset += sizeof(uint32) + sizeof(ItemPointerData) + sizeof(Oid);
					memcpy(zhtup,
						   (ZHeapTupleHeader) &uur->uur_tuple.data[offset],
						   undo_tup_len);
				}
				break;
			case UNDO_XID_LOCK_ONLY:
				{
					ItemId		lp;
					ZHeapTupleHeader zhtup, undo_tup_hdr;

					/* Copy the entire tuple from undo. */
					lp = PageGetItemId(page, uur->uur_offset);
					Assert(ItemIdIsNormal(lp));
					zhtup = (ZHeapTupleHeader) PageGetItem(page, lp);

					/*
					 * Override the tuple header values with values retrieved
					 * from undo record.
					 */
					undo_tup_hdr = (ZHeapTupleHeader) uur->uur_tuple.data;
					zhtup->t_infomask2 = undo_tup_hdr->t_infomask2;
					zhtup->t_infomask = undo_tup_hdr->t_infomask;
					zhtup->t_hoff = undo_tup_hdr->t_hoff;
				}
				break;
			case UNDO_INVALID_XACT_SLOT:
				/*
				 * If we are rewinding the undo log insert location then apply
				 * the undo action for invalid xact slot.  Refer detailed
				 * comments in PageFreezeTransSlots.
				 */
				if (!nopartial)
				{
					ItemId		lp;
					ZHeapTupleHeader zhtup;

					lp = PageGetItemId(page, uur->uur_offset);

					/* Reset the invalid xact flag from the tuple/itemid. */
					if (ItemIdIsDeleted(lp))
						ItemIdResetInvalidXact(lp);
					else
					{
						zhtup = (ZHeapTupleHeader) PageGetItem(page, lp);
						zhtup->t_infomask &= ~ZHEAP_INVALID_XACT_SLOT;
					}
				}
				break;
			default:
				elog(ERROR, "unsupported undo record type");
		}
	}

	/* update the transaction slot */
	if (blk_chain_complete)
	{
		opaque->transinfo[slot_no].xid = InvalidTransactionId;
		opaque->transinfo[slot_no].urec_ptr = InvalidUndoRecPtr;
	}
	else
		opaque->transinfo[slot_no].urec_ptr = urec_ptr;

	MarkBufferDirty(buffer);

	/*
	 * We are logging the complete page for undo actions, so we don't need to
	 * record the data for individual operations.  We can optimize it by
	 * recording the data for individual operations, but again if there are
	 * multiple operations, then it might be better to log the complete page.
	 * So we can have some threshold above which we always log the complete
	 * page.
	 */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

		recptr = XLogInsert(RM_UNDOACTION_ID, XLOG_UNDO_PAGE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	heap_close(rel, NoLock);
}
