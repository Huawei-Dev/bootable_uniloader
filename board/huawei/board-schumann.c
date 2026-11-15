// SPDX-License-Identifier: GPL-2.0-only
/*
 * Board support for Huawei MediaPad M6 10.8 (Schumann)
 * HiSilicon Kirin 980
 */

#include <board.h>
#include <util.h>
#include <drivers/framework.h>
#include <lib/simplefb.h>
#include <string.h>

/*
 * ARM SP805 watchdog.
 * Base address used by the Kirin980 SP805 watchdog.
 */
#define SCHUMANN_WDT_BASE  0xE8A06000UL

/*
 * Framebuffer initialized by Huawei bootloader.
 * Resolution: 1600 x 2560, RGB565.
 */
#define SCHUMANN_FB_BASE   0x38440050UL

/*
 * Enable LDI frame updates disabled by Huawei firmware.
 *
 * Clear bit 0: allow frame updates.
 * Set bit 0:   enable LDI.
 */
#define SCHUMANN_DSS_BASE          0xE8600000UL
#define SCHUMANN_DSS_LDI0_OFFSET   0x7D000UL

#define SCHUMANN_LDI_CTRL \
	(SCHUMANN_DSS_BASE + SCHUMANN_DSS_LDI0_OFFSET + 0x0024UL)

#define SCHUMANN_LDI_FRM_MSK \
	(SCHUMANN_DSS_BASE + SCHUMANN_DSS_LDI0_OFFSET + 0x0044UL)

static int schumann_early_init(void)
{
	/*
	 * Restore LDI frame updates disabled by Huawei firmware
	 * before handing control to the kernel.
	 *
	 * Clear bit 0: allow frame updates.
	 * Set bit 0:   enable LDI.
	 */
	writel(readl((volatile uint32_t *)SCHUMANN_LDI_FRM_MSK) & ~1U,
	       (void *)SCHUMANN_LDI_FRM_MSK);

	writel(readl((volatile uint32_t *)SCHUMANN_LDI_CTRL) | 1U,
	       (void *)SCHUMANN_LDI_CTRL);

	return 0;
}

static struct video_info schumann_fb = {
	.format  = FB_FORMAT_RGB565,
	.width   = 1600,
	.height  = 2560,
	.stride  = 2,
	.scale   = 2,
	.address = (void *)SCHUMANN_FB_BASE,
};

static const struct device schumann_devices[] = {
	{ "arm,sp805-wdt", (void *)SCHUMANN_WDT_BASE, "wdt0" },
	{ "simplefb", &schumann_fb, "fb" },
};

struct board_data board_ops = {
	.name = "huawei-schumann",
	.ops = {
		.early_init = schumann_early_init,
	},
	.devices = schumann_devices,
	.num_devices = ARRAY_SIZE(schumann_devices),
	.quirks = 0,
};
