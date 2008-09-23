package vmm;
use File::Spec;
use Exporter qw/import/;
use strict;
use warnings;

BEGIN {
	my @import = (qw/NAME CFGDIR UUID DISK SECTORS MEM FULLSCREEN/,
				qw/SUSPENDED COMMAND/);
	our @EXPORT = qw/main fail find_program run_program $VMNAME/;
	our $VMNAME = "UnknownVMM";
	foreach my $var (@import) {
		push(@EXPORT, "\$$var");
		eval "our \$$var = \$ENV{'$var'}"
			if exists $ENV{$var};
	}
}

sub main {
	our $VMNAME;
	our $SUSPENDED;
	my $msg;

	if (@ARGV and $ARGV[0] eq "info") {
		eval {main::info()};
		if ($@) {
			($msg = <<EOF) =~ s/^\s+//gm;
				VMM=$VMNAME
				RUNNABLE=no
				RUNNABLE_REASON=$@
EOF
		} else {
			($msg = <<EOF) =~ s/^\s+//gm;
				VMM=$VMNAME
				RUNNABLE=yes
EOF
		}
	} elsif (@ARGV and $ARGV[0] eq "run") {
		eval {main::run()};
		if ($@) {
			($msg = <<EOF) =~ s/^\s+//gm;
				SUSPENDED=$SUSPENDED
				SUCCESS=no
				ERROR=$@
EOF
		} else {
			($msg = <<EOF) =~ s/^\s+//gm;
				SUSPENDED=$SUSPENDED
				SUCCESS=yes
EOF
		}
	} else {
		print STDERR "Unknown or no mode specified\n";
		exit 1;
	}
	print $msg;
	exit 0;
}

# Calls die() with newline appended, which prevents file and line information
# from being appended
sub fail {
	my $str = shift;

	die "$str\n";
}

# If $1 is an absolute path and executable, return it.  If it is a relative
# path and executable via PATH, return the absolute path to the executable.
# If no executable is found, return undef.
sub find_program {
	my $prog = shift;

	my $dir;

	return (-x $prog ? $prog : undef)
		if $prog =~ m:^/:;
	foreach $dir (File::Spec->path()) {
		return "$dir/$prog"
			if -x "$dir/$prog";
	}
	return undef;
}

# Wrapper for system() that temporarily redirects stdout to stderr, so that
# the child can't write key-value pairs back to our calling process
sub run_program {
	my $cmd = shift;

	my $ret;
	my $saved;

	open($saved, ">&", *STDOUT)
		or fail "Couldn't dup stdout";
	open(STDOUT, ">&", *STDERR)
		or fail "Couldn't reopen stdout";
	$ret = system($cmd);
	open(STDOUT, ">&", $saved)
		or fail "Couldn't restore stdout";
	close($saved);
	return $ret;
}

1;