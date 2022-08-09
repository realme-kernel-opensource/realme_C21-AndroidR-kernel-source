#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/lz4.h>
#include <crypto/internal/scompress.h>
#include "lz4_asm.h"

struct lz4_asm_ctx {
	void *lz4_comp_mem;
};

static void *lz4_asm_alloc_ctx(struct crypto_scomp *tfm)
{
	void *ctx;

	ctx = vmalloc(LZ4_ASM_MEM_USAGE);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	return ctx;
}

static void lz4_asm_free_ctx(struct crypto_scomp *tfm, void *ctx)
{
	vfree(ctx);
}

static int lz4_asm_init(struct crypto_tfm *tfm)
{
	struct lz4_asm_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->lz4_comp_mem = lz4_asm_alloc_ctx(NULL);
	if (IS_ERR(ctx->lz4_comp_mem))
		return -ENOMEM;

	return 0;
}

static void lz4_asm_exit(struct crypto_tfm *tfm)
{
	struct lz4_asm_ctx *ctx = crypto_tfm_ctx(tfm);

	lz4_asm_free_ctx(NULL, ctx->lz4_comp_mem);
}

static int __lz4_asm_compress_crypto(const u8 *src, unsigned int slen,
				 u8 *dst, unsigned int *dlen, void *ctx)
{
	int	out_len = LZ4_compress_asm(src, dst,
				slen, *dlen, ctx);

	if (!out_len)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4_asm_scompress(struct crypto_scomp *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen,
			 void *ctx)
{
	return __lz4_asm_compress_crypto(src, slen, dst, dlen, ctx);
}

static int lz4_asm_compress_crypto(struct crypto_tfm *tfm, const u8 *src,
			       unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lz4_asm_ctx *ctx = crypto_tfm_ctx(tfm);

	return __lz4_asm_compress_crypto(src, slen, dst, dlen, ctx->lz4_comp_mem);
}

static int __lz4_asm_decompress_crypto(const u8 *src, unsigned int slen,
				   u8 *dst, unsigned int *dlen, void *ctx)
{
	int out_len = LZ4_decompress_asm(src, dst, slen, *dlen);

	if (out_len < 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4_asm_sdecompress(struct crypto_scomp *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen,
			   void *ctx)
{
	return __lz4_asm_decompress_crypto(src, slen, dst, dlen, NULL);
}

static int lz4_asm_decompress_crypto(struct crypto_tfm *tfm, const u8 *src,
				 unsigned int slen, u8 *dst,
				 unsigned int *dlen)
{
	return __lz4_asm_decompress_crypto(src, slen, dst, dlen, NULL);
}

static struct crypto_alg alg_lz4_asm = {
	.cra_name		= "lz4_asm",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct lz4_asm_ctx),
	.cra_module		= THIS_MODULE,
	.cra_list		= LIST_HEAD_INIT(alg_lz4_asm.cra_list),
	.cra_init		= lz4_asm_init,
	.cra_exit		= lz4_asm_exit,
	.cra_u			= { .compress = {
	.coa_compress		= lz4_asm_compress_crypto,
	.coa_decompress		= lz4_asm_decompress_crypto } }
};

static struct scomp_alg scomp_asm = {
	.alloc_ctx		= lz4_asm_alloc_ctx,
	.free_ctx		= lz4_asm_free_ctx,
	.compress		= lz4_asm_scompress,
	.decompress		= lz4_asm_sdecompress,
	.base			= {
		.cra_name	= "lz4_asm",
		.cra_driver_name = "lz4-scomp_asm",
		.cra_module	 = THIS_MODULE,
	}
};

static int __init lz4_asm_mod_init(void)
{
	int ret;

	ret = crypto_register_alg(&alg_lz4_asm);
	if (ret)
		return ret;

	ret = crypto_register_scomp(&scomp_asm);
	if (ret) {
		crypto_unregister_alg(&alg_lz4_asm);
		return ret;
	}

	return ret;
}

static void __exit lz4_asm_mod_fini(void)
{
	crypto_unregister_alg(&alg_lz4_asm);
	crypto_unregister_scomp(&scomp_asm);
}

module_init(lz4_asm_mod_init);
module_exit(lz4_asm_mod_fini);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 ASM Compression Algorithm");
MODULE_ALIAS("lz4_asm");
