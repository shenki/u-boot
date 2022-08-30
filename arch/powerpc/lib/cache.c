// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2002
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <cpu_func.h>
#include <asm/cache.h>
#include <watchdog.h>

static ulong maybe_watchdog_reset(ulong flushed)
{
	flushed += L1_CACHE_BYTES;
	if (flushed >= CONFIG_CACHE_FLUSH_WATCHDOG_THRESHOLD) {
		WATCHDOG_RESET();
		flushed = 0;
	}
	return flushed;
}

void flush_cache(ulong start_addr, ulong size)
{
	ulong addr, start, end;
	ulong flushed = 0;

	start = start_addr & ~(L1_CACHE_BYTES - 1);
	end = start_addr + size - 1;

	for (addr = start; (addr <= end) && (addr >= start);
			addr += L1_CACHE_BYTES) {
		asm volatile("dcbst 0,%0" : : "r" (addr) : "memory");
		flushed = maybe_watchdog_reset(flushed);
	}
	/* wait for all dcbst to complete on bus */
	asm volatile("sync" : : : "memory");

	for (addr = start; (addr <= end) && (addr >= start);
			addr += L1_CACHE_BYTES) {
		asm volatile("icbi 0,%0" : : "r" (addr) : "memory");
		flushed = maybe_watchdog_reset(flushed);
	}
	asm volatile("sync" : : : "memory");
	/* flush prefetch queue */
	asm volatile("isync" : : : "memory");
}
