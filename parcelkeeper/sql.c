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
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sqlite3.h>
#include "defs.h"

#define CACHE_BUCKETS 200
#define SLOW_THRESHOLD_MS 200

struct query {
	sqlite3_stmt *stmt;
	char *sql;
	struct timeval start;
};

static struct query *prepared[CACHE_BUCKETS];

static void sqlerr(sqlite3 *db)
{
	pk_log(LOG_ERROR, "SQL error: %s", sqlite3_errmsg(db));
}

static unsigned get_bucket(char *sql)
{
	/* DJB string hash algorithm */
	unsigned hash = 5381;

	while (*sql)
		hash = ((hash << 5) + hash) ^ *sql++;
	return hash % CACHE_BUCKETS;
}

static int alloc_query(struct query **result, sqlite3 *db, char *sql)
{
	struct query *qry;
	int ret;

	qry=malloc(sizeof(*qry));
	if (qry == NULL) {
		pk_log(LOG_ERROR, "malloc failed");
		return SQLITE_NOMEM;
	}
	qry->sql=strdup(sql);
	if (qry->sql == NULL) {
		pk_log(LOG_ERROR, "malloc failed");
		free(qry);
		return SQLITE_NOMEM;
	}
	ret=sqlite3_prepare_v2(db, sql, -1, &qry->stmt, NULL);
	if (ret) {
		sqlerr(db);
		free(qry->sql);
		free(qry);
	} else {
		*result=qry;
	}
	return ret;
}

static void destroy_query(struct query *qry)
{
	sqlite3_finalize(qry->stmt);
	free(qry->sql);
	free(qry);
}

static int get_query(struct query **result, sqlite3 *db, char *sql)
{
	unsigned bucket=get_bucket(sql);
	int ret;

	/* XXX when we go to multi-threaded, this will need locking */
	/* XXX also, might need a better hash table */
	if (prepared[bucket] && db == sqlite3_db_handle(prepared[bucket]->stmt)
				&& !strcmp(sql, prepared[bucket]->sql)) {
		*result=prepared[bucket];
		prepared[bucket]=NULL;
		ret=SQLITE_OK;
		state.sql_hits++;
	} else {
		ret=alloc_query(result, db, sql);
		state.sql_misses++;
	}

	if (ret == SQLITE_OK)
		gettimeofday(&(*result)->start, NULL);
	return ret;
}

int query(struct query **result, sqlite3 *db, char *query, char *fmt, ...)
{
	struct query *qry;
	sqlite3_stmt *stmt;
	va_list ap;
	int i=1;
	int ret;
	int found_unknown=0;
	void *blob;

	if (result != NULL)
		*result=NULL;
	ret=get_query(&qry, db, query);
	if (ret)
		return ret;
	stmt=qry->stmt;
	va_start(ap, fmt);
	for (; fmt != NULL && *fmt; fmt++) {
		switch (*fmt) {
		case 'd':
			ret=sqlite3_bind_int(stmt, i++, va_arg(ap, int));
			break;
		case 'f':
			ret=sqlite3_bind_double(stmt, i++, va_arg(ap, double));
			break;
		case 's':
		case 'S':
			ret=sqlite3_bind_text(stmt, i++, va_arg(ap, char *),
						-1, *fmt == 's'
						? SQLITE_TRANSIENT
						: SQLITE_STATIC);
			break;
		case 'b':
		case 'B':
			blob=va_arg(ap, void *);
			ret=sqlite3_bind_blob(stmt, i++, blob, va_arg(ap, int),
						*fmt == 'b' ? SQLITE_TRANSIENT
						: SQLITE_STATIC);
			break;
		default:
			pk_log(LOG_ERROR, "Unknown format specifier %c", *fmt);
			ret=SQLITE_MISUSE;
			/* Don't call sqlerr(), since we synthesized this
			   error */
			found_unknown=1;
			break;
		}
		if (ret)
			break;
	}
	va_end(ap);
	if (ret == SQLITE_OK)
		ret=query_next(qry);
	else if (!found_unknown)
		sqlerr(db);
	if (ret != SQLITE_ROW || result == NULL)
		query_free(qry);
	else
		*result=qry;
	return ret;
}

int query_next(struct query *qry)
{
	int ret;

	ret=sqlite3_step(qry->stmt);
	/* Collapse DONE into OK, since we don't want everyone to have to test
	   for a gratuitously nonzero error code */
	if (ret == SQLITE_DONE)
		ret=SQLITE_OK;
	if (ret != SQLITE_OK && ret != SQLITE_ROW)
		sqlerr(sqlite3_db_handle(qry->stmt));
	return ret;
}

void query_row(struct query *qry, char *fmt, ...)
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
	if (ms >= SLOW_THRESHOLD_MS)
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
}

void sql_shutdown(void)
{
	pk_log(LOG_STATS, "Prepared statement cache: %u hits, %u misses, "
	    			"%u replacements", state.sql_hits,
				state.sql_misses, state.sql_replacements);
	pk_log(LOG_STATS, "Busy handler called for %u queries; longest "
				"wait %u iterations", state.sql_busy_queries,
				state.sql_busy_highwater);
}

pk_err_t attach(sqlite3 *db, const char *handle, const char *file)
{
	if (query(NULL, db, "ATTACH ? AS ?", "ss", file, handle)) {
		pk_log(LOG_ERROR, "Couldn't attach %s", file);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _begin(sqlite3 *db, int immediate, const char *caller)
{
	char *sql = immediate ? "BEGIN IMMEDIATE" : "BEGIN";

	if (query(NULL, db, sql, NULL)) {
		pk_log(LOG_ERROR, "Couldn't begin transaction "
					"on behalf of %s()", caller);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _commit(sqlite3 *db, const char *caller)
{
	if (query(NULL, db, "COMMIT", NULL)) {
		pk_log(LOG_ERROR, "Couldn't commit transaction "
					"on behalf of %s()", caller);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

pk_err_t _rollback(sqlite3 *db, const char *caller)
{
	if (query(NULL, db, "ROLLBACK", NULL)) {
		pk_log(LOG_ERROR, "Couldn't roll back transaction "
					"on behalf of %s()", caller);
		return PK_IOERR;
	}
	return PK_SUCCESS;
}

static int busy_handler(void *db, int count)
{
	int ms;

	(void)db;  /* silence warning */
	if (count == 0) {
		ms=1;
		state.sql_busy_queries++;
	} else if (count <= 2) {
		ms=2;
	} else if (count <= 5) {
		ms=5;
	} else {
		ms=10;
	}
	if ((unsigned)count > state.sql_busy_highwater)
		state.sql_busy_highwater=count;
	usleep(ms * 1000);
	return 1;
}

pk_err_t set_busy_handler(sqlite3 *db)
{
	if (sqlite3_busy_handler(db, busy_handler, db)) {
		pk_log(LOG_ERROR, "Couldn't set busy handler for database");
		return PK_CALLFAIL;
	}
	return PK_SUCCESS;
}

/* This validates both the primary and attached databases */
pk_err_t validate_db(sqlite3 *db)
{
	struct query *qry;
	const char *str;
	int result;

	if (query(&qry, db, "PRAGMA integrity_check(1)", NULL) != SQLITE_ROW) {
		pk_log(LOG_ERROR, "Couldn't run SQLite integrity check");
		return PK_IOERR;
	}
	query_row(qry, "s", &str);
	result=strcmp(str, "ok");
	query_free(qry);
	if (result) {
		pk_log(LOG_ERROR, "SQLite integrity check failed");
		return PK_BADFORMAT;
	}
	return PK_SUCCESS;
}

pk_err_t cleanup_action(sqlite3 *db, char *sql, enum pk_log_type logtype,
			char *desc)
{
	int changes;

	if (query(NULL, db, sql, NULL) != SQLITE_OK) {
		pk_log(LOG_ERROR, "Couldn't clean %s", desc);
		return PK_IOERR;
	}
	changes=sqlite3_changes(db);
	if (changes > 0)
		pk_log(logtype, "Cleaned %d %s", changes, desc);
	return PK_SUCCESS;
}
