// SPDX-License-Identifier: GPL-2.0-only
/*
 * Huawei Everest: adapt an embedded EMUI10 DTB using bootloader runtime facts.
 * Ported from the previously tested kernel-side FDT fixups v4.
 * First stage: one board only (ever_tugl_vh), no DTB catalog selection.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <lib/debug.h>
#include <lib/libfdt/libfdt.h>
#include <main/everest-fdt.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define EVR_MAX_SOURCE_FDT_BYTES 2097152U
#define EVR_MAX_CATALOG_FDT_BYTES (16U * 1024U * 1024U)
#define EVR_BOOTARGS_MAX 4096

static uint8_t evr_runtime_fdt[CONFIG_FDT_BUF_SIZE] __attribute__((aligned(8)));
static char evr_bootargs_buf[EVR_BOOTARGS_MAX];

/* A short, persistent label from the original Huawei firmware FDT.
 * A neutral label is used only if firmware identity cannot be displayed.
 * Never retain pointers into a mutable FDT property. */
#define EVR_PRODUCT_NAME_MAX 64
static char evr_product_name[EVR_PRODUCT_NAME_MAX] = "EVR-UNKNOWN";

const char *everest_product_name(void)
{
    return evr_product_name;
}

/*
 * Huawei pre-kernel firmware may enforce strict alignment (SCTLR_EL1.A).
 * FDT properties are only 4-byte aligned; uniLoader's ARM64 memcpy.S uses
 * potentially unaligned 64-bit loads for copies of 8..15 bytes.
 * Use explicit byte accesses throughout this Everest-only FDT adapter.
 */
static void evr_fdt_copy_bytes(void *dst, const void *src, size_t size)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;

    while (size--) {
        *d++ = *s++;
    }
}

static bool evr_fdt_single_string(const char *value, int len)
{
	return value && len > 1 && value[len - 1] == '\0' &&
		strnlen(value, len) == (size_t)(len - 1);
}

/* The label is printed on the framebuffer: reject control characters and
 * overly long strings, but do not change FDT validation or boot behavior. */
static bool evr_fdt_safe_product_label(const char *value, int len)
{
    int i;

    if (!evr_fdt_single_string(value, len) ||
        len > (int)sizeof(evr_product_name))
        return false;

    for (i = 0; i < len - 1; i++) {
        unsigned char ch = ((const volatile unsigned char *)value)[i];

        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_'))
            return false;
    }
    return true;
}

static int evr_fdt_copy_prop(void *dst, const void *src,
				   const char *dst_path,
				   const char *src_path,
				   const char *name)
{
	int src_node, dst_node, len;
	const void *value;

	src_node = fdt_path_offset(src, src_path);
	dst_node = fdt_path_offset(dst, dst_path);
	if (src_node < 0 || dst_node < 0)
		return -FDT_ERR_NOTFOUND;

	value = fdt_getprop(src, src_node, name, &len);
	if (!value || len < 0)
		return -FDT_ERR_NOTFOUND;

	return fdt_setprop(dst, dst_node, name, value, len);
}

/*
 * The 4-cell board ID is the authoritative selection key.  Product names,
 * regional labels and modem IDs can legitimately change between firmware
 * generations, so their mismatch is reported, not used to switch boards.
 * Do not restrict device identity to a hard-coded model name or ID prefix.
 */
static int evr_fdt_validate_and_copy_identity(void *dst,
						const void *src)
{
	static const char * const names[] = {
		"hisi,boardname", "hisi,product_id", "hisi,product_name",
		"hisi,modem_id", "hisi,boardid"
	};
	int s, d, slen, dlen, ret;
	const void *sv, *dv;
	const fdt32_t *sid, *did;
	unsigned char copy[128];
	size_t i;

	s = fdt_path_offset(src, "/rootparam");
	d = fdt_path_offset(dst, "/rootparam");
	if (s < 0 || d < 0)
		return -FDT_ERR_NOTFOUND;

	sid = fdt_getprop(src, s, "hisi,boardid", &slen);
	did = fdt_getprop(dst, d, "hisi,boardid", &dlen);
	if (!sid || !did || slen != 4 * sizeof(*sid) ||
	    dlen != 4 * sizeof(*did) || memcmp(sid, did, slen)) {
		printk(KERN_CRIT, "Everest FDT: boardid absent, malformed or different\n");
		return -FDT_ERR_BADVALUE;
	}

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		sv = fdt_getprop(src, s, names[i], &slen);
		dv = fdt_getprop(dst, d, names[i], &dlen);
		if (!sv || !dv)
			return -FDT_ERR_BADVALUE;

		if (i == 0 || i == 2) {
			if (!evr_fdt_single_string(sv, slen) ||
			    !evr_fdt_single_string(dv, dlen))
				return -FDT_ERR_BADVALUE;
		} else if (i == 1 || i == 3) {
			if (slen != sizeof(fdt32_t) ||
			    dlen != sizeof(fdt32_t))
				return -FDT_ERR_BADVALUE;
		} else if (slen != 4 * sizeof(fdt32_t) ||
			   dlen != 4 * sizeof(fdt32_t)) {
			return -FDT_ERR_BADVALUE;
		}

		/* Product name comes from the original firmware, not the
		 * selected EMUI10 catalog member.  Save before FDT mutation. */
		if (i == 2 && evr_fdt_safe_product_label(sv, slen))
			evr_fdt_copy_bytes(evr_product_name, sv, (size_t)slen);

		if (slen != dlen || memcmp(sv, dv, slen)) {
			if (i == 4)
				return -FDT_ERR_BADVALUE;
			printk(KERN_WARNING, "Everest FDT: %s differs between BL and EMUI10 (boardid matches)\n",
				names[i]);
		}
	}

	/* Keep / and /rootparam consistent with the selected EMUI10 DTB.
	 * Copy via scratch storage: fdt_setprop() may relocate the source
	 * data if it lives inside the very same FDT being modified.
	 */
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		int root;
		root = fdt_path_offset(dst, "/");

		d = fdt_path_offset(dst, "/rootparam");
		if (root < 0 || d < 0)
			return -FDT_ERR_NOTFOUND;
		dv = fdt_getprop(dst, d, names[i], &dlen);
		if (!dv || dlen < 1 || dlen > (int)sizeof(copy))
			return -FDT_ERR_BADVALUE;
		evr_fdt_copy_bytes(copy, dv, dlen);
		ret = fdt_setprop(dst, root, names[i], copy, dlen);
		if (ret)
			return ret;
	}

	printk(KERN_NOTICE, "[%s:01] OK  Board ID validated\n", everest_product_name());
	return 0;
}

/* Full compatible string-list parser: rejects malformed trailing strings. */
static int evr_fdt_compatible_match(const char *compatible, int len,
					  const char *wanted, int wanted_len)
{
	bool found = false;

	if (!compatible || len <= 0 || !wanted || wanted_len <= 0)
		return -FDT_ERR_BADVALUE;

	while (len > 0) {
		size_t n = strnlen(compatible, len);

		if (n >= (size_t)len || !n)
			return -FDT_ERR_BADVALUE;
		if (n == (size_t)wanted_len &&
		    !memcmp(compatible, wanted, n))
			found = true;
		compatible += n + 1;
		len -= n + 1;
	}
	return found ? 1 : 0;
}

/* 0 = disabled, 1 = enabled, negative = malformed DT status. */
static int evr_fdt_panel_enabled(const char *status, int len)
{
	if (!status)
		return len == -FDT_ERR_NOTFOUND ? 1 : -FDT_ERR_BADVALUE;
	if (!evr_fdt_single_string(status, len))
		return -FDT_ERR_BADVALUE;
	if (!strcmp(status, "disabled") || !strcmp(status, "fail") ||
	    !strncmp(status, "fail-", 5))
		return 0;
	if (!strcmp(status, "ok") || !strcmp(status, "okay"))
		return 1;
	return -FDT_ERR_BADVALUE;
}

#define EVR_LCD_NAME_MAX 128
#define EVR_LCD_TYPE_MAX 128

/*
 * The bootloader knows which physical LCD is installed.  Find its one
 * enabled EMUI9.1 lcd_config child, and confirm lcd_panel_type refers
 * to THAT child within the SAME bootloader FDT.  Use its stable
 * lcd-kit,panel-name as the cross-firmware identity.
 *
 * We never use a stock EMUI9.1 compatible to select an EMUI10 node.
 */
static int evr_fdt_bootloader_panel_name(const void *src,
						char *saved_name,
						int saved_size)
{
	int cfg, panel, child, type_len, len, status_len, matches = 0;
	const char *type, *compat, *name, *status;
	int enabled, comp_len, ret;

	panel = fdt_path_offset(src, "/huawei,lcd_panel");
	cfg = fdt_path_offset(src, "/huawei,lcd_config");
	if (panel < 0 || cfg < 0)
		return -FDT_ERR_NOTFOUND;

	type = fdt_getprop(src, panel, "lcd_panel_type", &type_len);
	if (!evr_fdt_single_string(type, type_len) ||
	    type_len > EVR_LCD_TYPE_MAX)
		return -FDT_ERR_BADVALUE;

	for (child = fdt_first_subnode(src, cfg);
	     child >= 0; child = fdt_next_subnode(src, child)) {
		status = fdt_getprop(src, child, "status", &status_len);
		enabled = evr_fdt_panel_enabled(status, status_len);
		if (enabled < 0)
			return enabled;
		if (!enabled)
			continue;
		matches++;
		/* Exactly one enabled source LCD must match its own type. */
		compat = fdt_getprop(src, child, "compatible", &comp_len);
		ret = evr_fdt_compatible_match(compat, comp_len,
					     type, type_len - 1);
		if (ret != 1)
			return ret < 0 ? ret : -FDT_ERR_BADVALUE;

		name = fdt_getprop(src, child, "lcd-kit,panel-name", &len);
		if (!evr_fdt_single_string(name, len) || len > saved_size)
			return -FDT_ERR_BADVALUE;
		evr_fdt_copy_bytes(saved_name, name, len);
	}
	if (child != -FDT_ERR_NOTFOUND)
		return child;
	if (matches != 1) {
		printk(KERN_CRIT, "Everest FDT: bootloader enabled LCD count=%d (expected 1)\n",
			matches);
		return -FDT_ERR_BADVALUE;
	}
	return 0;
}

/*
 * Inspect the selected EMUI10 DTB, but do NOT reuse old node offsets
 * after fdt_setprop(): the blob may be relocated inside its buffer.
 * Return the unique panel offset only BEFORE mutation.  On the final
 * pass verify that it is the only enabled LCD node.
 */
static int evr_fdt_scan_emui10_panels(const void *dst,
						const char *wanted_name,
						bool after_fixup,
						int *selected,
						char *selected_type,
						int selected_size)
{
	int cfg, child, nmatch = 0, nenabled = 0;
	int len, status_len, compat_len, enabled, ret;
	const char *name, *status, *compatible;

	cfg = fdt_path_offset(dst, "/huawei,lcd_config");
	if (cfg < 0)
		return -FDT_ERR_NOTFOUND;

	for (child = fdt_first_subnode(dst, cfg);
	     child >= 0; child = fdt_next_subnode(dst, child)) {
		bool match = false;

		name = fdt_getprop(dst, child, "lcd-kit,panel-name", &len);
		if (name) {
			if (!evr_fdt_single_string(name, len))
				return -FDT_ERR_BADVALUE;
			match = !strcmp(name, wanted_name);
		} else if (len != -FDT_ERR_NOTFOUND) {
			return -FDT_ERR_BADVALUE;
		}

		status = fdt_getprop(dst, child, "status", &status_len);
		enabled = evr_fdt_panel_enabled(status, status_len);
		if (enabled < 0)
			return enabled;
		nenabled += enabled;
		if (enabled && !match)
			return -FDT_ERR_BADSTATE;

		if (!match)
			continue;
		nmatch++;
		if (selected)
			*selected = child;

		compatible = fdt_getprop(dst, child, "compatible", &compat_len);
		if (!compatible || compat_len <= 1)
			return -FDT_ERR_BADVALUE;
		len = strnlen(compatible, compat_len);
		if (!len || len >= compat_len || len + 1 > selected_size)
			return -FDT_ERR_BADVALUE;
		/* Validate the WHOLE compatible string-list. */
		ret = evr_fdt_compatible_match(compatible, compat_len,
					     compatible, len);
		if (ret != 1)
			return -FDT_ERR_BADVALUE;
		if (selected_type)
			evr_fdt_copy_bytes(selected_type, compatible, len + 1);
	}
	if (child != -FDT_ERR_NOTFOUND)
		return child;
	if (nmatch != 1 || (after_fixup && nenabled != 1)) {
		printk(KERN_CRIT, "Everest FDT: panel-name '%s' matches=%d enabled=%d\n",
			wanted_name, nmatch, nenabled);
		return -FDT_ERR_BADVALUE;
	}
	return 0;
}

static int evr_fdt_select_panel(void *dst, const void *src)
{
	char panel_name[EVR_LCD_NAME_MAX];
	char emui10_compatible[EVR_LCD_TYPE_MAX];
	char verified_compatible[EVR_LCD_TYPE_MAX];
	int selected = -1, dst_panel, ret, len;
	const char *type;

	ret = evr_fdt_bootloader_panel_name(src, panel_name,
					      sizeof(panel_name));
	if (ret)
		return ret;

	dst_panel = fdt_path_offset(dst, "/huawei,lcd_panel");
	if (dst_panel < 0)
		return -FDT_ERR_NOTFOUND;

	ret = evr_fdt_scan_emui10_panels(dst, panel_name, false, &selected,
						emui10_compatible,
						sizeof(emui10_compatible));
	if (ret || selected < 0)
		return ret ? ret : -FDT_ERR_BADVALUE;

	/* The enabled node is an EMUI10 node, not a transplanted EMUI9 node. */
	ret = fdt_setprop(dst, selected, "status", "ok", sizeof("ok"));
	if (ret)
		return ret;

	/* Driver expects the selected EMUI10 panel's own compatible. */
	dst_panel = fdt_path_offset(dst, "/huawei,lcd_panel");
	if (dst_panel < 0)
		return -FDT_ERR_NOTFOUND;
	ret = fdt_setprop(dst, dst_panel, "lcd_panel_type",
			  emui10_compatible, strlen(emui10_compatible) + 1);
	if (ret)
		return ret;

	/* Re-scan AFTER writes; neither offsets nor pointers are reusable. */
	ret = evr_fdt_scan_emui10_panels(dst, panel_name, true, NULL,
						verified_compatible,
						sizeof(verified_compatible));
	if (ret || strcmp(verified_compatible, emui10_compatible))
		return ret ? ret : -FDT_ERR_BADVALUE;
	dst_panel = fdt_path_offset(dst, "/huawei,lcd_panel");
	type = fdt_getprop(dst, dst_panel, "lcd_panel_type", &len);
	if (!evr_fdt_single_string(type, len) ||
	    strcmp(type, emui10_compatible))
		return -FDT_ERR_BADVALUE;

	printk(KERN_NOTICE, "[%s:02] OK  LCD panel matched\n", everest_product_name());
	return 0;
}

static int evr_fdt_validate_memory(const void *src, const void *dst)
{
	const void *tree[2] = { src, dst };
	const fdt32_t *cells, *reg;
	uint64_t base[8], size[8];
	int i, j, k, root, mem, len, count;

	/* Both trees must use the same 64-bit address + 64-bit size encoding. */
	for (k = 0; k < 2; k++) {
		root = fdt_path_offset(tree[k], "/");
		if (root < 0)
			return -FDT_ERR_NOTFOUND;
		cells = fdt_getprop(tree[k], root, "#address-cells", &len);
		if (!cells || len != sizeof(fdt32_t) || fdt32_to_cpu(*cells) != 2)
			return -FDT_ERR_BADVALUE;
		cells = fdt_getprop(tree[k], root, "#size-cells", &len);
		if (!cells || len != sizeof(fdt32_t) || fdt32_to_cpu(*cells) != 2)
			return -FDT_ERR_BADVALUE;
	}

	mem = fdt_path_offset(src, "/memory@0");
	if (mem < 0 || fdt_path_offset(dst, "/memory@0") < 0)
		return -FDT_ERR_NOTFOUND;
	reg = fdt_getprop(src, mem, "reg", &len);
	if (!reg || len < 16 || len > 128 || (len % 16))
		return -FDT_ERR_BADVALUE;
	count = len / 16;
	for (i = 0; i < count; i++) {
		base[i] = ((uint64_t)fdt32_to_cpu(reg[i * 4]) << 32) |
			fdt32_to_cpu(reg[i * 4 + 1]);
		size[i] = ((uint64_t)fdt32_to_cpu(reg[i * 4 + 2]) << 32) |
			fdt32_to_cpu(reg[i * 4 + 3]);
		if (!size[i] || base[i] > (~(uint64_t)0) - size[i] ||
		    (base[i] & 0xfff) || (size[i] & 0xfff))
			return -FDT_ERR_BADVALUE;
		for (j = 0; j < i; j++) {
			if (base[i] < base[j] + size[j] &&
			    base[j] < base[i] + size[i])
				return -FDT_ERR_BADVALUE;
		}
	}
	return 0;
}

static int evr_fdt_copy_validated_status(void *dst, const void *src,
						 const char *path)
{
	int node, len, enabled;
	const char *status;

	node = fdt_path_offset(src, path);
	if (node < 0)
		return -FDT_ERR_NOTFOUND;
	status = fdt_getprop(src, node, "status", &len);
	if (!status)
		return -FDT_ERR_BADVALUE;
	enabled = evr_fdt_panel_enabled(status, len);
	if (enabled < 0)
		return enabled;
	return evr_fdt_copy_prop(dst, src, path, path, "status");
}

static int evr_fdt_enable_hota(void *dst)
{
	int node, len;
	const fdt32_t *reg;
	const void *nomap;

	node = fdt_path_offset(dst, "/reserved-memory/hota");
	if (node < 0)
		return -FDT_ERR_NOTFOUND;
	reg = fdt_getprop(dst, node, "reg", &len);
	if (!reg || len != 4 * sizeof(*reg) ||
	    fdt32_to_cpu(reg[0]) != 0 ||
	    fdt32_to_cpu(reg[1]) != 0xaa200000 ||
	    fdt32_to_cpu(reg[2]) != 0 ||
	    fdt32_to_cpu(reg[3]) != 0x10000000)
		return -FDT_ERR_BADVALUE;

	nomap = fdt_getprop(dst, node, "no-map", &len);
	if (!nomap || len != 0)
		return -FDT_ERR_BADVALUE;
	return fdt_setprop(dst, node, "status", "ok", sizeof("ok"));
}

static int huawei_everest_adapt_fdt(void *dst, const void *src)
{
	int ret, node, len;
	const void *value;

	if (!src || !dst || fdt_check_header(src) || fdt_check_header(dst))
		return -FDT_ERR_BADVALUE;

	ret = evr_fdt_validate_and_copy_identity(dst, src);
	if (ret)
		return ret;

	ret = evr_fdt_select_panel(dst, src);
	if (ret)
		return ret;

	ret = evr_fdt_validate_memory(src, dst);
	if (ret)
		return ret;
	ret = evr_fdt_copy_prop(dst, src, "/memory@0", "/memory@0", "reg");
	if (ret)
		return ret;

	ret = evr_fdt_copy_validated_status(dst, src,
					"/reserved-memory/logo-buffer");
	if (ret)
		return ret;

	node = fdt_path_offset(src, "/framebuffer@E8600000");
	if (node < 0)
		return -FDT_ERR_NOTFOUND;
	value = fdt_getprop(src, node, "fastboot_enable_flag", &len);
	if (!value || len != sizeof(fdt32_t) ||
	    fdt32_to_cpu(*(const fdt32_t *)value) > 1)
		return -FDT_ERR_BADVALUE;
	ret = evr_fdt_copy_prop(dst, src, "/framebuffer@E8600000",
				"/framebuffer@E8600000", "fastboot_enable_flag");
	if (ret)
		return ret;

	node = fdt_path_offset(src, "/psci");
	if (node < 0)
		return -FDT_ERR_NOTFOUND;
	value = fdt_getprop(src, node, "method", &len);
	if (!evr_fdt_single_string(value, len) ||
	    (strcmp(value, "hvc") && strcmp(value, "smc")))
		return -FDT_ERR_BADVALUE;
	ret = evr_fdt_copy_prop(dst, src, "/psci", "/psci", "method");
	if (ret)
		return ret;

	/* Always apply the tested Kirin980 mixed-firmware HOTA reservation. */
	ret = evr_fdt_enable_hota(dst);
	if (ret)
		return ret;

	printk(KERN_NOTICE, "[%s:03] OK  RAM / HOTA validated\n", everest_product_name());
	return 0;
}

/*
 * The embedded EMUI10 DTB owns every hardware node.  Only /chosen is
 * replaced in full from the original Huawei firmware FDT, including any
 * properties or child nodes unknown to an older kernel-side setup.c.
 * No hardware node outside /chosen is created or imported.
 *
 * The original /chosen node must exist in both trees.  Do not keep stale
 * destination properties just because the firmware did not supply them.
 */
#define EVR_CHOSEN_PATH_MAX 1024
static char evr_chosen_path[EVR_CHOSEN_PATH_MAX];

static int evr_copy_chosen_node_props(void *dst, const void *src,
                                      int src_node, const char *dst_path,
                                      unsigned int *copied)
{
    int prop, next, len, dst_node, ret;
    const char *name;
    const void *value;

    for (prop = fdt_first_property_offset(src, src_node);
         prop >= 0; prop = next) {
        /* Never keep offsets into the destination across a mutation. */
        value = fdt_getprop_by_offset(src, prop, &name, &len);
        if (!value || !name || len < 0)
            return -FDT_ERR_BADSTRUCTURE;

        next = fdt_next_property_offset(src, prop);
        if (next < 0 && next != -FDT_ERR_NOTFOUND)
            return next;

        dst_node = fdt_path_offset(dst, dst_path);
        if (dst_node < 0)
            return dst_node;
        ret = fdt_setprop(dst, dst_node, name, value, len);
        if (ret)
            return ret;
        ++*copied;
    }
    return prop == -FDT_ERR_NOTFOUND ? 0 : prop;
}

static int evr_replace_chosen(void *dst, const void *src)
{
    int src_chosen, dst_chosen, prop, child, ret, depth, node, next;
    int parent;
    const char *name;
    const void *value;
    unsigned int props = 0, children = 0;
    size_t name_len;
    char *slash;

    src_chosen = fdt_path_offset(src, "/chosen");
    dst_chosen = fdt_path_offset(dst, "/chosen");
    if (src_chosen < 0 || dst_chosen < 0)
        return -FDT_ERR_NOTFOUND;

    /* Clear destination /chosen, but preserve its node itself. */
    for (;;) {
        prop = fdt_first_property_offset(dst, dst_chosen);
        if (prop == -FDT_ERR_NOTFOUND)
            break;
        if (prop < 0)
            return prop;
        value = fdt_getprop_by_offset(dst, prop, &name, NULL);
        if (!value || !name)
            return -FDT_ERR_BADSTRUCTURE;
        name_len = strlen(name);
        if (name_len >= sizeof(evr_chosen_path))
            return -FDT_ERR_NOSPACE;
        evr_fdt_copy_bytes(evr_chosen_path, name, name_len + 1);
        ret = fdt_delprop(dst, dst_chosen, evr_chosen_path);
        if (ret)
            return ret;
    }

    for (;;) {
        child = fdt_first_subnode(dst, dst_chosen);
        if (child == -FDT_ERR_NOTFOUND)
            break;
        if (child < 0)
            return child;
        ret = fdt_del_node(dst, child);
        if (ret)
            return ret;
    }

    ret = evr_copy_chosen_node_props(dst, src, src_chosen,
                                      "/chosen", &props);
    if (ret)
        return ret;

    /* Copy any firmware /chosen descendants, without touching hardware. */
    node = src_chosen;
    depth = 0;
    for (;;) {
        next = fdt_next_node(src, node, &depth);
        if (next == -FDT_ERR_NOTFOUND || depth <= 0)
            break;
        if (next < 0)
            return next;
        node = next;
        ret = fdt_get_path(src, node, evr_chosen_path,
                           sizeof(evr_chosen_path));
        if (ret)
            return ret;
        slash = strrchr(evr_chosen_path, '/');
        if (!slash || slash == evr_chosen_path)
            return -FDT_ERR_BADPATH;
        *slash = '\0';
        parent = fdt_path_offset(dst, evr_chosen_path);
        *slash = '/';
        if (parent < 0)
            return parent;
        child = fdt_add_subnode(dst, parent, slash + 1);
        if (child < 0)
            return child;
        ++children;

        ret = evr_copy_chosen_node_props(dst, src, node,
                                          evr_chosen_path, &props);
        if (ret)
            return ret;
    }

    printk(KERN_NOTICE,
           "[%s:chosen] OK  /chosen replaced from firmware (%u props, %u subnodes)\n",
           everest_product_name(), props, children);
    return 0;
}

static const char *evr_find_bootarg(const char *cmdline, const char *token)
{
    const char *p = cmdline;
    size_t len = strlen(token);

    while (*p) {
        const char *end;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        end = p;
        while (*end && *end != ' ')
            end++;
        if ((size_t)(end - p) == len && !memcmp(p, token, len))
            return p;
        p = end;
    }
    return NULL;
}

/* Old setup.c: only change charger mode if BOTH tokens are present. */
static int evr_fix_charger_bootmode(void *fdt)
{
    static const char normal[] = "androidboot.mode=normal";
    static const char charger[] = "androidboot.mode=charger";
    const char *args, *mode;
    size_t offset, new_len;
    int chosen, len, ret;

    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0)
        return chosen;
    args = fdt_getprop(fdt, chosen, "bootargs", &len);
    if (!args || len < 2)
        return 0;
    if (strnlen(args, len) != (size_t)(len - 1))
        return -FDT_ERR_BADVALUE;
    if (!evr_find_bootarg(args, "normal_reset_type=BR_POWERON_CHARGE"))
        return 0;
    mode = evr_find_bootarg(args, normal);
    if (!mode)
        return 0;

    offset = mode - args;
    new_len = len + sizeof(charger) - sizeof(normal);
    if (new_len > sizeof(evr_bootargs_buf))
        return -FDT_ERR_NOSPACE;

    evr_fdt_copy_bytes(evr_bootargs_buf, args, offset);
    evr_fdt_copy_bytes(evr_bootargs_buf + offset, charger, sizeof(charger) - 1);
    evr_fdt_copy_bytes(evr_bootargs_buf + offset + sizeof(charger) - 1,
           mode + sizeof(normal) - 1,
           len - offset - (sizeof(normal) - 1));

    ret = fdt_setprop(fdt, chosen, "bootargs", evr_bootargs_buf, new_len);
    if (!ret)
        printk(KERN_NOTICE, "[%s:06] OK  Charger boot mode\n", everest_product_name());
    return ret;
}

/*
 * The linker embeds everest_catalog.dtb as one read-only FDT, sized to its
 * actual contents (no 16 MiB runtime reservation).  Its dtb-data payloads
 * are only FDT-property aligned: NEVER pass an embedded pointer to libfdt.
 * Instead, copy the selected member bytewise to our existing 4 MiB, 8-byte
 * aligned runtime workspace, then open it in place.
 *
 * Return 0 for a traditional single-board DTB, 1 for a selected catalog
 * member, and a negative libfdt error on failure.
 */
static int evr_catalog_select(const void *input, const void *bootloader_fdt,
                              void *workspace)
{
    const char *compatible;
    const void *payload = NULL;
    const fdt32_t *boardid, *candidate_id;
    int root, list, child, len, payload_len = 0, ret, matches = 0;
    int i;

    list = fdt_path_offset(input, "/everest,dtb-catalog");
    if (list == -FDT_ERR_NOTFOUND)
        return 0;  /* Existing single-DTB path. */
    if (list < 0)
        return list;

    if (fdt_totalsize(input) > EVR_MAX_CATALOG_FDT_BYTES)
        return -FDT_ERR_NOSPACE;

    root = fdt_path_offset(input, "/");
    if (root < 0)
        return root;
    compatible = fdt_getprop(input, root, "compatible", &len);
    if (!evr_fdt_single_string(compatible, len) ||
        strcmp(compatible, "huawei,everest-dtb-catalog-v1"))
        return -FDT_ERR_BADVALUE;

    root = fdt_path_offset(bootloader_fdt, "/rootparam");
    if (root < 0)
        return root;
    boardid = fdt_getprop(bootloader_fdt, root, "hisi,boardid", &len);
    if (!boardid || len != 4 * (int)sizeof(fdt32_t))
        return -FDT_ERR_BADVALUE;

    for (child = fdt_first_subnode(input, list);
         child >= 0; child = fdt_next_subnode(input, child)) {
        bool equal = true;

        candidate_id = fdt_getprop(input, child, "hisi,boardid", &len);
        if (!candidate_id || len != 4 * (int)sizeof(fdt32_t))
            return -FDT_ERR_BADVALUE;
        /* Safe under Huawei SCTLR_EL1.A: no unaligned wide load. */
        for (i = 0; i < 4 * (int)sizeof(fdt32_t); i++) {
            if (((const volatile unsigned char *)candidate_id)[i] !=
                ((const volatile unsigned char *)boardid)[i]) {
                equal = false;
                break;
            }
        }
        if (!equal)
            continue;
        if (++matches != 1)
            return -FDT_ERR_BADVALUE;
        payload = fdt_getprop(input, child, "dtb-data", &payload_len);
        if (!payload || payload_len < (int)sizeof(struct fdt_header) ||
            payload_len > (int)EVR_MAX_SOURCE_FDT_BYTES ||
            payload_len > (int)CONFIG_FDT_BUF_SIZE)
            return -FDT_ERR_BADVALUE;
    }
    if (child != -FDT_ERR_NOTFOUND)
        return child;
    if (matches != 1)
        return -FDT_ERR_NOTFOUND;

    /* The embedded DTB can be 4-byte aligned, not necessarily 8-byte aligned. */
    evr_fdt_copy_bytes(workspace, payload, (size_t)payload_len);
    ret = fdt_check_header(workspace);
    if (ret)
        return ret;
    if (fdt_totalsize(workspace) != (uint32_t)payload_len)
        return -FDT_ERR_BADVALUE;

    /* Verify the selected payload's identity before any FDT adaptation. */
    root = fdt_path_offset(workspace, "/rootparam");
    if (root < 0)
        return root;
    candidate_id = fdt_getprop(workspace, root, "hisi,boardid", &len);
    if (!candidate_id || len != 4 * (int)sizeof(fdt32_t))
        return -FDT_ERR_BADVALUE;
    for (i = 0; i < 4 * (int)sizeof(fdt32_t); i++) {
        if (((const volatile unsigned char *)candidate_id)[i] !=
            ((const volatile unsigned char *)boardid)[i])
            return -FDT_ERR_BADVALUE;
    }

    printk(KERN_INFO, "Everest FDT: selected catalog board %u.%u.%u.%u (%u bytes)\n",
           fdt32_to_cpu(boardid[0]), fdt32_to_cpu(boardid[1]),
           fdt32_to_cpu(boardid[2]), fdt32_to_cpu(boardid[3]),
           (unsigned int)payload_len);
    return 1;
}

int everest_patch_dtb(void **dt, const void *bootloader_fdt)
{
    void *out = evr_runtime_fdt;
    int ret;
    uint32_t orig_size, dest_size;

    if (!dt || !*dt || !bootloader_fdt ||
        ((uintptr_t)bootloader_fdt & 7)) {
        printk(KERN_ERR, "Everest FDT: missing/misaligned source DTB\n");
        return -FDT_ERR_BADVALUE;
    }
    ret = fdt_check_header(bootloader_fdt);
    if (ret) {
        printk(KERN_ERR, "Everest FDT: invalid bootloader FDT: %s\n",
               fdt_strerror(ret));
        return ret;
    }
    orig_size = fdt_totalsize(bootloader_fdt);
    if (orig_size < sizeof(struct fdt_header) ||
        orig_size > EVR_MAX_SOURCE_FDT_BYTES)
        return -FDT_ERR_BADVALUE;
    ret = fdt_check_header(*dt);
    if (ret)
        return ret;

    /* Select by the original Huawei board ID, or keep the old single DTB. */
    ret = evr_catalog_select(*dt, bootloader_fdt, out);
    if (ret < 0)
        goto error;
    if (ret == 1) {
        dest_size = fdt_totalsize(out);
        if (dest_size < sizeof(struct fdt_header) ||
            dest_size > sizeof(evr_runtime_fdt))
            return -FDT_ERR_NOSPACE;
        /* In-place open is supported by the bundled libfdt. */
        ret = fdt_open_into(out, out, sizeof(evr_runtime_fdt));
    } else {
        dest_size = fdt_totalsize(*dt);
        if (dest_size < sizeof(struct fdt_header) ||
            dest_size > sizeof(evr_runtime_fdt))
            return -FDT_ERR_NOSPACE;
        ret = fdt_open_into(*dt, out, sizeof(evr_runtime_fdt));
    }
    if (ret)
        goto error;
    /* Validate hardware identity and apply the tested EMUI10 fixups first. */
    ret = huawei_everest_adapt_fdt(out, bootloader_fdt);
    if (ret)
        goto error;
    /* Replace the entire /chosen subtree, not just four properties. */
    ret = evr_replace_chosen(out, bootloader_fdt);
    if (ret)
        goto error;
    ret = evr_fix_charger_bootmode(out);
    if (ret)
        goto error;
    ret = fdt_pack(out);
    if (ret)
        goto error;

    /* This uniLoader libfdt ships no fdt_check_full() implementation.
     * Validate the packed header and traverse all nodes instead.
     */
    ret = fdt_check_header(out);
    if (ret)
        goto error;
    {
        int depth = 0;
        int node = fdt_next_node(out, -1, &depth);

        if (node != 0) {
            ret = node < 0 ? node : -FDT_ERR_BADSTRUCTURE;
            goto error;
        }
        while (node >= 0)
            node = fdt_next_node(out, node, &depth);
        if (node != -FDT_ERR_NOTFOUND || depth != 0) {
            ret = node != -FDT_ERR_NOTFOUND ? node : -FDT_ERR_BADSTRUCTURE;
            goto error;
        }
    }

    *dt = out;
    printk(KERN_NOTICE, "[%s:04] OK  FDT adaptation completed\n", everest_product_name());
    return 0;

error:
    printk(KERN_ERR, "[%s:04] FAIL  FDT adaptation failed\n", everest_product_name());
    printk(KERN_ERR, "Everest FDT: firmware adaptation failed: %s (%d)\n",
           fdt_strerror(ret), ret);
    return ret;
}
