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

#ifndef PK_DEFS_H
#define PK_DEFS_H

#include <stdio.h>
#include <uuid.h>
#include <sqlite3.h>

typedef enum pk_err {
	PK_SUCCESS=0,
	PK_OVERFLOW,
	PK_IOERR,
	PK_NOTFOUND,
	PK_INVALID,
	PK_NOMEM,
	PK_NOKEY,
	PK_TAGFAIL,
	PK_BADFORMAT,
	PK_CALLFAIL,
	PK_PROTOFAIL,
	PK_NETFAIL,  /* Used instead of IOERR if a retry might fix it */
	PK_BUSY,
} pk_err_t;

enum pk_log_type {
	LOG_INFO,
	LOG_ERROR,
	LOG_STATS
};

enum cryptotype {
	CRY_UNKNOWN=0,
	CRY_BLOWFISH_SHA1=1,
	CRY_AES_SHA1=2
};

enum compresstype {
	COMP_UNKNOWN=0,
	COMP_NONE=1,
	COMP_ZLIB=2,
	COMP_LZF=3
};

/* pk_getopt() requires this to be a bitmask */
enum mode {
	MODE_RUN      = 0x01,
	MODE_UPLOAD   = 0x02,
	MODE_EXAMINE  = 0x04,
	MODE_VALIDATE = 0x08,
	MODE_HELP     = 0x10,
	MODE_VERSION  = 0x20,
};

struct pk_connection;

struct pk_config {
	/* top-level parcel directory and its contents */
	char *parcel_dir;
	char *parcel_cfg;

	/* cache directory and its contents */
	char *cache_dir;
	char *keyring;
	char *cache_file;
	char *cache_index;
	char *devfile;
	char *lockfile;
	char *pidfile;

	/* last directory and its contents */
	char *last_dir;
	char *last_keyring;

	/* hoard cache and its contents */
	char *hoard_dir;
	char *hoard_file;
	char *hoard_index;

	/* upload directory and its contents */
	char *dest_dir;
	char *dest_stats;

	/* log parameters */
	char *log_file;
	char *log_info_str;
	unsigned log_file_mask;
	unsigned log_stderr_mask;

	/* miscellaneous parameters */
	char *user;
	char *parcel;
	enum compresstype compress;
	int foreground;
};

struct pk_state {
	FILE *log_fp;
	int lock_fd;
	int cache_fd;
	char *loopdev_name;
	int loopdev_fd;
	int chardev_fd;
	int signal_fds[2];
	struct pk_connection *conn;
	sqlite3 *db;

	int bdev_index;

	enum cryptotype crypto;
	unsigned required_compress;
	unsigned chunks;
	unsigned chunksize;
	unsigned chunks_per_dir;
	unsigned hashlen;
	unsigned offset;
	char uuid[UUID_LEN_BIN];
	char *master;

	unsigned request_count;  /* XXX */
};

extern struct pk_config config;
extern struct pk_state state;
extern const char *isr_release;
extern const char *rcs_revision;

#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define _stringify(str) #str
/* the first expansion stringifies; the second expands cpp constants */
#define stringify(str) _stringify(str)

/* cmdline.c */
enum mode parse_cmdline(int argc, char **argv);

/* log.c */
void log_start(void);
void log_shutdown(void);
void _pk_log(enum pk_log_type type, char *fmt, const char *func, ...);
#define pk_log(type, fmt, args...) _pk_log(type, fmt, __func__, ## args)

/* parcelcfg.c */
pk_err_t parse_parcel_cfg(void);

/* cache.c */
pk_err_t cache_init(void);
void cache_shutdown(void);
pk_err_t cache_get(unsigned chunk, void *tag, void *key,
			enum compresstype *compress, unsigned *length);
pk_err_t cache_update(unsigned chunk, const void *tag, const void *key,
			enum compresstype compress, unsigned length);
int copy_for_upload(void);
int validate_keyring(void);
int validate_cache(void);
int examine_cache(void);

/* hoard.c */
pk_err_t hoard_init(void);

/* nexus.c */
pk_err_t nexus_init(void);
void nexus_run(void);
void nexus_shutdown(void);

/* transport.c */
pk_err_t transport_init(void);
void transport_shutdown(void);
pk_err_t transport_get(void *buf, unsigned chunk, size_t *len);

/* sql.c */
int query(sqlite3_stmt **result, sqlite3 *db, char *query, char *fmt, ...);
int query_next(sqlite3_stmt *stmt);
void query_row(sqlite3_stmt *stmt, char *fmt, ...);
void query_free(sqlite3_stmt *stmt);
pk_err_t attach(sqlite3 *db, const char *handle, const char *file);
pk_err_t begin(sqlite3 *db);
pk_err_t commit(sqlite3 *db);
pk_err_t rollback(sqlite3 *db);

/* util.c */
int is_dir(const char *path);
int is_file(const char *path);
int at_eof(int fd);
pk_err_t parseuint(unsigned *out, char *in, int base);
enum cryptotype parse_crypto(char *desc);
enum compresstype parse_compress(char *desc);
unsigned crypto_hashlen(enum cryptotype type);
int compress_is_valid(enum compresstype type);
pk_err_t read_file(const char *path, char *buf, int *bufsize);
pk_err_t read_sysfs_file(const char *path, char *buf, int bufsize);
char *pk_strerror(pk_err_t err);
int set_signal_handler(int sig, void (*handler)(int sig));
void print_progress(unsigned chunks, unsigned maxchunks);
pk_err_t fork_and_wait(int *status_fd);
pk_err_t acquire_lock(void);
void release_lock(void);
pk_err_t create_pidfile(void);
void remove_pidfile(void);
char *form_chunk_path(char *prefix, unsigned chunk);
pk_err_t digest(void *out, const void *in, unsigned len);
char *format_tag(const void *tag);
void log_tag_mismatch(const void *expected, const void *found);

#endif
