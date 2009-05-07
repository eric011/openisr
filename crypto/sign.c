/*
 * libisrcrypto - cryptographic library for the OpenISR (R) system
 *
 * Copyright (C) 2008-2009 Carnegie Mellon University
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License as
 * published by the Free Software Foundation.  A copy of the GNU Lesser General
 * Public License should have been distributed along with this library in the
 * file LICENSE.LGPL.
 *          
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 */

#include <stdlib.h>
#include "isrcrypto.h"
#define LIBISRCRYPTO_INTERNAL
#include "internal.h"

static const struct isrcry_sign_desc *sign_desc(enum isrcry_sign type)
{
	switch (type) {
	}
	return NULL;
}

static int key_type_ok(enum isrcry_key_type type)
{
	switch (type) {
	case ISRCRY_KEY_PUBLIC:
	case ISRCRY_KEY_PRIVATE:
		return 1;
	}
	return 0;
}

static int key_format_ok(enum isrcry_key_format fmt)
{
	switch (fmt) {
	case ISRCRY_KEY_FORMAT_RAW:
		return 1;
	}
	return 0;
}

exported struct isrcry_sign_ctx *isrcry_sign_alloc(enum isrcry_sign type,
			struct isrcry_random_ctx *rand)
{
	struct isrcry_sign_ctx *sctx;

	sctx = malloc(sizeof(*sctx));
	if (sctx == NULL)
		return NULL;
	sctx->desc = sign_desc(type);
	if (sctx->desc == NULL) {
		free(sctx);
		return NULL;
	}
	sctx->rand = rand;
	sctx->pubkey = sctx->privkey = NULL;
	return sctx;
}

exported void isrcry_sign_free(struct isrcry_sign_ctx *sctx)
{
	sctx->desc->free(sctx);
	free(sctx);
}

exported enum isrcry_result isrcry_sign_make_keys(struct isrcry_sign_ctx *sctx,
			unsigned length)
{
	return sctx->desc->make_keys(sctx, length);
}

exported enum isrcry_result isrcry_sign_get_key(struct isrcry_sign_ctx *sctx,
			enum isrcry_key_type type, enum isrcry_key_format fmt,
			void *out, unsigned *outlen)
{
	if (!key_type_ok(type) || !key_format_ok(fmt))
		return ISRCRY_INVALID_ARGUMENT;
	return sctx->desc->get_key(sctx, type, fmt, out, outlen);
}

exported enum isrcry_result isrcry_sign_set_key(struct isrcry_sign_ctx *sctx,
			enum isrcry_key_type type, enum isrcry_key_format fmt,
			void *key, unsigned keylen)
{
	if (!key_type_ok(type) || !key_format_ok(fmt))
		return ISRCRY_INVALID_ARGUMENT;
	return sctx->desc->set_key(sctx, type, fmt, key, keylen);
}

exported enum isrcry_result isrcry_sign_sign(struct isrcry_sign_ctx *sctx,
			void *hash, unsigned hashlen, void *out,
			unsigned *outlen)
{
	return sctx->desc->sign(sctx, hash, hashlen, out, outlen);
}

exported enum isrcry_result isrcry_sign_verify(struct isrcry_sign_ctx *sctx,
			void *hash, unsigned hashlen, void *sig,
			unsigned siglen)
{
	return sctx->desc->verify(sctx, hash, hashlen, sig, siglen);
}