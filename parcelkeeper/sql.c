/*
 * Parcelkeeper - support daemon for the OpenISR (R) system virtual disk
 *
 * Copyright (C) 2006-2007 Carnegie Mellon University
 *
 * This software is distributed under the terms of the Eclipse Public License,
 * Version 1.0 which can be found in the file named LICENSE.Eclipse.  ANY USE,
 * REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES RECIPIENT'S
 * ACCEPTANCE OF THIS AGREEMENT
 */

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sqlite3.h>
#include "defs.h"

#define CACHE_BUCKETS 199
#define SLOW_THRESHOLD_MS 200
#define ERRBUFSZ 256
#define MAX_WAIT_USEC 10000
#define PROGRESS_HANDLER_INTERVAL 100000

struct query {
	sqlite3_stmt *stmt;
	const char *sql;
	struct timeval start;
};

static struct query *prepared[CACHE_BUCKETS];
static __thread int result;  /* set by query() and query_next() */
static __thread char errmsg[ERRBUFSZ];

static void sqlerr(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(errmsg, ERRBUFSZ, fmt, ap);
	va_end(ap);
}

static unsigned get_bucket(const char *sql)
{
	/* DJB string hash algorithm */
	unsigned hash = 5381;

	while (*sql)
		hash = ((hash << 5) + hash) ^ *sql++;
	return hash % CACHE_BUCKETS;
}

static int alloc_query(struct query **new_qry, sqlite3 *db, const char *sql)
{
	struct query *qry;
	int ret;

	qry=malloc(sizeof(*qry));
	if (qry == NULL) {
		pk_log(LOG_ERROR, "malloc failed");
		return SQLITE_NOMEM;
	}
	ret=sqlite3_prepare_v2(db, sql, -1, &qry->stmt, NULL);
	if (ret) {
		sqlerr("%s", sqlite3_errmsg(db));
		free(qry);
	} else {
		qry->sql=sqlite3_sql(qry->stmt);
		*new_qry=qry;
	}
	return ret;
}

static void destroy_query(struct query *qry)
{
	sqlite3_finalize(qry->stmt);
	free(qry);
}

static int get_query(struct query **new_qry, sqlite3 *db, const char *sql)
{
	unsigned bucket=get_bucket(sql);
	int ret;

	/* XXX when we go to multi-threaded, this will need locking */
	/* XXX also, might need a better hash table */
	if (prepared[bucket] && db == sqlite3_db_handle(prepared[bucket]->stmt)
				&& !strcmp(sql, prepared[bucket]->sql)) {
		*new_qry=prepared[bucket];
		prepared[bucket]=NULL;
		ret=SQLITE_OK;
		state.sql_hits++;
	} else {
		ret=alloc_query(new_qry, db, sql);
		state.sql_misses++;
	}

	if (ret == SQLITE_OK)
		gettimeofday(&(*new_qry)->start, NULL);
	return ret;
}

pk_err_t query(struct query **new_qry, sqlite3 *db, const char *query,
			const char *fmt, ...)
{
	struct query *qry;
	sqlite3_stmt *stmt;
	va_list ap;
	int i=1;
	int found_unknown=0;
	void *blob;

	if (new_qry != NULL)
		*new_qry=NULL;
	result=get_query(&qry, db, query);
	if (result)
		return PK_SQLERR;
	stmt=qry->stmt;
	va_start(ap, fmt);
	for (; fmt != NULL && *fmt; fmt++) {
		switch (*fmt) {
		case 'd':
			result=sqlite3_bind_int(stmt, i++, va_arg(ap, int));
			break;
		case 'D':
			result=sqlite3_bind_int64(stmt, i++, va_arg(ap,
						int64_t));
			break;
		case 'f':
			result=sqlite3_bind_double(stmt, i++, va_arg(ap,
						double));
			break;
		case 's':
		case 'S':
			result=sqlite3_bind_text(stmt, i++, va_arg(ap, char *),
						-1, *fmt == 's'
						? SQLITE_TRANSIENT
						: SQLITE_STATIC);
			break;
		case 'b':
		case 'B':
			blob=va_arg(ap, void *);
			result=sqlite3_bind_blob(stmt, i++, blob, va_arg(ap,
						int), *fmt == 'b'
						? SQLITE_TRANSIENT
						: SQLITE_STATIC);
			break;
		default:
			sqlerr("Unknown format specifier %c", *fmt);
			result=SQLITE_MISUSE;
			found_unknown=1;
			break;
		}
		if (result)
			break;
	}
	va_end(ap);
	if (result == SQLITE_OK)
		query_next(qry);
	else if (!found_unknown)
		sqlerr("%s", sqlite3_errmsg(db));
	if (result != SQLITE_ROW || new_qry == NULL)
		query_free(qry);
	else
		*new_qry=qry;
	if (result == SQLITE_OK || result == SQLITE_ROW)
		return PK_SUCCESS;
	else
		return PK_SQLERR;
}

pk_err_t query_next(struct query *qry)
{
	if (pending_signal()) {
		/* Try to stop the query.  If this succeeds, the transaction
		   will be automatically rolled back.  Often, though, the
		   attempt will not succeed. */
		sqlite3_interrupt(sqlite3_db_handle(qry->stmt));
	}
	result=sqlite3_step(qry->stmt);
	/* Collapse DONE into OK, since they're semantically equivalent and
	   it simplifies error checking */
	if (result == SQLITE_DONE)
		result=SQLITE_OK;
	/* Collapse IOERR_BLOCKED into BUSY, likewise */
	if (result == SQLITE_IOERR_BLOCKED)
		result=SQLITE_BUSY;
	if (result == SQLITE_OK || result == SQLITE_ROW) {
		return PK_SUCCESS;
	} else {
		sqlerr("%s", sqlite3_errmsg(sqlite3_db_handle(qry->stmt)));
		return PK_SQLERR;
	}
}

int query_result(void)
{
	return result;
}

const char *query_errmsg(void)
{
	return errmsg;
}

void query_row(struct query *qry, const char *fmt, ...)
{
	struct sqlite3_stmt *stmt=qry->stmt;
	va_list ap;
	int i=0;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		switch (*fmt) {
		case 'd':
			*va_arg(ap, int *)=sqlite3_column_int(stmt, i++);
			break;
		case 'D':
			*va_arg(ap, int64_t *)=sqlite3_column_int64(stmt, i++);
			break;
		case 'f':
			*va_arg(ap, double *)=sqlite3_column_double(stmt, i++);
			break;
		case 's':
		case 'S':
			*va_arg(ap, const unsigned char **)=
						sqlite3_column_text(stmt, i);
			if (*fmt == 'S')
				*va_arg(ap, int *)=sqlite3_column_bytes(stmt,
							i);
			i++;
			break;
		case 'b':
			*va_arg(ap, const void **)=sqlite3_column_blob(stmt, i);
			*va_arg(ap, int *)=sqlite3_column_bytes(stmt, i++);
			break;
		case 'n':
			*va_arg(ap, int *)=sqlite3_column_bytes(stmt, i++);
			break;
		default:
			pk_log(LOG_ERROR, "Unknown format specifier %c", *fmt);
			break;
		}
	}
	va_end(ap);
}

void query_free(struct query *qry)
{
	struct timeval cur;
	struct timeval diff;
	unsigned ms;
	unsigned bucket;

	if (qry == NULL)
		return;

	gettimeofday(&cur, NULL);
	timersub(&cur, &qry->start, &diff);
	ms = diff.tv_sec * 1000 + diff.tv_usec / 1000;
	/* COMMIT is frequently slow, but we don't learn anything by logging
	   that, and it clutters up the logs */
	if (ms >= SLOW_THRESHOLD_MS && strcmp(qry->sql, "COMMIT"))
		pk_log(LOG_SLOW_QUERY, "Slow query took %u ms: \"%s\"",
					ms, qry->sql);
	pk_log(LOG_QUERY, "Query took %u ms: \"%s\"", ms, qry->sql);

	sqlite3_reset(qry->stmt);
	sqlite3_clear_bindings(qry->stmt);
	bucket=get_bucket(qry->sql);
	/* XXX locking */
	if (prepared[bucket]) {
		destroy_query(prepared[bucket]);
		state.sql_replacements++;
	}
	prepared[bucket]=qry;
}

void query_flush(void)
{
	int i;

	/* XXX locking */
	for (i=0; i<CACHE_BUCKETS; i++) {
		if (prepared[i]) {
			destroy_query(prepared[i]);
			prepared[i]=NULL;
		}
	}
}

void sql_init(void)
{
	pk_log(LOG_INFO, "Using SQLite %s", sqlite3_version);
	if (strcmp(SQLITE_VERSION, sqlite3_version))
		pk_log(LOG_INFO, "Warning: built against version "
					SQLITE_VERSION);
	srandom(time(NULL));
}

void sql_shutdown(void)
{
	pk_log(LOG_STATS, "Prepared statement cache: %u hits, %u misses, "
	    			"%u replacements", state.sql_hits,
				state.sql_misses, state.sql_replacements);
	pk_log(LOG_STATS, "Busy handler called for %u queries; %u timeouts",
				state.sql_busy_queries,
				state.sql_busy_timeouts);
	pk_log(LOG_STATS, "%u SQL retries; %llu ms spent in backoffs",
				state.sql_retries,
				state.sql_wait_usecs / 1000);
}

static int busy_handler(void *db, int count)
{
	long time;

	(void)db;  /* silence warning */
	if (count == 0)
		state.sql_busy_queries++;
	if (count >= 10) {
		state.sql_busy_timeouts++;
		return 0;
	}
	time=random() % (MAX_WAIT_USEC/2);
	state.sql_wait_usecs += time;
	usleep(time);
	return 1;
}

static int progress_handler(void *db)
{
	(void)db;  /* silence warning */
	return pending_signal();
}

pk_err_t sql_setup_conn(sqlite3 *db)
{
	if (sqlite3_extended_result_codes(db, 1)) {
		pk_log(LOG_ERROR, "Couldn't enable extended result codes "
					"for database");
		return PK_CALLFAIL;
	}
	if (sqlite3_busy_handler(db, busy_handler, db)) {
		pk_log(LOG_ERROR, "Couldn't set busy handler for database");
		return PK_CALLFAIL;
	}
	/* Every so often during long-running queries, check to see if a
	   signal is pending */
	sqlite3_progress_handler(db, PROGRESS_HANDLER_INTERVAL,
				progress_handler, db);
again:
	if (query(NULL, db, "PRAGMA synchronous = NORMAL", NULL)) {
		if (query_retry())
			goto again;
		pk_log_sqlerr("Couldn't set synchronous pragma for database");
		return PK_CALLFAIL;
	}
	return PK_SUCCESS;
}

/* This should not be called inside a transaction, since the whole point of
   sleeping is to do it without locks held */
int query_retry(void)
{
	long time;

	/* ATTACH (and possibly other cases?) sometimes returns SQLITE_ERROR
	   when it means SQLITE_BUSY.  This is non-trivial to fix within
	   SQLite, so we examine the error message as a workaround.  If
	   you find that a locked database results in the errmsg
	   "SQL logic error or missing database" instead of
	   "database is locked", your SQLite does not have a fix for ticket
	   #2371. */
	if (query_busy() || (query_result() == SQLITE_ERROR &&
				!strcmp(query_errmsg(),
				"database is locked"))) {
		/* The SQLite busy handler is not called when SQLITE_BUSY
		   results from a failed attempt to promote a shared
		   lock to reserved.  So we can't just retry after getting
		   SQLITE_BUSY; we have to back off first. */
		time=random() % MAX_WAIT_USEC;
		state.sql_wait_usecs += time;
		usleep(time);
		state.sql_retries++;
		return 1;
	}
	return 0;
}

pk_err_t attach(sqlite3 *db, const char *handle, const char *file)
{
again:
	if (query(NULL, db, "ATTACH ? AS ?", "ss", file, handle)) {
		if (query_retry())
			goto again;
		pk_log_sqlerr("Couldn't attach %s", file);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _begin(sqlite3 *db, const char *caller)
{
again:
	if (query(NULL, db, "BEGIN", NULL)) {
		if (query_busy())
			goto again;
		pk_log_sqlerr("Couldn't begin transaction on behalf of %s()",
					caller);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _commit(sqlite3 *db, const char *caller)
{
again:
	if (query(NULL, db, "COMMIT", NULL)) {
		if (query_busy())
			goto again;
		pk_log_sqlerr("Couldn't commit transaction on behalf of %s()",
					caller);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _rollback(sqlite3 *db, const char *caller)
{
	int saved=result;
	pk_err_t ret=PK_SUCCESS;

again:
	/* SQLITE_INTERRUPT implies that a rollback has already occurred.
	   Try anyway, just to be safe, but don't report an error if we
	   fail. */
	if (query(NULL, db, "ROLLBACK", NULL) && saved != SQLITE_INTERRUPT) {
		if (query_busy())
			goto again;
		pk_log_sqlerr("Couldn't roll back transaction on behalf of "
					"%s()", caller);
		ret=PK_IOERR;
	}
	result=saved;
	return ret;
}

/* This validates both the primary and attached databases */
pk_err_t validate_db(sqlite3 *db)
{
	struct query *qry;
	const char *str;
	int res;

again:
	query(&qry, db, "PRAGMA integrity_check(1)", NULL);
	if (query_retry()) {
		goto again;
	} else if (!query_has_row()) {
		pk_log_sqlerr("Couldn't run SQLite integrity check");
		return PK_IOERR;
	}
	query_row(qry, "s", &str);
	res=strcmp(str, "ok");
	query_free(qry);
	if (res) {
		pk_log(LOG_ERROR, "SQLite integrity check failed");
		return PK_BADFORMAT;
	}
	return PK_SUCCESS;
}

pk_err_t cleanup_action(sqlite3 *db, const char *sql, enum pk_log_type logtype,
			const char *desc)
{
	int changes;

	if (query(NULL, db, sql, NULL)) {
		pk_log_sqlerr("Couldn't clean %s", desc);
		return PK_IOERR;
	}
	changes=sqlite3_changes(db);
	if (changes > 0)
		pk_log(logtype, "Cleaned %d %s", changes, desc);
	return PK_SUCCESS;
}
