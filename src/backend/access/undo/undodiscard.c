/*-------------------------------------------------------------------------
 *
 * undodiscard.c
 *	  discard undo records
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undodiscard.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/undolog.h"
#include "access/undodiscard.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "postmaster/undoloop.h"

/*
 * Discard the undo for the log
 *
 * Search the undo log, get the start record for each transaction until we get
 * the transaction with xid >= xmin or an invalid xid.  Then call undolog
 * routine to discard upto that point and update the memory structure for the
 * log slot. We set the hibernate flag if we do not have any undo logs, this
 * flag is passed to the undo worker wherein it determines if system is idle
 * and it should sleep for sometime.
 *
 * Return the oldest xid remaining in this undo log (which should be >= xmin,
 * since we'll discard everything older).  Return InvalidTransactionId if the
 * undo log is empty.
 */
static TransactionId
UndoDiscardOneLog(UndoLogControl *log, TransactionId xmin, bool *hibernate)
{
	UndoRecPtr	undo_recptr, next_urecptr, from_urecptr, next_insert;
	UnpackedUndoRecord	*uur;
	bool	need_discard = false;
	uint16 uur_prevlen;
	TransactionId	undoxid;
	TransactionId	latest_discardxid = InvalidTransactionId;
	uint32	epoch;

	/*
	 * Check if there is anything for us to do in this undo log.  If not, we
	 * can return early.  We hold a lock because otherwise we can't safely
	 * read log->meta.insert.
	 */
	LWLockAcquire(&log->mutex, LW_SHARED);
	if (log->meta.discard == log->meta.insert)
	{
		LWLockRelease(&log->mutex);
		return InvalidTransactionId;
	}
	if (!TransactionIdPrecedes(log->oldest_xid, xmin))
	{
		TransactionId xid = log->oldest_xid;
		LWLockRelease(&log->mutex);
		return xid;
	}
	undo_recptr = MakeUndoRecPtr(log->logno, log->meta.discard);
	LWLockRelease(&log->mutex);

	/*
	 * TODO: Currently we are assuming that nothing else could advance the
	 * discard pointer after the above check.  If that ceases to be true (for
	 * example if a foreground process or another background worker could do
	 * it) we'll have to review this.
	 */

	/* Loop until we run out of discardable transactions. */
	do
	{
		bool isCommitted;

		/* Fetch the undo record for given undo_recptr. */
		uur = UndoFetchRecord(undo_recptr, InvalidBlockNumber,
							  InvalidOffsetNumber, InvalidTransactionId);

		Assert(uur != NULL);

		isCommitted = TransactionIdDidCommit(uur->uur_xid);
		next_urecptr = uur->uur_next;
		undoxid = uur->uur_xid;
		epoch = uur->uur_xidepoch;

		/* There might not be any undo log and hibernation might be needed. */
		*hibernate = true;

		/*
		 * At system restart, undo actions need to be applied for all the
		 * transactions which were running the last time system was up. Now,
		 * the transactions which were running when the system was up and those
		 * that are active now are in-progress. To distinguish them we compare
		 * their respective xids to oldestxmin. Basically, the transactions
		 * with xid smaller than oldestxmin are the aborted ones. Hence,
		 * performing their undo actions.
		 */
		if (!isCommitted && TransactionIdPrecedes(undoxid, xmin))
		{
			/*
			 * At the time of recovery, we might not have a valid next undo
			 * record pointer and in that case we'll calculate the location
			 * of from pointer using the last record of next insert location.
			 */
			if ((next_urecptr != SpecialUndoRecPtr)
				&& (next_urecptr != InvalidUndoRecPtr))
			{
				UnpackedUndoRecord *next_urec = UndoFetchRecord(next_urecptr,
																InvalidBlockNumber,
																InvalidOffsetNumber,
																InvalidTransactionId);
				from_urecptr = UndoGetPrevUndoRecptr(next_urecptr, next_urec->uur_prevlen);
				UndoRecordRelease(next_urec);
			}
			else
			{
				uur_prevlen = UndoLogGetPrevLen(log->logno);
				Assert(uur_prevlen != 0);
				next_insert = UndoLogGetNextInsertPtr(log->logno, undoxid);
				if (!UndoRecPtrIsValid(next_insert))
				{
					UndoRecordRelease(uur);
					continue;
				}

				from_urecptr = UndoGetPrevUndoRecptr(next_insert, uur_prevlen);
			}

			UndoRecordRelease(uur);
			uur = NULL;
			StartTransactionCommand();
			execute_undo_actions(from_urecptr, undo_recptr, true);
			CommitTransactionCommand();
		}

		/* we can discard upto this point. */
		if (TransactionIdFollowsOrEquals(undoxid, xmin) ||
			next_urecptr == SpecialUndoRecPtr ||
			next_urecptr == InvalidUndoRecPtr)
		{
			/* Hey, I got some undo log to discard, can not hibernate now. */
			*hibernate = false;

			if (uur)
				UndoRecordRelease(uur);

			/*
			 * If Transaction id is smaller than the xmin that means this must
			 * be the last transaction in this undo log, so we need to get the
			 * last insert point in this undo log and discard till that point.
			 * Also, if a transation is aborted, we stop discarding undo from the
			 * same location.
			 */
			if (TransactionIdPrecedes(undoxid, xmin))
			{
				UndoRecPtr	next_insert = InvalidUndoRecPtr;

				/*
				 * Get the last insert location for this transaction Id, if it
				 * returns invalid pointer that means there is new transaction
				 * has started for this undolog.
				 */
				next_insert = UndoLogGetNextInsertPtr(log->logno, undoxid);

				if (!UndoRecPtrIsValid(next_insert))
					continue;

				undo_recptr = next_insert;
				need_discard = true;
				epoch = 0;
				latest_discardxid = undoxid;
				undoxid = InvalidTransactionId;
			}

			LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
			log->oldest_xid = undoxid;
			log->oldest_xidepoch = epoch;
			LWLockRelease(&log->mutex);

			if (need_discard)
				UndoLogDiscard(undo_recptr, latest_discardxid);

			break;
		}

		/*
		 * This transaction is smaller than the xmin so lets jump to the next
		 * transaction.
		 */
		undo_recptr = next_urecptr;
		latest_discardxid = undoxid;

		if(uur)
			UndoRecordRelease(uur);

		need_discard = true;

	} while (true);

	return undoxid;
}

/*
 * Discard the undo for all the transaction whose xid is smaller than xmin
 *
 *	Check the DiscardInfo memory array for each slot (every undo log) , process
 *	the undo log for all the slot which have xid smaller than xmin or invalid
 *	xid. Fetch the record from the undo log transaction by transaction until we
 *	find the xid which is not smaller than xmin.
 */
void
UndoDiscard(TransactionId oldestXmin, bool *hibernate)
{
	TransactionId	oldestXidHavingUndo = oldestXmin;
	uint64			epoch = GetEpochForXid(oldestXmin);
	UndoLogControl *log = NULL;

	/*
	 * TODO: Ideally we'd arrange undo logs so that we can efficiently find
	 * those with oldest_xid < oldestXmin, but for now we'll just scan all of
	 * them.
	 */
	while ((log = UndoLogNext(log)))
	{
		TransactionId oldest_xid;

		oldest_xid = UndoDiscardOneLog(log, oldestXmin, hibernate);

		if (TransactionIdIsValid(oldest_xid) &&
			TransactionIdPrecedes(oldest_xid, oldestXidHavingUndo))
		{
			oldestXidHavingUndo = oldest_xid;
			epoch = GetEpochForXid(oldest_xid);
		}
	}

	/*
	 * Update the oldestXidWithEpochHavingUndo in the shared memory.
	 *
	 * XXX In future if multiple worker can perform discard then we may need
	 * to use compare and swap for updating the shared memory value.
	 */
	pg_atomic_write_u64(&ProcGlobal->oldestXidWithEpochHavingUndo,
						MakeEpochXid(epoch, oldestXidHavingUndo));
}
