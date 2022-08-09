#ifndef _LZ4_ASM_H_
#define _LZ4_ASM_H_

#include "lz4_constants.h"

typedef struct {
	uint32_t offset;
	uint32_t word;
} lz4_hash_entry_t;

typedef struct {
	lz4_hash_entry_t hash_table[LZ4_COMPRESS_HASH_ENTRIES];
} lz4_hash_t;

#define LZ4_ASM_MEM_USAGE (sizeof(lz4_hash_t))

extern int lz4_decode_asm(char **dst_ptr, char *dst_begin, char *dst_end,
                          const char **src_ptr, const char *src_end);

extern void lz4_encode_asm(char ** dst_ptr,
                       size_t dst_size,
                       const char ** src_ptr,
                       const char * src_begin,
                       size_t src_size,
                       void * hash_table);

#endif
