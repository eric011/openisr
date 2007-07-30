/*
 * Parcelkeeper - support daemon for the OpenISR (TM) system virtual disk
 *
 * Copyright (C) 2006-2007 Carnegie Mellon University
 *
 * This software is distributed under the terms of the Eclipse Public License,
 * Version 1.0 which can be found in the file named LICENSE.Eclipse.  ANY USE,
 * REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES RECIPIENT'S
 * ACCEPTANCE OF THIS AGREEMENT
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "defs.h"

int is_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return 0;
	return S_ISDIR(st.st_mode);
}

int is_file(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return 0;
	return S_ISREG(st.st_mode);
}

int at_eof(int fd)
{
	off_t cur=lseek(fd, 0, SEEK_CUR);
	if (lseek(fd, 0, SEEK_END) != cur) {
		lseek(fd, cur, SEEK_SET);
		return 0;
	}
	return 1;
}

pk_err_t read_file(const char *path, char *buf, int *bufsize)
{
	int fd;
	int count;
	pk_err_t ret=PK_SUCCESS;

	fd=open(path, O_RDONLY);
	if (fd == -1) {
		switch (errno) {
		case ENOTDIR:
		case ENOENT:
			return PK_NOTFOUND;
		case ENOMEM:
			return PK_NOMEM;
		default:
			return PK_IOERR;
		}
	}
	count=read(fd, buf, *bufsize);
	if (count == -1)
		ret=PK_IOERR;
	else if (count == *bufsize && !at_eof(fd))
		ret=PK_OVERFLOW;
	else
		*bufsize=count;
	close(fd);
	return ret;
}

/* Read a file consisting of a newline-terminated string, and return the string
   without the newline */
pk_err_t read_sysfs_file(const char *path, char *buf, int bufsize)
{
	pk_err_t ret=read_file(path, buf, &bufsize);
	if (ret)
		return ret;
	while (--bufsize >= 0 && buf[bufsize] != '\n');
	if (bufsize < 0)
		return PK_BADFORMAT;
	buf[bufsize]=0;
	return PK_SUCCESS;
}

char *pk_strerror(pk_err_t err)
{
	switch (err) {
	case PK_SUCCESS:
		return "Success";
	case PK_OVERFLOW:
		return "Buffer too small for data";
	case PK_IOERR:
		return "I/O error";
	case PK_NOTFOUND:
		return "Object not found";
	case PK_INVALID:
		return "Invalid parameter";
	case PK_NOMEM:
		return "Out of memory";
	case PK_NOKEY:
		return "No such key in keyring";
	case PK_TAGFAIL:
		return "Tag did not match data";
	case PK_BADFORMAT:
		return "Invalid format";
	case PK_CALLFAIL:
		return "Call failed";
	case PK_PROTOFAIL:
		return "Driver protocol error";
	case PK_NETFAIL:
		return "Network failure";
	case PK_BUSY:
		return "Object busy";
	}
	return "(Unknown)";
}

int set_signal_handler(int sig, void (*handler)(int sig))
{
	struct sigaction sa = {};
	sa.sa_handler=handler;
	sa.sa_flags=SA_RESTART;
	return sigaction(sig, &sa, NULL);
}

void print_progress(unsigned chunks, unsigned maxchunks)
{
	unsigned percent;
	unsigned chunks_per_mb=(1 << 20)/state.chunksize;

	if (maxchunks)
		percent=chunks*100/maxchunks;
	else
		percent=0;
	printf("  %u%% (%u/%u MB)\n", percent, chunks/chunks_per_mb,
				maxchunks/chunks_per_mb);
	/* Move cursor to previous line */
	printf("\x1b[A");
}

/* Create lock file.  flock locks don't work over NFS; byterange locks don't
   work over AFS; and dotlocks are difficult to check for freshness.  So
   we use a whole-file fcntl lock.  The lock shouldn't become stale because the
   kernel checks that for us; however, over NFS file systems without a lock
   manager, locking will fail.  For safety, we treat that as an error. */
pk_err_t acquire_lock(void)
{
	int fd;
	struct flock lock = {
		.l_type   = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start  = 0,
		.l_len    = 0
	};

	fd=open(config.lockfile, O_CREAT|O_WRONLY, 0666);
	if (fd == -1) {
		pk_log(LOG_ERROR, "Couldn't open lock file %s",
					config.lockfile);
		return PK_IOERR;
	}
	if (fcntl(fd, F_SETLK, &lock)) {
		close(fd);
		if (errno == EACCES || errno == EAGAIN)
			return PK_BUSY;
		else
			return PK_CALLFAIL;
	}
	state.lock_fd=fd;
	return PK_SUCCESS;
}

void release_lock(void)
{
	unlink(config.lockfile);
	close(state.lock_fd);
}

pk_err_t create_pidfile(void)
{
	FILE *fp;

	fp=fopen(config.pidfile, "w");
	if (fp == NULL) {
		pk_log(LOG_ERROR, "Couldn't open pid file %s", config.pidfile);
		return PK_IOERR;
	}
	fprintf(fp, "%d\n", getpid());
	fclose(fp);
	return PK_SUCCESS;
}

void remove_pidfile(void)
{
	unlink(config.pidfile);
}

/* Fork, and have the parent wait for the child to indicate that the parent
   should exit.  In the parent, this returns only on error.  In the child, it
   returns success and sets *status_fd.  If the child writes a byte to the fd,
   the parent will exit with that byte as its exit status.  If the child closes
   the fd without writing anything, the parent will exit(0). */
pk_err_t fork_and_wait(int *status_fd)
{
	int fds[2];
	pid_t pid;
	char ret=1;

	/* Make sure the child isn't killed if the parent dies */
	if (set_signal_handler(SIGPIPE, SIG_IGN)) {
		pk_log(LOG_ERROR, "Couldn't block SIGPIPE");
		return PK_CALLFAIL;
	}
	if (pipe(fds)) {
		pk_log(LOG_ERROR, "Can't create pipe");
		return PK_CALLFAIL;
	}

	pid=fork();
	if (pid == -1) {
		pk_log(LOG_ERROR, "fork() failed");
		return PK_CALLFAIL;
	} else if (pid) {
		/* Parent */
		close(fds[1]);
		if (read(fds[0], &ret, sizeof(ret)) == 0)
			exit(0);
		else
			exit(ret);
	} else {
		/* Child */
		close(fds[0]);
		*status_fd=fds[1];
	}
	return PK_SUCCESS;
}