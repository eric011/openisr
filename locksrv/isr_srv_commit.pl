#!/usr/bin/perl

######################################################################
# isr_srv_commit.pl - Commits a locally cached parcel on the server
######################################################################

#
#                       Internet Suspend/Resume
#           A system for capture and transport of PC state
#
#              Copyright (c) 2002-2004, Intel Corporation
#            Copyright (c) 2004, Carnegie Mellon University
#
# This software is distributed under the terms of the Eclipse Public
# License, Version 1.0 which can be found in the file named LICENSE.Eclipse.
# ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
# RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

###################
# Prologue
###################
use strict;
use Getopt::Std;
use lib "/usr/local/isr/bin";
use Server;
$| = 1; # Autoflush output on every print statement

####################
# Begin main routine
####################

# 
# Local variables
#
my $username;
my $parcelname;
my $parceldir;
my $lastver;
my $nextver;
my $nextverfilename;
my $lastdir;
my $nextdir;
my $cachedir;
my $numdirs;
my $i;
my $verbose;
my $file;

my @files;

#
# Parse the command line args
#
no strict 'vars';
getopts('hu:p:V');

if ($opt_h) {
    usage();
}
if (!$opt_p) {
    usage("Missing parcel name (-p)");
}
$username = $opt_u;
$username = $ENV{'USER'} if !$username;
$parcelname = $opt_p;
$parceldir = "$Server::CONTENT_ROOT$username/$parcelname";
$verbose = $opt_V;
use strict 'vars';

#
# Make sure the parcel directory exists
#
(-e $parceldir)
    or errexit("$parceldir does not exist");

#
# If there is no cache directory, simply exit with successful status
#
$cachedir = "$parceldir/cache";
if (! -e $cachedir) {
    exit 0;
}

# Otherwise, make sure the cache has the right permissions
system("chmod -R 755 $parceldir/cache");

#
# Determine the most recent (last) version number
# 
opendir(DIR, $parceldir)
    or errexit("Could not open directory $parceldir");

# List only files whose names contain only digits
@files = reverse sort grep(/^\d+$/, readdir(DIR)); 
closedir(DIR);
$lastver = int(@files[0]);
$nextver = $lastver + 1;

#
# Set some variables that we need later on
#
$lastdir = "$parceldir/" . sprintf("%06d", $lastver);
$nextdir = "$parceldir/" . sprintf("%06d", $nextver);

#
# Make sure the expected files are present in cache and last
# before doing anything else
# 
# Check cache
(-e "$cachedir/cfg.tgz.enc")
    or errexit("Missing $cachedir/cfg.tgz.enc. No changes were made on the server.");
(-e "$cachedir/keyring.enc")
    or errexit("Missing $cachedir/keyring.enc. No changes were made on the server.");
(-e "$cachedir/hdk")
    or errexit("Missing $cachedir/hdk directory. No changes were made on the server.");

# Check last
(-e "$lastdir/cfg.tgz.enc")
    or errexit("Missing $lastdir/cfg.tgz.enc. No changes were made on the server.");
(-e "$lastdir/keyring.enc")
    or errexit("Missing $lastdir/keyring.enc. No changes were made on the server.");
(-e "$lastdir/hdk")
    or errexit("Missing $lastdir/hdk directory. No changes were made on the server.");

#
# Set some hdk parameters
#
$numdirs = get_numdirs(get_parcelcfg_path($username, $parcelname));

#
# Move the entire last directory to next
#
rename($lastdir, $nextdir)
    or unix_errexit("Couldn't move $lastdir to $nextdir");

# 
# Move all of the non-chunk files from next back to last where they belong.
#
mkdir($lastdir)
    or unix_errexit("Couldn't make $lastdir");
mkdir("$lastdir/hdk")
    or unix_errexit("Couldn't make $lastdir/hdk");

foreach $file ("cfg.tgz.enc", "keyring.enc") {
    rename("$nextdir/$file", "$lastdir/$file")
	or unix_errexit("Unable to move $file from $nextdir back to $lastdir");
}

#
# Move the new non-chunk files from the cache into next
#
foreach $file ("cfg.tgz.enc", "keyring.enc") {
    rename("$cachedir/$file", "$nextdir/$file")
	or unix_errexit("Unable to move $file files from $cachedir to $nextdir");
}

# 
# For each cache chunk k, move chunk k from next to last, then move chunk k from cache to next
#
print "Scanning $numdirs dirs in $cachedir...\n"
    if $verbose;

for ($i = 0; $i < $numdirs; $i++) {
    my $dirname = sprintf("%04d", $i);
    my $chunk;
    my @files;
    
    # If the directory exists, copy its chunks, otherwise go to next dir
    if (opendir(DIR, "$cachedir/hdk/$dirname")) {
	# Generate a sorted list of the chunks in the current directory
	@files = sort grep(!/^[\._]/, readdir(DIR)); # filter out "." and ".."

	# Create target subdirectory if there are files to be moved
	if(@files) {
	    mkdir("$lastdir/hdk/$dirname")
		or unix_errexit("Unable to create target last directory $lastdir/hdk/$dirname.");
	}

	closedir(DIR);
	foreach $chunk (@files) {
	    # Move the about-be-overwritten chunk from next to last
	    rename("$nextdir/hdk/$dirname/$chunk", "$lastdir/hdk/$dirname/$chunk")
		or unix_errexit("Unable to move $nextdir/hdk/$dirname/$chunk (next) to $lastdir/hdk/$dirname (last).");
	    
	    # Move the new chunk from cache to next
	    rename("$cachedir/hdk/$dirname/$chunk", "$nextdir/hdk/$dirname/$chunk")
		or unix_errexit("Unable to move $cachedir/hdk/$dirname/$chunk (cache) to $nextdir/hdk/$dirname (next).");
	}
    }
}    

#
# Reset the last link
#
unlink("$parceldir/last")
    or unix_errexit("Unable to remove link $parceldir/last.");
$nextverfilename = sprintf("%06d", $nextver);
symlink($nextverfilename, "$parceldir/last")
    or unix_errexit("Unable to create link $parceldir/last.");

#
# Clean up and exit
#
exit 0;

##################
# end main routine
##################

#
# usage - print help message and terminate
#
sub usage
{
    my $msg = shift;
    my $progname;

    # Strip any path information from the program name
    ($progname = $0) =~ s#.*/##s; 

    if ($msg) {
        print "$progname: $msg\n";
    }

    print "Usage: $progname [-hV] [-u <username>] -p <parcel>\n";
    print "Options:\n";
    print "  -h    Print this message\n";
    print "  -V    Be verbose\n";
    print "  -u    Username for this parcel (default is $ENV{'USER'})\n";
    print "  -p    Parcel name\n";
    print "\n";
    exit 0;
}
