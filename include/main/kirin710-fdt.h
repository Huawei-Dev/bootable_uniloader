// SPDX-License-Identifier: GPL-2.0-only
#ifndef UNILOADER_KIRIN710_FDT_H
#define UNILOADER_KIRIN710_FDT_H

#include <stdint.h>

#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif

/* FDT is only committed to *dt on a fully validated successful fixup. */
int kirin710_patch_dtb(void **dt, const void *bootloader_fdt);
const char *kirin710_product_name(void);

#endif /* UNILOADER_KIRIN710_FDT_H */
