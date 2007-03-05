#!/bin/sh
#
# mkrevision.sh - generate revision headers from Subversion metadata
#
# Copyright (C) 2006-2007 Carnegie Mellon University
#
# This software is distributed under the terms of the Eclipse Public
# License, Version 1.0 which can be found in the file named LICENSE.Eclipse.
# ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
# RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

set -e

FILETYPE=$1
SUBDIR=$2

if [ -e .svn ] ; then
	VER=`svnversion .`
	BRANCH=`svn info . | egrep "^URL: " | sed -e "s:^.*/svn/openisr/::" \
				-e "s:/${SUBDIR}$::"`
elif [ -d `dirname $0`/.git ] ; then
	VER=`git-describe`
	BRANCH=`git-name-rev HEAD | cut -d\  -f2`
else
	exit 0
fi

# It's better to use a separate object file for the revision data,
# since "svn update" will then force a relink but not a recompile.
# However, we shouldn't do this for shared libraries, because then
# "svn_revision" and "svn_branch" become part of the library's ABI.
case $FILETYPE in
object)
	FILENAME=revision.c
	cat > $FILENAME-new <<- EOF
		const char *svn_revision = "$VER";
		const char *svn_branch = "$BRANCH";
	EOF
	;;
header)
	FILENAME=revision.h
	cat > $FILENAME-new <<- EOF
		#define SVN_REVISION "$VER"
		#define SVN_BRANCH "$BRANCH"
	EOF
	;;
perl)
	FILENAME=IsrRevision.pm
	cat > $FILENAME-new <<- EOF
		package Isr;
		\$SVN_REVISION = "$VER";
		\$SVN_BRANCH = "$BRANCH";
		1;
	EOF
	;;
*)
	echo "Usage: $0 {object|header|perl} <subdir>" >&2
	exit 1
	;;
esac

if [ -f $FILENAME ] && cmp -s $FILENAME $FILENAME-new ; then
	# No need to rebuild if the actual content of $FILENAME
	# hasn't changed
	rm -f $FILENAME-new
else
	mv $FILENAME-new $FILENAME
fi
