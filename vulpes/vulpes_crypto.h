/*
                               Fauxide

		      A virtual disk drive tool
 
               Copyright (c) 2002-2004, Intel Corporation
                          All Rights Reserved

This software is distributed under the terms of the Eclipse Public License, 
Version 1.0 which can be found in the file named LICENSE.  ANY USE, 
REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES RECIPIENT'S 
ACCEPTANCE OF THIS AGREEMENT

*/

#ifndef VULPES_CRYPTO
#define VULPES_CRYPTO

#define HASH_LEN 20
#define HASH_LEN_HEX (2 * HASH_LEN + 1)

void digest(const char *mesg, unsigned mesgLen, char *result);
int vulpes_encrypt(const unsigned char *inString, int inStringLength,
		   unsigned char **outString, int *outStringLength,
		   const unsigned char *key, int keyLen, int doPad);
int vulpes_decrypt(const unsigned char *inString, int inStringLength,
		   unsigned char **outString, int *outStringLength,
		   const unsigned char *key, int keyLen, int doPad);

#endif
