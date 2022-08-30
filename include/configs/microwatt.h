/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __MICROWATT_H__
#define __MICROWATT_H__

#define MEM_BASE			0
#define	CONFIG_SYS_SDRAM_BASE		0
#define CONFIG_SYS_MONITOR_BASE		0x0c000000
#define CONFIG_SYS_MONITOR_LEN		0x00080000
#define CONFIG_SYS_INIT_RAM_ADDR	0x0e000000
#define CONFIG_SYS_GBL_DATA_OFFSET	0x01000000

#define CONFIG_BOOTCOMMAND		"run netboot"
#define CONFIG_BOOTFILE 		"dtbImage.microwatt.elf"
#define CONFIG_EXTRA_ENV_SETTINGS                                       \
	"autostart=yes\0"						\
	"netboot=dhcp && bootelf\0"

#endif
