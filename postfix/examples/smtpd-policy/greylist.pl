#!/usr/bin/perl

use DB_File;
use Fcntl;
use Sys::Syslog qw(:DEFAULT setlogsock);

#
# Usage: greylist.pl [-v]
#
# Demo delegated Postfix SMTPD policy server. This server implements
# greylisting. State is kept in a Berkeley DB database.  Logging is
# sent to syslogd.
#
# How it works: each time a Postfix SMTP server process is started
# it connects to the policy service socket, and Postfix runs one
# instance of this PERL script.  By default, a Postfix SMTP server
# process terminates after 100 seconds of idle time, or after serving
# 100 clients. Thus, the cost of starting this PERL script is smoothed
# out over time.
#
# To run this from /etc/postfix/master.cf:
#
#    policy  unix  -       n       n       -       -       spawn
#      user=nobody argv=/usr/bin/perl /usr/libexec/postfix/greylist.pl
#
# To use this from Postfix SMTPD, use in /etc/postfix/main.cf:
#
#    smtpd_recipient_restrictions =
#	...
#	reject_unauth_destination
#	check_policy_service unix:private/policy
#	...
#
# NOTE: specify check_policy_service AFTER reject_unauth_destination
# or else your system can become an open relay.
#
# To test this script by hand, execute:
#
#    % perl greylist.pl
#
# Each query is a bunch of attributes. Order does not matter, and
# the demo script uses only a few of all the attributes shown below:
#
#    request=smtpd_access_policy
#    protocol_state=RCPT
#    protocol_name=SMTP
#    helo_name=some.domain.tld
#    queue_id=8045F2AB23
#    sender=foo@bar.tld
#    recipient=bar@foo.tld
#    client_address=1.2.3.4
#    client_name=another.domain.tld
#    instance=123.456.7
#    sasl_method=plain
#    sasl_username=you
#    sasl_sender=
#    size=12345
#    [empty line]
#
# The policy server script will answer in the same style, with an
# attribute list followed by a empty line:
#
#    action=dunno
#    [empty line]
#

#
# greylist status database and greylist time interval. DO NOT create the
# greylist status database in a world-writable directory such as /tmp
# or /var/tmp. DO NOT create the greylist database in a file system
# that can run out of space.
#
# In case of database corruption, this script saves the database as
# $database_name.time(), so that the mail system does not get stuck.
#
$database_name="/Library/Server/Mail/Data/gldb/greylist.db";
$whitelist_host_file="/Library/Server/Mail/Data/gldb/whitelist_host";
$whitelist_domain_file="/Library/Server/Mail/Data/gldb/whitelist_domain";
$whitelist_db_name="/Library/Server/Mail/Data/gldb/whitelist.db";
$greylist_delay=60;

#
# Auto-whitelist threshold. Specify 0 to disable, or the number of
# successful "come backs" after which a client is no longer subject
# to greylisting.
#
$auto_whitelist_threshold = 10;

#
# Syslogging options for verbose mode and for fatal errors.
# NOTE: comment out the $syslog_socktype line if syslogging does not
# work on your system.
#
$syslog_socktype = 'unix'; # inet, unix, stream, console
$syslog_facility="mail";
$syslog_options="pid";
$syslog_priority="info";

sub add_whitelist {
	# check for null host name
	my ($_host_name) = $attr{"host_name"};
	if ($_host_name eq "") {
		syslog $syslog_priority, "Warning: missing whitelist host name attribute";
		return 0;
	}

	# Open the database on the fly.
	open_whitelist_db() unless $whitelist_db_obj;

	# Is host already in white list
	$value = read_whitelist_db($attr{"host_name"});

	# Add host if not in database
	if ($value == 0) {
		syslog $syslog_priority, "adding host: %s to whitelist host", $attr{"host_name"} if $verbose;
		update_whitelist_db($attr{"host_name"}, 1);

		open WHITELIST_FILE, ">> $whitelist_host_file" or 
			syslog $syslog_priority, "Error: unable to open whitelist host file: %s", $whitelist_host_file;
		print WHITELIST_FILE "$attr{\"host_name\"}\n";
		close WHITELIST_FILE;
	}
}

sub add_whitelist_domain {
	# check for null host name
	my ($_domain_name) = $attr{"domain_name"};
	if ($_domain_name eq "") {
		syslog $syslog_priority, "Warning: missing whitelist domain name attribute";
		return 0;
	}

	# Open the database on the fly.
	open_whitelist_db() unless $whitelist_db_obj;

	# Is domain already in white list
	$value = read_whitelist_db($attr{"domain_name"});

	# Add domain if not in database
	if ($value == 0) {
		syslog $syslog_priority, "adding domain: %s to whitelist doman", $attr{"domain_name"} if $verbose;
		update_whitelist_db($attr{"domain_name"}, 1);

		open WHITELIST_FILE, ">> $whitelist_domain_file" or 
			syslog $syslog_priority, "Error: unable to open whitelist domain file: %s", $whitelist_domain_file;
		print WHITELIST_FILE "$attr{\"domain_name\"}\n";
		close WHITELIST_FILE;
	}
}

#
# Demo SMTPD access policy routine. The result is an action just like
# it would be specified on the right-hand side of a Postfix access
# table.  Request attributes are available via the %attr hash.
#
sub smtpd_access_policy {
    my($key, $time_stamp, $now, $count, $domain);

    # Open the database on the fly.
    open_database() unless $database_obj;

    # Open the whitelist database on the fly.
    open_whitelist_db() unless $whitelist_db_obj;

    # Check if domain is whitelisted
	$domain = get_domain_name($attr{"client_name"});
	$count = read_whitelist_db($domain);
	if ($count > 0) {
		syslog $syslog_priority, "domain: %s is whitelisted", $domain if $verbose;
	    return "dunno";
	}

    # Check if host is whitelisted
	$count = read_whitelist_db($attr{"client_name"});
	if ($count > 0) {
		syslog $syslog_priority, "host: %s is whitelisted", $attr{"client_name"} if $verbose;
	    return "dunno";
	}

    # Search the auto-whitelist.
    if ($auto_whitelist_threshold > 0) {
        $count = read_database($attr{"client_address"});
        if ($count > $auto_whitelist_threshold) {
	    return "dunno";
        }
    }

    # Lookup the time stamp for this client/sender/recipient.
    $key =
	lc $attr{"client_address"}."/".$attr{"sender"}."/".$attr{"recipient"};
    $time_stamp = read_database($key);
    $now = time();

    # If this is a new request add this client/sender/recipient to the database.
    if ($time_stamp == 0) {
	$time_stamp = $now;
	update_database($key, $time_stamp);
    }

    # The result can be any action that is allowed in a Postfix access(5) map.
    #
    # To label mail, return ``PREPEND'' headername: headertext
    #
    # In case of success, return ``DUNNO'' instead of ``OK'' so that the
    # check_policy_service restriction can be followed by other restrictions.
    #
    # In case of failure, specify ``DEFER_IF_PERMIT optional text...''
    # so that mail can still be blocked by other access restrictions.
    #
    syslog $syslog_priority, "request age %d", $now - $time_stamp if $verbose;
    if ($now - $time_stamp > $greylist_delay) {
	# Update the auto-whitelist.
	if ($auto_whitelist_threshold > 0) {
	    update_database($attr{"client_address"}, $count + 1);
	}
	return "dunno";
    } else {
	# Apple
	syslog $syslog_priority, "Temporary message rejection to: <$attr{\"recipient\"}> from: <$attr{\"sender\"}> sent from: [$attr{\"client_address\"}] for: $greylist_delay seconds due to greylisting";
	return "defer_if_permit Service is unavailable";
    }
}

#
# You should not have to make changes below this point.
#
sub LOCK_SH { 1 };	# Shared lock (used for reading).
sub LOCK_EX { 2 };	# Exclusive lock (used for writing).
sub LOCK_NB { 4 };	# Don't block (for testing).
sub LOCK_UN { 8 };	# Release lock.

#
# Log an error and abort.
#
sub fatal_exit {
    my($first) = shift(@_);
    syslog "err", "fatal: $first", @_;
    exit 1;
}

#
# Open hash database.
#
sub open_database {
    my($database_fd);

    # Use tied database to make complex manipulations easier to express.
    $database_obj = tie(%db_hash, 'DB_File', $database_name,
				O_CREAT|O_RDWR, 0644, $DB_BTREE);
	if ( !$database_obj ) {
		# don't prevent mail deliveries due to corrupt database
		my $db_backup = $database_name . "." . time();
		syslog $syslog_priority, "Warning: open failed for: %s : backing up to: %s",
									$database_name, $db_backup;
		rename $database_name, $db_backup ||
			fatal_exit "Can't save %s as %s: $!", $database_name, $db_backup;

		# try again
		$database_obj = tie(%db_hash, 'DB_File', $database_name,
					O_CREAT|O_RDWR, 0644, $DB_BTREE) ||
					fatal_exit "Cannot open database %s: $!", $database_name;
	}
    $database_fd = $database_obj->fd;
    open DATABASE_HANDLE, "+<&=$database_fd" ||
	fatal_exit "Cannot fdopen database %s: $!", $database_name;
    syslog $syslog_priority, "open %s", $database_name if $verbose;
}

#
# Open hash whitelist database.
#
sub open_whitelist_db {
    my($whitelist_db_fd);

    # Use tied database to make complex manipulations easier to express.
	$whitelist_db_obj = tie(%db_hash, 'DB_File', $whitelist_db_name,
				O_CREAT|O_RDWR, 0644, $DB_BTREE);
	if ( !$whitelist_db_obj ) {
		# don't prevent mail deliveries due to corrupt database
		my $db_backup = $whitelist_db_name . "." . time();
		syslog $syslog_priority, "Warning: open failed for: %s : backing up to: %s",
									$whitelist_db_name, $db_backup;
		rename $whitelist_db_name, $db_backup ||
			fatal_exit "Can't save %s as %s: $!", $whitelist_db_name, $db_backup;

		# try again
		$whitelist_db_obj = tie(%db_hash, 'DB_File', $whitelist_db_name,
					O_CREAT|O_RDWR, 0644, $DB_BTREE) ||
					fatal_exit "Cannot open database %s: $!", $whitelist_db_name;
	}
    $whitelist_db_fd = $whitelist_db_obj->fd;
    open WHITELIST_DB_HANDLE, "+<&=$whitelist_db_fd" ||
	fatal_exit "Cannot fdopen database %s: $!", $whitelist_db_name;
    syslog $syslog_priority, "open %s", $whitelist_db_name if $verbose;
}

#
# Read database. Use a shared lock to avoid reading the database
# while it is being changed. XXX There should be a way to synchronize
# our cache from the on-file database before looking up the key.
#
sub read_database {
    my($key) = @_;
    my($value);

    flock DATABASE_HANDLE, LOCK_SH ||
	fatal_exit "Can't get shared lock on %s: $!", $database_name;
    # XXX Synchronize our cache from the on-disk copy before lookup.
    $value = $db_hash{$key};
    syslog $syslog_priority, "lookup %s: %s", $key, $value if $verbose;
    flock DATABASE_HANDLE, LOCK_UN ||
	fatal_exit "Can't unlock %s: $!", $database_name;
    return $value;
}

#
# Read database. Use a shared lock to avoid reading the database
# while it is being changed. XXX There should be a way to synchronize
# our cache from the on-file database before looking up the key.
#
sub read_whitelist_db {
    my($key) = @_;
    my($value);

    flock WHITELIST_DB_HANDLE, LOCK_SH ||
	fatal_exit "Can't get shared lock on %s: $!", $whitelist_db_name;
    # XXX Synchronize our cache from the on-disk copy before lookup.
    $value = $db_hash{$key};
    syslog $syslog_priority, "whitelist lookup %s: %s", $key, $value if $verbose;
    flock WHITELIST_DB_HANDLE, LOCK_UN ||
	fatal_exit "Can't unlock %s: $!", $whitelist_db_name;
    return $value;
}

#
# Update database. Use an exclusive lock to avoid collisions with
# other updaters, and to avoid surprises in database readers. XXX
# There should be a way to synchronize our cache from the on-file
# database before updating the database.
#
sub update_database {
    my($key, $value) = @_;

    syslog $syslog_priority, "store %s: %s", $key, $value if $verbose;
    flock DATABASE_HANDLE, LOCK_EX ||
	fatal_exit "Can't exclusively lock %s: $!", $database_name;
    # XXX Synchronize our cache from the on-disk copy before update.
    $db_hash{$key} = $value;
    $database_obj->sync() &&
	fatal_exit "Can't update %s: $!", $database_name;
    flock DATABASE_HANDLE, LOCK_UN ||
	fatal_exit "Can't unlock %s: $!", $database_name;
}

#
# Update database. Use an exclusive lock to avoid collisions with
# other updaters, and to avoid surprises in database readers. XXX
# There should be a way to synchronize our cache from the on-file
# database before updating the database.
#
sub update_whitelist_db {
    my($key, $value) = @_;

    syslog $syslog_priority, "store whitelist host %s: %s", $key, $value if $verbose;
    flock WHITELIST_DB_HANDLE, LOCK_EX ||
	fatal_exit "Can't exclusively lock %s: $!", $whitelist_db_name;
    # XXX Synchronize our cache from the on-disk copy before update.
    $db_hash{$key} = $value;
    $whitelist_db_obj->sync() &&
	fatal_exit "Can't update %s: $!", $whitelist_db_name;
    flock WHITELIST_DB_HANDLE, LOCK_UN ||
	fatal_exit "Can't unlock %s: $!", $whitelist_db_name;
}

#
# Parse hostname to obtain domain name
#
sub get_domain_name {
    my($in_host_name) = @_;
    my($value);
	my($count) = 0;

	@tokens = split(/\./, $in_host_name);
	$count = $#tokens;
	$value=$tokens[$count-1] . "." . $tokens[$count];
	return $value;
}

#
# Signal 11 means that we have some kind of database corruption (yes
# Berkeley DB should handle this better).  Move the corrupted database
# out of the way, and start with a new database.
#
sub sigsegv_handler {
    my $backup = $database_name . "." . time();

    rename $database_name, $backup || 
	fatal_exit "Can't save %s as %s: $!", $database_name, $backup;
    fatal_exit "Caught signal 11; the corrupted database is saved as $backup";

    my $wl_backup = $whitelist_db_name . "." . time();

    rename $whitelist_db_name, $wl_backup || 
	fatal_exit "Can't save %s as %s: $!", $whitelist_db_name, $wl_backup;
    fatal_exit "Caught signal 11; the corrupted database is saved as $wl_backup";
}

$SIG{'SEGV'} = 'sigsegv_handler';

#
# This process runs as a daemon, so it can't log to a terminal. Use
# syslog so that people can actually see our messages.
#
setlogsock $syslog_socktype;
openlog $0, $syslog_options, $syslog_facility;

#
# We don't need getopt() for now.
#
while ($option = shift(@ARGV)) {
    if ($option eq "-v") {
	$verbose = 1;
    } else {
	syslog $syslog_priority, "Invalid option: %s. Usage: %s [-v]",
		$option, $0;
	exit 1;
    }
}

#
# Unbuffer standard output.
#
select((select(STDOUT), $| = 1)[0]);

#
# Receive a bunch of attributes, evaluate the policy, send the result.
#
while (<STDIN>) {
    if (/([^=]+)=(.*)\n/) {
	$attr{substr($1, 0, 512)} = substr($2, 0, 512);
    } elsif ($_ eq "\n") {
	if ($verbose) {
	    for (keys %attr) {
		syslog $syslog_priority, "Attribute: %s=%s", $_, $attr{$_};
	    }
	}
		if ( $attr{"request"} eq "smtpd_access_policy" ) {
			$action = smtpd_access_policy();
		} elsif ( $attr{"request"} eq "whitelist" ) {
			$action = add_whitelist();
		} elsif ( $attr{"request"} eq "whitelist_domain" ) {
			$action = add_whitelist_domain();
		} else {
			fatal_exit "unrecognized request type: '%s'", $attr{request};
		}
		syslog $syslog_priority, "Action: %s", $action if $verbose;
		print STDOUT "action=$action\n\n";
		%attr = ();
    } else {
	chop;
	syslog $syslog_priority, "warning: ignoring garbage: %.100s", $_;
    }
}
