// SPDX-License-Identifier: GPL-2.0-only
/*
 * Huawei Kirin 710 family: SNE, POT, MAR.
 *
 * SP805 and logo-buffer addresses are shared by the three supplied DTSes.
 * The early framebuffer configuration has been boot-tested on SNE 6 GiB;
 * POT/MAR firmware display handoff has not been hardware-validated.
 * No DSS or LDI register reprogramming is required on the tested SNE.
 */
#include <board.h>
#include <drivers/framework.h>
#include <lib/simplefb.h>
#include <util.h>

#if (defined(CONFIG_HUAWEI_SNE) + defined(CONFIG_HUAWEI_POT) + \
     defined(CONFIG_HUAWEI_MAR)) != 1
#error "Select exactly one Huawei Kirin 710 board"
#endif

#define K710_SP805_BASE 0xE8A06000UL
#define K710_FB_LOGO_BASE 0x18C00000UL

#if defined(CONFIG_HUAWEI_MAR)
#define K710_FB_HEIGHT 2312
#define K710_BOARD_NAME "huawei-mar"
#elif defined(CONFIG_HUAWEI_POT)
#define K710_FB_HEIGHT 2340
#define K710_BOARD_NAME "huawei-pot"
#else /* CONFIG_HUAWEI_SNE */
#define K710_FB_HEIGHT 2340
#define K710_BOARD_NAME "huawei-sydney"
#endif

static struct video_info k710_fb = {
    .format = FB_FORMAT_ARGB8888,
    .width = 1080,
    .height = K710_FB_HEIGHT,
    .stride = 4,
    .scale = 2,
    .address = (void *)K710_FB_LOGO_BASE,
};

static const struct device k710_devices[] = {
    { "arm,sp805-wdt", (void *)K710_SP805_BASE, "wdt0" },
    { "simplefb", &k710_fb, "fb" },
};

struct board_data board_ops = {
    .name = K710_BOARD_NAME,
    .devices = k710_devices,
    .num_devices = sizeof(k710_devices) / sizeof(k710_devices[0]),
    .quirks = 0,
};
