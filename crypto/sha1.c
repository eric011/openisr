/*
 * sha1 - SHA1 hash algorithm
 *
 * Originally from Nettle
 * Modified by Benjamin Gilbert <bgilbert@cs.cmu.edu>
 *
 * Copyright (C) 2001 Peter Gutmann, Andrew Kuchling, Niels Möller
 * Copyright (C) 2006-2008 Carnegie Mellon University
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

#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "isrcrypto.h"
#define LIBISRCRYPTO_INTERNAL
#include "internal.h"

#define SHA1_DATA_SIZE 64

exported void isrcry_sha1_init(struct isrcry_sha1_ctx *ctx)
{
	/* Set the h-vars to their initial values */
	ctx->digest[0] = 0x67452301L;
	ctx->digest[1] = 0xEFCDAB89L;
	ctx->digest[2] = 0x98BADCFEL;
	ctx->digest[3] = 0x10325476L;
	ctx->digest[4] = 0xC3D2E1F0L;
	
	/* Initialize block count */
	ctx->count = 0;
	
	/* Initialize buffer */
	ctx->index = 0;
}

exported void isrcry_sha1_update(struct isrcry_sha1_ctx *ctx,
			const unsigned char *buffer, unsigned length)
{
	if (ctx->index) {
		/* Try to fill partial block */
		unsigned left = SHA1_DATA_SIZE - ctx->index;
		if (length < left) {
			memcpy(ctx->block + ctx->index, buffer, length);
			ctx->index += length;
			return;	/* Finished */
		} else {
			memcpy(ctx->block + ctx->index, buffer, left);
			_isrcry_sha1_compress(ctx->digest, ctx->block);
			ctx->count++;
			buffer += left;
			length -= left;
		}
	}
	while (length >= SHA1_DATA_SIZE) {
		_isrcry_sha1_compress(ctx->digest, buffer);
		ctx->count++;
		buffer += SHA1_DATA_SIZE;
		length -= SHA1_DATA_SIZE;
	}
	if ((ctx->index = length))
		/* Buffer leftovers */
		memcpy(ctx->block, buffer, length);
}

/* Final wrapup - pad to SHA1_DATA_SIZE-byte boundary with the bit pattern
   1 0* (64-bit count of bits processed, MSB-first) */
exported void isrcry_sha1_final(struct isrcry_sha1_ctx *ctx,
			unsigned char *digest)
{
	uint64_t bitcount;
	unsigned i = ctx->index;
	
	/* Set the first char of padding to 0x80.  This is safe since there is
	   always at least one byte free */
	assert(i < SHA1_DATA_SIZE);
	ctx->block[i++] = 0x80;
	
	if (i > (SHA1_DATA_SIZE - 8)) {
		/* No room for length in this block. Process it and
		   pad with another one */
		memset(ctx->block + i, 0, SHA1_DATA_SIZE - i);
		_isrcry_sha1_compress(ctx->digest, ctx->block);
		i = 0;
	}
	if (i < (SHA1_DATA_SIZE - 8))
		memset(ctx->block + i, 0, (SHA1_DATA_SIZE - 8) - i);
	
	/* There are 512 = 2^9 bits in one block */
	bitcount = (ctx->count << 9) | (ctx->index << 3);
	
	/* This is slightly inefficient, as the numbers are converted to
	   big-endian format, and will be converted back by the compression
	   function. It's probably not worth the effort to fix this. */
	STORE32H(bitcount >> 32, ctx->block + (SHA1_DATA_SIZE - 8));
	STORE32H(bitcount, ctx->block + (SHA1_DATA_SIZE - 4));
	
	_isrcry_sha1_compress(ctx->digest, ctx->block);
	
	for (i = 0; i < ISRCRY_SHA1_DIGEST_SIZE / 4; i++, digest += 4)
		STORE32H(ctx->digest[i], digest);
}
