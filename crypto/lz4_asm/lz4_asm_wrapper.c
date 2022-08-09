#include <linux/lz4.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>
#include <asm/neon.h>
#include <asm/cputype.h>
#include <linux/slab.h>
#include "lz4_asm.h"

int LZ4_compress_asm(const char *source, char *dest, int inputSize,
	int maxOutputSize, void *workmem)
{
	int ret;
	lz4_hash_t *ht = (lz4_hash_t*)workmem;
	const char *dest_begin, *src_begin;

	dest_begin = dest;
	src_begin = source;

	kernel_neon_begin();
	lz4_encode_asm(&dest, maxOutputSize, &source, src_begin, inputSize, ht->hash_table);
	kernel_neon_end();

	ret = dest - dest_begin;

	return  ret;
}

int LZ4_decompress_asm(const char *source, char *dest,
	int compressedSize, int maxDecompressedSize)
{
	const char * src_begin = source;
	char * dst_begin = dest;
	int ret;

	kernel_neon_begin();
	ret = lz4_decode_asm(&dest, dst_begin, dst_begin + maxDecompressedSize, &source, src_begin + compressedSize);
	kernel_neon_end();
	if (ret)
		return 0;
	return (int)(dest - dst_begin);
}
#ifndef STATIC
EXPORT_SYMBOL(LZ4_compress_asm);
EXPORT_SYMBOL(LZ4_decompress_asm);
#endif
