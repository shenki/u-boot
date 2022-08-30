// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for litex-mmc interface.
 *
 * Mostly borrowed from the Linux driver.
 *
 * Copyright (C) 2019-2020 Antmicro <www.antmicro.com>
 * Copyright 2021 Paul Mackerras, IBM Corp.
 *
 * With contributions from Gabriel Somlo.
 */
#include <common.h>
#include <dm.h>
#include <mmc.h>
#include <clk.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/iopoll.h>
#include <linux/litex.h>

#include <asm/io.h>

#define LITEX_PHY_CARDDETECT  0x00
#define LITEX_PHY_CLOCKERDIV  0x04
#define LITEX_PHY_INITIALIZE  0x08
#define LITEX_PHY_WRITESTATUS 0x0C
#define LITEX_CORE_CMDARG     0x00
#define LITEX_CORE_CMDCMD     0x04
#define LITEX_CORE_CMDSND     0x08
#define LITEX_CORE_CMDRSP     0x0C
#define LITEX_CORE_CMDEVT     0x1C
#define LITEX_CORE_DATEVT     0x20
#define LITEX_CORE_BLKLEN     0x24
#define LITEX_CORE_BLKCNT     0x28
#define LITEX_BLK2MEM_BASE    0x00
#define LITEX_BLK2MEM_LEN     0x08
#define LITEX_BLK2MEM_ENA     0x0C
#define LITEX_BLK2MEM_DONE    0x10
#define LITEX_BLK2MEM_LOOP    0x14
#define LITEX_MEM2BLK_BASE    0x00
#define LITEX_MEM2BLK_LEN     0x08
#define LITEX_MEM2BLK_ENA     0x0C
#define LITEX_MEM2BLK_DONE    0x10
#define LITEX_MEM2BLK_LOOP    0x14
#define LITEX_MEM2BLK         0x18
#define LITEX_IRQ_STATUS      0x00
#define LITEX_IRQ_PENDING     0x04
#define LITEX_IRQ_ENABLE      0x08

#define SD_CTL_DATA_XFER_NONE  0
#define SD_CTL_DATA_XFER_READ  1
#define SD_CTL_DATA_XFER_WRITE 2

#define SD_CTL_RESP_NONE       0
#define SD_CTL_RESP_SHORT      1
#define SD_CTL_RESP_LONG       2
#define SD_CTL_RESP_SHORT_BUSY 3

#define SD_BIT_DONE    BIT(0)
#define SD_BIT_WR_ERR  BIT(1)
#define SD_BIT_TIMEOUT BIT(2)
#define SD_BIT_CRC_ERR BIT(3)

#define SD_SLEEP_US       5
#define SD_TIMEOUT_US 20000

#define SD_OK         0

#define MMC_BUS_WIDTH_4	2

struct litex_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

#define REGS_SDPHY	0
#define REGS_SDCORE	1
#define REGS_SDREADER	2
#define REGS_SDWRITER	3

struct litex_mmc_host {
	struct mmc *mmc;
	struct litex_mmc_plat *plat;
	struct udevice *dev;

	void __iomem *sdphy;
	void __iomem *sdcore;
	void __iomem *sdreader;
	void __iomem *sdwriter;

	char *data;

	unsigned int ref_clk;
	unsigned int sd_clk;

	u32 resp[4];
	u16 rca;

	bool is_bus_width_set;
	bool app_cmd;
};

static int litex_mmc_sdcard_wait_done(void __iomem *reg, struct udevice *dev)
{
	u8 evt;
	int ret;

	ret = readb_poll_timeout(reg, evt, evt & SD_BIT_DONE, SD_TIMEOUT_US);
	if (ret)
		return ret;
	if (evt == SD_BIT_DONE)
		return 0;
	if (evt & SD_BIT_WR_ERR)
		return -EIO;
	if (evt & SD_BIT_TIMEOUT)
		return -ETIMEDOUT;
	if (evt & SD_BIT_CRC_ERR)
		return -EILSEQ;
	dev_err(dev, "%s: unknown error (evt=%x)\n", __func__, evt);
	return -EINVAL;
}

static int litex_mmc_send_cmd(struct litex_mmc_host *host,
			      u8 cmd, u32 arg, u8 response_len, u8 transfer)
{
	struct udevice *dev = host->dev;
	void __iomem *reg;
	int ret;
	u8 evt;

	litex_write32(host->sdcore + LITEX_CORE_CMDARG, arg);
	litex_write32(host->sdcore + LITEX_CORE_CMDCMD,
		      cmd << 8 | transfer << 5 | response_len);
	litex_write8(host->sdcore + LITEX_CORE_CMDSND, 1);

	ret = litex_mmc_sdcard_wait_done(host->sdcore + LITEX_CORE_CMDEVT, dev);
	if (ret) {
		dev_err(dev, "Command (cmd %d) error, status %d\n", cmd, ret);
		return ret;
	}

	if (response_len != SD_CTL_RESP_NONE) {
		/*
		 * NOTE: this matches the semantics of litex_read32()
		 * regardless of underlying arch endianness!
		 */
		memcpy_fromio(host->resp,
			      host->sdcore + LITEX_CORE_CMDRSP, 0x10);
	}

	udelay(10);

	if (!host->app_cmd && cmd == SD_CMD_SEND_RELATIVE_ADDR)
		host->rca = (host->resp[3] >> 16) & 0xffff;

	host->app_cmd = (cmd == MMC_CMD_APP_CMD);

	if (transfer == SD_CTL_DATA_XFER_NONE)
		return ret; /* OK from prior litex_mmc_sdcard_wait_done() */

	ret = litex_mmc_sdcard_wait_done(host->sdcore + LITEX_CORE_DATEVT, dev);
	if (ret) {
		dev_err(dev, "Data xfer (cmd %d) error, status %d\n", cmd, ret);
		return ret;
	}

	/* Wait for completion of (read or write) DMA transfer */
	reg = (transfer == SD_CTL_DATA_XFER_READ) ?
		host->sdreader + LITEX_BLK2MEM_DONE :
		host->sdwriter + LITEX_MEM2BLK_DONE;
	ret = readb_poll_timeout(reg, evt, evt & SD_BIT_DONE, SD_TIMEOUT_US);
	if (ret)
		dev_err(dev, "DMA timeout (cmd %d)\n", cmd);

	return ret;
}

static int litex_mmc_send_app_cmd(struct litex_mmc_host *host)
{
	return litex_mmc_send_cmd(host, MMC_CMD_APP_CMD, host->rca << 16,
				  SD_CTL_RESP_SHORT, SD_CTL_DATA_XFER_NONE);
}

static int litex_mmc_send_set_bus_w_cmd(struct litex_mmc_host *host, u32 width)
{
	return litex_mmc_send_cmd(host, SD_CMD_APP_SET_BUS_WIDTH, width,
				  SD_CTL_RESP_SHORT, SD_CTL_DATA_XFER_NONE);
}

static int litex_mmc_set_bus_width(struct litex_mmc_host *host)
{
	bool app_cmd_sent;
	int ret;

	if (host->is_bus_width_set)
		return 0;

	/* Ensure 'app_cmd' precedes 'app_set_bus_width_cmd' */
	app_cmd_sent = host->app_cmd; /* was preceding command app_cmd? */
	if (!app_cmd_sent) {
		ret = litex_mmc_send_app_cmd(host);
		if (ret)
			return ret;
	}

	/* LiteSDCard only supports 4-bit bus width */
	ret = litex_mmc_send_set_bus_w_cmd(host, MMC_BUS_WIDTH_4);
	if (ret)
		return ret;

	/* Re-send 'app_cmd' if necessary */
	if (app_cmd_sent) {
		ret = litex_mmc_send_app_cmd(host);
		if (ret)
			return ret;
	}

	host->is_bus_width_set = true;

	return 0;
}

static int litex_mmc_get_cd(struct udevice *dev)
{
	struct litex_mmc_host *host = dev_get_priv(dev);
	int ret;

	ret = !litex_read8(host->sdphy + LITEX_PHY_CARDDETECT);
	if (ret)
		return ret;

	/* Ensure bus width will be set (again) upon card (re)insertion */
	host->is_bus_width_set = false;

	return 0;
}

static u32 litex_mmc_response_len(struct mmc_cmd *cmd)
{
	if (cmd->resp_type & MMC_RSP_136)
		return SD_CTL_RESP_LONG;
	if (!(cmd->resp_type & MMC_RSP_PRESENT))
		return SD_CTL_RESP_NONE;
	if (cmd->resp_type & MMC_RSP_BUSY)
		return SD_CTL_RESP_SHORT_BUSY;
	return SD_CTL_RESP_SHORT;
}

static void litex_mmc_do_dma(struct litex_mmc_host *host, struct mmc_data *data,
			     unsigned int *len, u8 *transfer)
{
	struct udevice *dev = mmc_dev(host->mmc);
	dma_addr_t dma;

	/*
	 * We do DMA directly to/from the data buffer.
	 */
	dma = (dma_addr_t) data->dest;
	*len = data->blocksize * data->blocks;
	host->data = data->dest;

	if (data->flags & MMC_DATA_READ) {
		litex_write8(host->sdreader + LITEX_BLK2MEM_ENA, 0);
		litex_write64(host->sdreader + LITEX_BLK2MEM_BASE, dma);
		litex_write32(host->sdreader + LITEX_BLK2MEM_LEN, *len);
		litex_write8(host->sdreader + LITEX_BLK2MEM_ENA, 1);
		*transfer = SD_CTL_DATA_XFER_READ;

	} else if (data->flags & MMC_DATA_WRITE) {
		litex_write8(host->sdwriter + LITEX_MEM2BLK_ENA, 0);
		litex_write64(host->sdwriter + LITEX_MEM2BLK_BASE, dma);
		litex_write32(host->sdwriter + LITEX_MEM2BLK_LEN, *len);
		litex_write8(host->sdwriter + LITEX_MEM2BLK_ENA, 1);
		*transfer = SD_CTL_DATA_XFER_WRITE;
	} else {
		dev_warn(dev, "Data present w/o read or write flag.\n");
		/* Continue: set cmd status, mark req done */
	}

	litex_write16(host->sdcore + LITEX_CORE_BLKLEN, data->blocksize);
	litex_write32(host->sdcore + LITEX_CORE_BLKCNT, data->blocks);
}

static int litex_mmc_request(struct udevice *dev, struct mmc_cmd *cmd,
			      struct mmc_data *data)
{
	struct litex_mmc_host *host = dev_get_priv(dev);
	int status;
	unsigned int length = 0;

	u32 response_len = litex_mmc_response_len(cmd);
	u8 transfer = SD_CTL_DATA_XFER_NONE;

	/* First check that the card is still there */
	if (!litex_mmc_get_cd(dev)) {
		return -ENOMEDIUM;
	}

	host->data = NULL;
	if (data) {
		/*
		 * LiteSDCard only supports 4-bit bus width; therefore, we MUST
		 * inject a SET_BUS_WIDTH (acmd6) before the very first data
		 * transfer, earlier than when the mmc subsystem would normally
		 * get around to it!
		 */
		status = litex_mmc_set_bus_width(host);
		if (status) {
			dev_err(dev, "Can't set bus width!\n");
			return status;
		}

		litex_mmc_do_dma(host, data, &length, &transfer);
	}

	status = litex_mmc_send_cmd(host, cmd->cmdidx, cmd->cmdarg,
			  response_len, transfer);
	host->data = NULL;

	if (status)
		/* Card may be gone; don't assume bus width is still set */
		host->is_bus_width_set = false;

	// It looks strange I know, but it's as it should be
	if (response_len == SD_CTL_RESP_SHORT ||
	    response_len == SD_CTL_RESP_SHORT_BUSY) {
		cmd->response[0] = host->resp[3];
		cmd->response[1] = host->resp[2] & 0xFF;
	} else if (response_len == SD_CTL_RESP_LONG) {
		cmd->response[0] = host->resp[0];
		cmd->response[1] = host->resp[1];
		cmd->response[2] = host->resp[2];
		cmd->response[3] = host->resp[3];
	}

	if (transfer == SD_CTL_DATA_XFER_READ)
		asm volatile ("dcbf 0,%0" : : "r" (data->dest));

	return status;
}

static void litex_mmc_setclk(struct litex_mmc_host *host, unsigned int freq)
{
	struct udevice *dev = mmc_dev(host->mmc);
	u32 div;

	div = freq ? host->ref_clk / freq : 256U;
	div = roundup_pow_of_two(div);
	div = clamp(div, 2U, 256U);
	dev_dbg(dev, "sd_clk_freq=%d: set to %d via div=%d\n",
		freq, host->ref_clk / div, div);
	litex_write16(host->sdphy + LITEX_PHY_CLOCKERDIV, div);
	host->sd_clk = freq;
}

static int litex_mmc_set_ios(struct udevice *dev)
{
	struct litex_mmc_host *host = dev_get_priv(dev);
	struct mmc *mmc = mmc_get_mmc_dev(dev);

	/*
	 * NOTE: Ignore any ios->bus_width updates; they occur right after
	 * the mmc core sends its own acmd6 bus-width change notification,
	 * which is redundant since we snoop on the command flow and inject
	 * an early acmd6 before the first data transfer command is sent!
	 */

	/* Update sd_clk */
	if (mmc->clock != host->sd_clk)
		litex_mmc_setclk(host, mmc->clock);

	return 0;
}

static int litex_mmc_probe(struct udevice *dev)
{
	struct litex_mmc_plat *plat = dev_get_plat(dev);
	struct litex_mmc_host *host = dev_get_priv(dev);
	struct mmc *mmc = mmc_get_mmc_dev(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	unsigned long phys_addr;
	unsigned long i;
	void __iomem *regs[4];
	struct clk *clk;


	host->dev = dev;
	host->mmc = mmc;
	host->plat = plat;
	upriv->mmc = &plat->mmc;
	plat->cfg.name = dev->name;

	/* Initialize clock source */
	clk = devm_clk_get(dev, NULL);
	host->ref_clk = clk_get_rate(clk);
	/* Fallback for old device tree */
	if (IS_ERR(clk))
		host->ref_clk = 100000000;
	host->sd_clk = 0;

	/*
	 * LiteSDCard only supports 4-bit bus width; therefore, we MUST inject
	 * a SET_BUS_WIDTH (acmd6) before the very first data transfer, earlier
	 * than when the mmc subsystem would normally get around to it!
	 */
	host->is_bus_width_set = false;
	host->app_cmd = false;

	for (i = 0; i < 4; ++i) {
		phys_addr = dev_read_addr_index(dev, i);
		if (phys_addr == FDT_ADDR_T_NONE)
			return -EINVAL;
		regs[i] = (void __iomem *) phys_addr;
	}
	host->sdphy = regs[REGS_SDPHY];
	host->sdcore = regs[REGS_SDCORE];
	host->sdreader = regs[REGS_SDREADER];
	host->sdwriter = regs[REGS_SDWRITER];

	/* Ensure DMA bus masters are disabled */
	litex_write8(host->sdreader + LITEX_BLK2MEM_ENA, 0);
	litex_write8(host->sdwriter + LITEX_MEM2BLK_ENA, 0);

	/*
	 * Set default sd_clk frequency range based on empirical observations
	 * of LiteSDCard gateware behavior on typical SDCard media
	 */
	plat->cfg.f_max = host->ref_clk / 2;
	plat->cfg.f_min = host->ref_clk / 512;
	plat->cfg.b_max = 65535;

	plat->cfg.host_caps = MMC_MODE_4BIT  | MMC_CAP_NEEDS_POLL;
	if (plat->cfg.f_max > 25000000)
		plat->cfg.host_caps |= MMC_MODE_HS;

	plat->cfg.voltages = MMC_VDD_32_33 | MMC_VDD_33_34;

	return 0;
}

static const struct udevice_id litex_mmc_match[] = {
	{ .compatible = "litex,mmc" },
	{ }
};

static const struct dm_mmc_ops litex_mmc_ops = {
	.send_cmd = litex_mmc_request,
	.set_ios = litex_mmc_set_ios,
	.get_cd = litex_mmc_get_cd,
};

static int litex_mmc_bind(struct udevice *dev)
{
	struct litex_mmc_plat *plat = dev_get_plat(dev);

	return mmc_bind(dev, &plat->mmc, &plat->cfg);
}

U_BOOT_DRIVER(litex_mmc_host) = {
	.name = "litex-mmc-host",
	.id = UCLASS_MMC,
	.of_match = litex_mmc_match,
	.bind = litex_mmc_bind,
	.probe = litex_mmc_probe,
	.priv_auto = sizeof(struct litex_mmc_host),
	.plat_auto = sizeof(struct litex_mmc_plat),
	.ops = &litex_mmc_ops,
};
