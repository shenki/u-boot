/*
 * (C) Copyright ASPEED Technology Inc.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#ifndef ASPEED_HACE_H
#define ASPEED_HACE_H

#define ASPEED_SHA1		0x0
#define ASPEED_SHA224		0x1
#define ASPEED_SHA256		0x2
#define ASPEED_SHA384		0x3
#define ASPEED_SHA512		0x4

#define ASPEED_RSA1024		0x0
#define ASPEED_RSA2048		0x1
#define ASPEED_RSA3072		0x2
#define ASPEED_RSA4096		0x3

#define ASPEED_HACE_BASE		(0x1E6D0000)
#define ASPEED_HACE_SRC			0x00
#define ASPEED_HACE_DEST		0x04
#define ASPEED_HACE_CONTEXT		0x08	/* 8 byte aligned*/
#define ASPEED_HACE_DATA_LEN		0x0C

#define ASPEED_HACE_CMD			0x10
#define  HACE_SHA_BE_EN			BIT(3)
#define  HACE_MD5_LE_EN			BIT(2)
#define  HACE_ALGO_MD5			0
#define  HACE_ALGO_SHA1			BIT(5)
#define  HACE_ALGO_SHA224		BIT(6)
#define  HACE_ALGO_SHA256		(BIT(4) | BIT(6))
#define  HACE_ALGO_SHA512		(BIT(5) | BIT(6))
#define  HACE_ALGO_SHA384		(BIT(5) | BIT(6) | BIT(10))
#define  HACE_SG_EN			BIT(18)

#define ASPEED_HACE_STS			(ASPEED_HACE_BASE + 0x1C)
#define  HACE_RSA_ISR			BIT(13)
#define  HACE_CRYPTO_ISR		BIT(12)
#define  HACE_HASH_ISR			BIT(9)
#define  HACE_RSA_BUSY			BIT(2)
#define  HACE_CRYPTO_BUSY		BIT(1)
#define  HACE_HASH_BUSY			BIT(0)
#define ASPEED_HACE_HASH_SRC		(ASPEED_HACE_BASE + 0x20)
#define ASPEED_HACE_HASH_DIGEST_BUFF	(ASPEED_HACE_BASE + 0x24)
#define ASPEED_HACE_HASH_KEY_BUFF	(ASPEED_HACE_BASE + 0x28)	// 64 byte aligned,g6 16 byte aligned
#define ASPEED_HACE_HASH_DATA_LEN	(ASPEED_HACE_BASE + 0x2C)
#define ASPEED_HACE_HASH_CMD		(ASPEED_HACE_BASE + 0x30)

#define MAX_SG_32				32

struct aspeed_sg_list {
	u32 len;
	u32 phy_addr;
} __packed;

struct aspeed_hash_ctx {
	struct aspeed_sg_list sg_tbl[MAX_SG_32];
	u32 method;
	u32 digest_size;
	u32 len;
	u32 sg_num;
};

#endif /* #ifndef ASPEED_HACE_H */
