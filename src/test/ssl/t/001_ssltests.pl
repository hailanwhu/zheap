use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 40;
use ServerSetup;
use File::Copy;

#### Some configuration

# This is the hostname used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';

# Allocation of base connection string shared among multiple tests.
my $common_connstr;

# The client's private key must not be world-readable, so take a copy
# of the key stored in the code tree and update its permissions.
copy("ssl/client.key", "ssl/client_tmp.key");
chmod 0600, "ssl/client_tmp.key";

#### Part 0. Set up the server.

note "setting up data directory";
my $node = get_new_node('master');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;
configure_test_server_for_ssl($node, $SERVERHOSTADDR, 'trust');
switch_server_cert($node, 'server-cn-only');

### Part 1. Run client-side tests.
###
### Test that libpq accepts/rejects the connection correctly, depending
### on sslmode and whether the server's certificate looks correct. No
### client certificate is used in these tests.

note "running client tests";

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# The server should not accept non-SSL connections.
test_connect_fails($common_connstr, "sslmode=disable",
				   "server doesn't accept non-SSL connections");

# Try without a root cert. In sslmode=require, this should work. In verify-ca
# or verify-full mode it should fail.
test_connect_ok($common_connstr, "sslrootcert=invalid sslmode=require",
				"connect without server root cert sslmode=require");
test_connect_fails($common_connstr, "sslrootcert=invalid sslmode=verify-ca",
				"connect without server root cert sslmode=verify-ca");
test_connect_fails($common_connstr, "sslrootcert=invalid sslmode=verify-full",
				   "connect without server root cert sslmode=verify-full");

# Try with wrong root cert, should fail. (We're using the client CA as the
# root, but the server's key is signed by the server CA.)
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/client_ca.crt sslmode=require",
				   "connect with wrong server root cert sslmode=require");
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/client_ca.crt sslmode=verify-ca",
				   "connect with wrong server root cert sslmode=verify-ca");
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/client_ca.crt sslmode=verify-full",
				   "connect with wrong server root cert sslmode=verify-full");

# Try with just the server CA's cert. This fails because the root file
# must contain the whole chain up to the root CA.
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/server_ca.crt sslmode=verify-ca",
				   "connect with server CA cert, without root CA");

# And finally, with the correct root cert.
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=require",
				"connect with correct server CA cert file sslmode=require");
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
				"connect with correct server CA cert file sslmode=verify-ca");
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=verify-full",
				"connect with correct server CA cert file sslmode=verify-full");

# Test with cert root file that contains two certificates. The client should
# be able to pick the right one, regardless of the order in the file.
test_connect_ok($common_connstr,
				"sslrootcert=ssl/both-cas-1.crt sslmode=verify-ca",
				"cert root file that contains two certificates, order 1");
test_connect_ok($common_connstr,
				"sslrootcert=ssl/both-cas-2.crt sslmode=verify-ca",
				"cert root file that contains two certificates, order 2");

# CRL tests

# Invalid CRL filename is the same as no CRL, succeeds
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=invalid",
				"sslcrl option with invalid file name");

# A CRL belonging to a different CA is not accepted, fails
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/client.crl",
				   "CRL belonging to a different CA");

# With the correct CRL, succeeds (this cert is not revoked)
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
				"CRL with a non-revoked cert");

# Check that connecting with verify-full fails, when the hostname doesn't
# match the hostname in the server's certificate.
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok($common_connstr, "sslmode=require host=wronghost.test",
				"mismatch between host name and server certificate sslmode=require");
test_connect_ok($common_connstr, "sslmode=verify-ca host=wronghost.test",
				"mismatch between host name and server certificate sslmode=verify-ca");
test_connect_fails($common_connstr, "sslmode=verify-full host=wronghost.test",
				   "mismatch between host name and server certificate sslmode=verify-full");


# Test Subject Alternative Names.
switch_server_cert($node, 'server-multiple-alt-names');

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok($common_connstr, "host=dns1.alt-name.pg-ssltest.test",
				"host name matching with X.509 Subject Alternative Names 1");
test_connect_ok($common_connstr, "host=dns2.alt-name.pg-ssltest.test",
				"host name matching with X.509 Subject Alternative Names 2");
test_connect_ok($common_connstr, "host=foo.wildcard.pg-ssltest.test",
				"host name matching with X.509 Subject Alternative Names wildcard");

test_connect_fails($common_connstr, "host=wronghost.alt-name.pg-ssltest.test",
				   "host name not matching with X.509 Subject Alternative Names");
test_connect_fails($common_connstr,
				   "host=deep.subdomain.wildcard.pg-ssltest.test",
				   "host name not matching with X.509 Subject Alternative Names wildcard");

# Test certificate with a single Subject Alternative Name. (this gives a
# slightly different error message, that's all)
switch_server_cert($node, 'server-single-alt-name');

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok($common_connstr, "host=single.alt-name.pg-ssltest.test",
				"host name matching with a single X.509 Subject Alternative Name");

test_connect_fails($common_connstr, "host=wronghost.alt-name.pg-ssltest.test",
				   "host name not matching with a single X.509 Subject Alternative Name");
test_connect_fails($common_connstr,
				   "host=deep.subdomain.wildcard.pg-ssltest.test",
				   "host name not matching with a single X.509 Subject Alternative Name wildcard");

# Test server certificate with a CN and SANs. Per RFCs 2818 and 6125, the CN
# should be ignored when the certificate has both.
switch_server_cert($node, 'server-cn-and-alt-names');

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok($common_connstr, "host=dns1.alt-name.pg-ssltest.test",
				"certificate with both a CN and SANs 1");
test_connect_ok($common_connstr, "host=dns2.alt-name.pg-ssltest.test",
				"certificate with both a CN and SANs 2");
test_connect_fails($common_connstr, "host=common-name.pg-ssltest.test",
				   "certificate with both a CN and SANs ignores CN");

# Finally, test a server certificate that has no CN or SANs. Of course, that's
# not a very sensible certificate, but libpq should handle it gracefully.
switch_server_cert($node, 'server-no-names');
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok($common_connstr,
				"sslmode=verify-ca host=common-name.pg-ssltest.test",
				"server certificate without CN or SANs sslmode=verify-ca");
test_connect_fails($common_connstr,
				   "sslmode=verify-full host=common-name.pg-ssltest.test",
				   "server certificate without CN or SANs sslmode=verify-full");

# Test that the CRL works
switch_server_cert($node, 'server-revoked');

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# Without the CRL, succeeds. With it, fails.
test_connect_ok($common_connstr,
				"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
				"connects without client-side CRL");
test_connect_fails($common_connstr,
				   "sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
				   "does not connect with client-side CRL");

### Part 2. Server-side tests.
###
### Test certificate authorization.

note "running server tests";

$common_connstr =
"sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR";

# no client cert
test_connect_fails($common_connstr,
				   "user=ssltestuser sslcert=invalid",
				   "certificate authorization fails without client cert");

# correct client cert
test_connect_ok($common_connstr,
				"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
				"certificate authorization succeeds with correct client cert");

# client cert belonging to another user
test_connect_fails($common_connstr,
				   "user=anotheruser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
				   "certificate authorization fails with client cert belonging to another user");

# revoked client cert
test_connect_fails($common_connstr,
				   "user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=ssl/client-revoked.key",
				   "certificate authorization fails with revoked client cert");

# intermediate client_ca.crt is provided by client, and isn't in server's ssl_ca_file
switch_server_cert($node, 'server-cn-only', 'root_ca');
$common_connstr =
"user=ssltestuser dbname=certdb sslkey=ssl/client_tmp.key sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok($common_connstr,
				"sslmode=require sslcert=ssl/client+client_ca.crt",
				"intermediate client certificate is provided by client");
test_connect_fails($common_connstr, "sslmode=require sslcert=ssl/client.crt",
				   "intermediate client certificate is missing");

# clean up
unlink "ssl/client_tmp.key";
