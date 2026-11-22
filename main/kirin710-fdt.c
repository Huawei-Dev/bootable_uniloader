// SPDX-License-Identifier: GPL-2.0-only
/*
 * Huawei Kirin710 family: adapt EMUI10 DTBs using bootloader runtime facts.
 * Independent K710 adaptation derived from the existing Everest uniLoader implementation.
 * Select an embedded DTB by exact 4-cell hisi,boardid.
 * Hardware boot verified on one SNE 6 GiB device; other variants untested.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <lib/debug.h>
#include <lib/libfdt/libfdt.h>
#include <main/kirin710-fdt.h>

#define K710_MAX_SOURCE_FDT_BYTES 2097152U
#define K710_MAX_CATALOG_FDT_BYTES (64U * 1024U * 1024U)

static uint8_t k710_runtime_fdt[CONFIG_FDT_BUF_SIZE] __attribute__((aligned(8)));

/* A short, persistent label from the original Huawei firmware FDT.
 * A neutral label is used only if firmware identity cannot be displayed.
 * Never retain pointers into a mutable FDT property. */
#define K710_PRODUCT_NAME_MAX 64
static char k710_product_name[K710_PRODUCT_NAME_MAX] = "k710-UNKNOWN";

const char *kirin710_product_name(void)
{
    return k710_product_name;
}

/*
 * Huawei pre-kernel firmware may enforce strict alignment (SCTLR_EL1.A).
 * FDT properties are only 4-byte aligned; uniLoader's ARM64 memcpy.S uses
 * potentially unaligned 64-bit loads for copies of 8..15 bytes.
 * Use explicit byte accesses throughout this Kirin710 FDT adapter.
 */
static void k710_fdt_copy_bytes(void *dst, const void *src, size_t size)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;

    while (size--) {
        *d++ = *s++;
    }
}

static bool k710_fdt_single_string(const char *value, int len)
{
	return value && len > 1 && value[len - 1] == '\0' &&
		strnlen(value, len) == (size_t)(len - 1);
}

/* The label is printed on the framebuffer: reject control characters and
 * overly long strings, but do not change FDT validation or boot behavior. */
static bool k710_fdt_safe_product_label(const char *value, int len)
{
    int i;

    if (!k710_fdt_single_string(value, len) ||
        len > (int)sizeof(k710_product_name))
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

static int k710_fdt_copy_prop(void *dst, const void *src,
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

/* The complete four-cell boardid is the only hardware identity key.
 * /rootparam in the selected EMUI10 DTB was already populated statically.
 * Do not overwrite it with older firmware labels or modem fields.
 */
static int k710_fdt_validate_identity(const void *dst, const void *src)
{
    const fdt32_t *source_id, *target_id;
    const char *label;
    int source, target, slen, tlen, i;

    source = fdt_path_offset(src, "/rootparam");
    target = fdt_path_offset(dst, "/rootparam");
    if (source < 0 || target < 0)
        return -FDT_ERR_NOTFOUND;
    source_id = fdt_getprop(src, source, "hisi,boardid", &slen);
    target_id = fdt_getprop(dst, target, "hisi,boardid", &tlen);
    if (!source_id || !target_id || slen != 16 || tlen != 16)
        return -FDT_ERR_BADVALUE;
    /* Do not assume FDT properties have 8-byte alignment. */
    for (i = 0; i < 16; i++)
        if (((const volatile uint8_t *)source_id)[i] !=
            ((const volatile uint8_t *)target_id)[i])
            return -FDT_ERR_BADVALUE;

    label = fdt_getprop(src, source, "hisi,product_name", &slen);
    if (k710_fdt_safe_product_label(label, slen))
        k710_fdt_copy_bytes(k710_product_name, label, (size_t)slen);
    printk(KERN_NOTICE, "[%s:01] OK boardid=%u.%u.%u.%u\n",
           kirin710_product_name(), fdt32_to_cpu(source_id[0]),
           fdt32_to_cpu(source_id[1]), fdt32_to_cpu(source_id[2]),
           fdt32_to_cpu(source_id[3]));
    return 0;
}

/* Full compatible string-list parser: rejects malformed trailing strings. */
static int k710_fdt_compatible_match(const char *compatible, int len,
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

#define K710_LCD_NAME_MAX 192
#define K710_LCD_TYPE_MAX 192
#define K710_LCD_PATH_MAX 384
#define K710_MAX_RAM_RANGES 16

struct k710_panel_identity {
    /* K710_LCD_NODE_ID_V1: retain the exact stock active panel node name. */
    char node_name[K710_LCD_NAME_MAX];
    char name[K710_LCD_NAME_MAX];
    char model[K710_LCD_NAME_MAX];
    bool have_model;
};

/* Read optional, single, NUL-terminated display string into own storage. */
static int k710_fdt_read_string(const void *fdt, int node,
                                 const char *prop, char *out, size_t cap,
                                 bool required, bool *present)
{
    const char *str;
    int len;
    str = fdt_getprop(fdt, node, prop, &len);
    if (!str) {
        if (!required && len == -FDT_ERR_NOTFOUND) {
            if (present)
                *present = false;
            return 0;
        }
        return -FDT_ERR_BADVALUE;
    }
    if (len > (int)cap || !k710_fdt_single_string(str, len))
        return -FDT_ERR_BADVALUE;
    k710_fdt_copy_bytes(out, str, (size_t)len);
    if (present)
        *present = true;
    return 0;
}

/* Huawei uses both "disable" and "disabled" in the stock Kirin710 FDTs. */
static int k710_fdt_status_enabled(const char *status, int len)
{
    if (!status)
        return len == -FDT_ERR_NOTFOUND ? 1 : -FDT_ERR_BADVALUE;
    if (!k710_fdt_single_string(status, len))
        return -FDT_ERR_BADVALUE;
    if (!strcmp(status, "disable") || !strcmp(status, "disabled") ||
        !strcmp(status, "fail") || !strncmp(status, "fail-", 5))
        return 0;
    if (!strcmp(status, "ok") || !strcmp(status, "okay"))
        return 1;
    return -FDT_ERR_BADVALUE;
}

static int k710_fdt_source_panel(const void *src,
                                  struct k710_panel_identity *identity)
{
    int cfg, node, panel, type_len, compat_len, len, enabled, matches = 0;
    const char *type, *compatible, *status, *node_name;
    int node_name_len;
    int ret;

    cfg = fdt_path_offset(src, "/huawei,lcd_config");
    panel = fdt_path_offset(src, "/huawei,lcd_panel");
    if (cfg < 0 || panel < 0)
        return -FDT_ERR_NOTFOUND;
    type = fdt_getprop(src, panel, "lcd_panel_type", &type_len);
    if (!k710_fdt_single_string(type, type_len))
        return -FDT_ERR_BADVALUE;

    for (node = fdt_first_subnode(src, cfg);
         node >= 0; node = fdt_next_subnode(src, node)) {
        status = fdt_getprop(src, node, "status", &len);
        enabled = k710_fdt_status_enabled(status, len);
        if (enabled < 0)
            return enabled;
        if (!enabled)
            continue;
        if (++matches != 1)
            return -FDT_ERR_BADVALUE;
        node_name = fdt_get_name(src, node, &node_name_len);
        if (!node_name || node_name_len <= 0 ||
            node_name_len >= (int)sizeof(identity->node_name))
            return -FDT_ERR_BADVALUE;
        k710_fdt_copy_bytes(identity->node_name, node_name,
                            (size_t)node_name_len + 1);
        compatible = fdt_getprop(src, node, "compatible", &compat_len);
        ret = k710_fdt_compatible_match(compatible, compat_len, type,
                                       type_len - 1);
        if (ret != 1)
            return ret < 0 ? ret : -FDT_ERR_BADVALUE;
        ret = k710_fdt_read_string(src, node, "hw,lcdkit-panel-name",
                                    identity->name, sizeof(identity->name),
                                    true, NULL);
        if (ret)
            return ret;
        ret = k710_fdt_read_string(src, node, "hw,lcdkit-panel-model",
                                    identity->model, sizeof(identity->model),
                                    false, &identity->have_model);
        if (ret)
            return ret;
    }
    if (node != -FDT_ERR_NOTFOUND)
        return node;
    if (matches != 1)
        return -FDT_ERR_BADVALUE;
    printk(KERN_NOTICE, "Kirin710 FDT: bootloader LCD node='%s'\n",
           identity->node_name);
    printk(KERN_NOTICE, "Kirin710 FDT: bootloader LCD name='%s' model='%s'\n",
           identity->name, identity->have_model ? identity->model : "<absent>");
    return 0;
}

/* Match on name; if model exists in both trees it must match as well.
 * Model-only fallback is unsafe: multiple LCD vendors can share a model.
 */
static int k710_fdt_scan_panel(const void *dst,
                                const struct k710_panel_identity *wanted,
                                char *selected_path, size_t path_cap,
                                char *selected_type, size_t type_cap,
                                int *matched, int *enabled_count)
{
    int cfg, node, len, status_len, model_len, comp_len, ret;
    int matches = 0, enabled_total = 0;
    const char *name, *model, *status, *compatible, *node_name;
    int node_name_len;

    cfg = fdt_path_offset(dst, "/huawei,lcd_config");
    if (cfg < 0)
        return cfg;
    for (node = fdt_first_subnode(dst, cfg);
         node >= 0; node = fdt_next_subnode(dst, node)) {
        int is_enabled;
        status = fdt_getprop(dst, node, "status", &status_len);
        is_enabled = k710_fdt_status_enabled(status, status_len);
        if (is_enabled < 0)
            return is_enabled;
        enabled_total += is_enabled;

        name = fdt_getprop(dst, node, "hw,lcdkit-panel-name", &len);
        if (!name) {
            if (len != -FDT_ERR_NOTFOUND)
                return -FDT_ERR_BADVALUE;
            continue;
        }
        if (!k710_fdt_single_string(name, len))
            return -FDT_ERR_BADVALUE;
        if (strcmp(name, wanted->name))
            continue;

        model = fdt_getprop(dst, node, "hw,lcdkit-panel-model", &model_len);
        if (model) {
            if (!k710_fdt_single_string(model, model_len))
                return -FDT_ERR_BADVALUE;
            if (wanted->have_model && strcmp(model, wanted->model))
                continue;
        } else if (model_len != -FDT_ERR_NOTFOUND) {
            return -FDT_ERR_BADVALUE;
        }
        /* Same name/model can identify different Kirin 710 board revisions.
         * Match the original firmware node identifier too. EMUI9.1
         * appends a compatible suffix omitted by EMUI10 DTS nodes.
         */
        node_name = fdt_get_name(dst, node, &node_name_len);
        if (!node_name || node_name_len <= 0 ||
            node_name_len >= (int)sizeof(wanted->node_name))
            return -FDT_ERR_BADVALUE;
        if (strcmp(node_name, wanted->node_name) &&
            !(strncmp(wanted->node_name, node_name,
                       (size_t)node_name_len) == 0 &&
              wanted->node_name[node_name_len] == '_'))
            continue;
        if (++matches != 1)
            return -FDT_ERR_BADVALUE;
        ret = fdt_get_path(dst, node, selected_path, (int)path_cap);
        if (ret)
            return ret;
        compatible = fdt_getprop(dst, node, "compatible", &comp_len);
        if (!compatible || comp_len < 2)
            return -FDT_ERR_BADVALUE;
        len = strnlen(compatible, comp_len);
        if (!len || len >= comp_len || (size_t)(len + 1) > type_cap)
            return -FDT_ERR_BADVALUE;
        ret = k710_fdt_compatible_match(compatible, comp_len,
                                          compatible, len);
        if (ret != 1)
            return -FDT_ERR_BADVALUE;
        k710_fdt_copy_bytes(selected_type, compatible, (size_t)(len + 1));
    }
    if (node != -FDT_ERR_NOTFOUND)
        return node;
    *matched = matches;
    *enabled_count = enabled_total;
    return matches == 1 ? 0 : -FDT_ERR_BADVALUE;
}

/* Set any non-selected active panel to disabled. Each mutation restarts the
 * scan because libfdt node offsets are not stable after fdt_setprop().
 */
static int k710_fdt_disable_other_panels(void *dst, const char *selected_path)
{
    char other[K710_LCD_PATH_MAX];
    int cfg, node, len, state, ret;
    const char *status;
    for (;;) {
        bool changed = false;
        cfg = fdt_path_offset(dst, "/huawei,lcd_config");
        if (cfg < 0)
            return cfg;
        for (node = fdt_first_subnode(dst, cfg);
             node >= 0; node = fdt_next_subnode(dst, node)) {
            status = fdt_getprop(dst, node, "status", &len);
            state = k710_fdt_status_enabled(status, len);
            if (state < 0)
                return state;
            if (!state)
                continue;
            ret = fdt_get_path(dst, node, other, sizeof(other));
            if (ret)
                return ret;
            if (!strcmp(other, selected_path))
                continue;
            ret = fdt_setprop(dst, node, "status", "disabled", sizeof("disabled"));
            if (ret)
                return ret;
            changed = true;
            break;
        }
        if (!changed)
            return node == -FDT_ERR_NOTFOUND ? 0 : node;
    }
}

static int k710_fdt_select_panel(void *dst, const void *src)
{
    struct k710_panel_identity wanted = { 0 };
    char path[K710_LCD_PATH_MAX], check_path[K710_LCD_PATH_MAX];
    char type[K710_LCD_TYPE_MAX], check_type[K710_LCD_TYPE_MAX];
    int ret, selected, panel, len, matched, enabled;
    const char *value;

    ret = k710_fdt_source_panel(src, &wanted);
    if (ret)
        return ret;
    ret = k710_fdt_scan_panel(dst, &wanted, path, sizeof(path),
                               type, sizeof(type), &matched, &enabled);
    if (ret)
        return ret;
    ret = k710_fdt_disable_other_panels(dst, path);
    if (ret)
        return ret;
    selected = fdt_path_offset(dst, path);
    if (selected < 0)
        return selected;
    ret = fdt_setprop(dst, selected, "status", "ok", sizeof("ok"));
    if (ret)
        return ret;

    panel = fdt_path_offset(dst, "/huawei,lcd_panel");
    if (panel < 0)
        return panel;
    ret = fdt_setprop(dst, panel, "lcd_panel_type", type, strlen(type) + 1);
    if (ret)
        return ret;

    /* The bias IC is physical hardware; import only when firmware provides
     * an explicit value. Do not copy other EMUI9.1 display node properties.
     */
    panel = fdt_path_offset(src, "/huawei,lcd_panel");
    if (panel < 0)
        return panel;
    value = fdt_getprop(src, panel, "lcd-bias-ic-name", &len);
    if (value) {
        if (!k710_fdt_single_string(value, len))
            return -FDT_ERR_BADVALUE;
        ret = k710_fdt_copy_prop(dst, src, "/huawei,lcd_panel",
                                  "/huawei,lcd_panel", "lcd-bias-ic-name");
        if (ret)
            return ret;
    } else if (len != -FDT_ERR_NOTFOUND) {
        return -FDT_ERR_BADVALUE;
    }

    ret = k710_fdt_scan_panel(dst, &wanted, check_path, sizeof(check_path),
                               check_type, sizeof(check_type), &matched,
                               &enabled);
    if (ret || enabled != 1 || strcmp(check_path, path) || strcmp(type, check_type))
        return ret ? ret : -FDT_ERR_BADVALUE;
    panel = fdt_path_offset(dst, "/huawei,lcd_panel");
    if (panel < 0)
        return panel;
    value = fdt_getprop(dst, panel, "lcd_panel_type", &len);
    if (!k710_fdt_single_string(value, len) || strcmp(type, value))
        return -FDT_ERR_BADVALUE;
    printk(KERN_NOTICE, "[%s:02] OK LCD name='%s' model='%s'\n",
           kirin710_product_name(), wanted.name,
           wanted.have_model ? wanted.model : "<absent>");
    return 0;
}

struct k710_ram_range {
    uint64_t base, size;
};

static int k710_fdt_read_ram(const void *src, const void *dst,
                              struct k710_ram_range ranges[K710_MAX_RAM_RANGES],
                              int *count)
{
    const fdt32_t *cells, *reg;
    const void *trees[2] = { src, dst };
    int k, root, node, len, i, j;

    for (k = 0; k < 2; k++) {
        root = fdt_path_offset(trees[k], "/");
        if (root < 0)
            return root;
        cells = fdt_getprop(trees[k], root, "#address-cells", &len);
        if (!cells || len != sizeof(fdt32_t) || fdt32_to_cpu(*cells) != 2)
            return -FDT_ERR_BADVALUE;
        cells = fdt_getprop(trees[k], root, "#size-cells", &len);
        if (!cells || len != sizeof(fdt32_t) || fdt32_to_cpu(*cells) != 2)
            return -FDT_ERR_BADVALUE;
    }
    node = fdt_path_offset(src, "/memory@0");
    if (node < 0 || fdt_path_offset(dst, "/memory@0") < 0)
        return -FDT_ERR_NOTFOUND;
    reg = fdt_getprop(src, node, "reg", &len);
    if (!reg || len < 16 || len > 16 * K710_MAX_RAM_RANGES || (len % 16))
        return -FDT_ERR_BADVALUE;
    *count = len / 16;
    for (i = 0; i < *count; i++) {
        ranges[i].base = ((uint64_t)fdt32_to_cpu(reg[4*i]) << 32) |
                         fdt32_to_cpu(reg[4*i+1]);
        ranges[i].size = ((uint64_t)fdt32_to_cpu(reg[4*i+2]) << 32) |
                         fdt32_to_cpu(reg[4*i+3]);
        if (!ranges[i].size || ranges[i].base > UINT64_MAX - ranges[i].size ||
            (ranges[i].base & 4095) || (ranges[i].size & 4095))
            return -FDT_ERR_BADVALUE;
        for (j = 0; j < i; j++) {
            uint64_t e1 = ranges[i].base + ranges[i].size;
            uint64_t e2 = ranges[j].base + ranges[j].size;
            if (ranges[i].base < e2 && ranges[j].base < e1)
                return -FDT_ERR_BADVALUE;
        }
    }
    return 0;
}

static bool k710_fdt_region_in_ram(const struct k710_ram_range *ram,
                                   int count, uint64_t base, uint64_t size)
{
    int i;
    if (!size || base > UINT64_MAX - size)
        return false;
    for (i = 0; i < count; i++) {
        if (base >= ram[i].base &&
            base + size <= ram[i].base + ram[i].size)
            return true;
    }
    return false;
}

static int k710_fdt_reservation_status(void *dst, const void *src,
                                        const char *path)
{
    const char *value;
    int src_node, dst_node, len, enabled;
    src_node = fdt_path_offset(src, path);
    dst_node = fdt_path_offset(dst, path);
    if (src_node < 0 || dst_node < 0)
        return -FDT_ERR_NOTFOUND;
    value = fdt_getprop(src, src_node, "status", &len);
    if (!value)
        return -FDT_ERR_BADVALUE;
    enabled = k710_fdt_status_enabled(value, len);
    if (enabled < 0)
        return enabled;
    return k710_fdt_copy_prop(dst, src, path, path, "status");
}

static int k710_fdt_validate_cma(const void *src,
                                  const struct k710_ram_range *ram, int count)
{
    const fdt32_t *reg;
    uint64_t base, size;
    int node, len;
    node = fdt_path_offset(src, "/reserved-memory/hisi_cma");
    if (node < 0)
        return node;
    reg = fdt_getprop(src, node, "reg", &len);
    if (!reg || len != 16)
        return -FDT_ERR_BADVALUE;
    base = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
    size = ((uint64_t)fdt32_to_cpu(reg[2]) << 32) | fdt32_to_cpu(reg[3]);
    if ((base & 4095) || (size & 4095) ||
        !k710_fdt_region_in_ram(ram, count, base, size))
        return -FDT_ERR_BADVALUE;
    printk(KERN_NOTICE, "Kirin710 FDT: firmware hisi_cma base=0x%llx size=%llu MiB\n",
           (unsigned long long)base, (unsigned long long)(size >> 20));
    return 0;
}

/* A *disabled* camera-mem does not request this dynamic allocation. If
 * enabled, the 640-MiB allocation must fit in a contiguous RAM intersection
 * with the specified alloc-ranges. No inference from total RAM GB alone.
 */
static int k710_fdt_validate_camera_pool(const void *src, const void *dst,
                                          const struct k710_ram_range *ram,
                                          int count)
{
    const fdt32_t *size_prop, *alloc, *alignment_prop;
    const char *status;
    uint64_t size, base, limit, alignment, lo, hi, first;
    int node, len, state, alloc_len, alignment_len, i;
    node = fdt_path_offset(src, "/reserved-memory/camera-mem");
    if (node < 0)
        return node;
    status = fdt_getprop(src, node, "status", &len);
    state = k710_fdt_status_enabled(status, len);
    if (state < 0)
        return state;
    if (!state)
        return 0;
    /* Allocation descriptors are kept from the selected EMUI10 DTB. */
    node = fdt_path_offset(dst, "/reserved-memory/camera-mem");
    if (node < 0)
        return node;
    size_prop = fdt_getprop(dst, node, "size", &len);
    alloc = fdt_getprop(dst, node, "alloc-ranges", &alloc_len);
    alignment_prop = fdt_getprop(dst, node, "alignment", &alignment_len);
    if (!size_prop || len != 8 || !alloc || alloc_len != 16 ||
        !alignment_prop || alignment_len != 8)
        return -FDT_ERR_BADVALUE;
    size = ((uint64_t)fdt32_to_cpu(size_prop[0]) << 32) |
            fdt32_to_cpu(size_prop[1]);
    base = ((uint64_t)fdt32_to_cpu(alloc[0]) << 32) | fdt32_to_cpu(alloc[1]);
    limit = ((uint64_t)fdt32_to_cpu(alloc[2]) << 32) |
             fdt32_to_cpu(alloc[3]);
    alignment = ((uint64_t)fdt32_to_cpu(alignment_prop[0]) << 32) |
                 fdt32_to_cpu(alignment_prop[1]);
    if (!size || !limit || !alignment || (alignment & (alignment - 1)) ||
        base > UINT64_MAX - limit)
        return -FDT_ERR_BADVALUE;
    for (i = 0; i < count; i++) {
        lo = ram[i].base > base ? ram[i].base : base;
        hi = ram[i].base + ram[i].size < base + limit ?
             ram[i].base + ram[i].size : base + limit;
        if (hi <= lo || lo > UINT64_MAX - (alignment - 1))
            continue;
        first = (lo + alignment - 1) & ~(alignment - 1);
        if (first < hi && hi - first >= size)
            return 0;
    }
    return -FDT_ERR_BADVALUE;
}

static int k710_fdt_sync_memory(void *dst, const void *src)
{
    struct k710_ram_range ram[K710_MAX_RAM_RANGES];
    int count, ret;
    ret = k710_fdt_read_ram(src, dst, ram, &count);
    if (ret)
        return ret;
    ret = k710_fdt_validate_cma(src, ram, count);
    if (ret)
        return ret;
    ret = k710_fdt_validate_camera_pool(src, dst, ram, count);
    if (ret)
        return ret;
    ret = k710_fdt_copy_prop(dst, src, "/memory@0", "/memory@0", "reg");
    if (ret)
        return ret;
    ret = k710_fdt_copy_prop(dst, src, "/reserved-memory/hisi_cma",
                              "/reserved-memory/hisi_cma", "reg");
    if (ret)
        return ret;
    ret = k710_fdt_reservation_status(dst, src, "/reserved-memory/camera-mem");
    if (ret)
        return ret;
    ret = k710_fdt_reservation_status(dst, src, "/reserved-memory/sec_camera");
    if (ret)
        return ret;
    printk(KERN_NOTICE, "[%s:03] OK RAM (%d ranges), CMA, camera statuses\n",
           kirin710_product_name(), count);
    return 0;
}

static int huawei_kirin710_adapt_fdt(void *dst, const void *src)
{
    int ret;
    if (!src || !dst || fdt_check_header(src) || fdt_check_header(dst))
        return -FDT_ERR_BADVALUE;
    ret = k710_fdt_validate_identity(dst, src);
    if (ret)
        return ret;
    ret = k710_fdt_select_panel(dst, src);
    if (ret)
        return ret;
    ret = k710_fdt_sync_memory(dst, src);
    if (ret)
        return ret;
    /* Logo reservation, PSCI, fastboot and HDR are static K710 DTS fixes. */
    return 0;
}

/* Replace the entire /chosen subtree from the firmware FDT.
 * Other device-specific changes are limited to LCD selection and RAM/CMA/
 * camera reservations in huawei_kirin710_adapt_fdt().
 * The /chosen node must exist in both trees.
 */
#define K710_CHOSEN_PATH_MAX 1024
static char k710_chosen_path[K710_CHOSEN_PATH_MAX];

static int k710_copy_chosen_node_props(void *dst, const void *src,
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

static int k710_replace_chosen(void *dst, const void *src)
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
        if (name_len >= sizeof(k710_chosen_path))
            return -FDT_ERR_NOSPACE;
        k710_fdt_copy_bytes(k710_chosen_path, name, name_len + 1);
        ret = fdt_delprop(dst, dst_chosen, k710_chosen_path);
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

    ret = k710_copy_chosen_node_props(dst, src, src_chosen,
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
        ret = fdt_get_path(src, node, k710_chosen_path,
                           sizeof(k710_chosen_path));
        if (ret)
            return ret;
        slash = strrchr(k710_chosen_path, '/');
        if (!slash || slash == k710_chosen_path)
            return -FDT_ERR_BADPATH;
        *slash = '\0';
        parent = fdt_path_offset(dst, k710_chosen_path);
        *slash = '/';
        if (parent < 0)
            return parent;
        child = fdt_add_subnode(dst, parent, slash + 1);
        if (child < 0)
            return child;
        ++children;

        ret = k710_copy_chosen_node_props(dst, src, node,
                                          k710_chosen_path, &props);
        if (ret)
            return ret;
    }

    printk(KERN_NOTICE,
           "[%s:chosen] OK  /chosen replaced from firmware (%u props, %u subnodes)\n",
           kirin710_product_name(), props, children);
    return 0;
}

/*
 * The linker embeds kirin710_catalog.dtb as one read-only FDT, sized to its
 * actual contents (no allocation for the whole catalog). Its member payloads
 * are only 4-byte aligned: copy selected DTB bytewise into the 8-byte
 * aligned CONFIG_FDT_BUF_SIZE runtime workspace before libfdt access.
 *
 * Return 0 for a traditional single-board DTB, 1 for a selected catalog
 * member, and a negative libfdt error on failure.
 */
static int k710_catalog_select(const void *input, const void *bootloader_fdt,
                              void *workspace)
{
    const char *compatible;
    const void *payload = NULL;
    const fdt32_t *boardid, *candidate_id;
    int root, list, child, len, payload_len = 0, ret, matches = 0;
    int i;

    list = fdt_path_offset(input, "/kirin710,dtb-catalog");
    if (list == -FDT_ERR_NOTFOUND)
        return 0;  /* Existing single-DTB path. */
    if (list < 0)
        return list;

    if (fdt_totalsize(input) > K710_MAX_CATALOG_FDT_BYTES)
        return -FDT_ERR_NOSPACE;

    root = fdt_path_offset(input, "/");
    if (root < 0)
        return root;
    compatible = fdt_getprop(input, root, "compatible", &len);
    if (!k710_fdt_single_string(compatible, len) ||
        strcmp(compatible, "huawei,kirin710-dtb-catalog-v1"))
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
            payload_len > (int)K710_MAX_SOURCE_FDT_BYTES ||
            payload_len > (int)CONFIG_FDT_BUF_SIZE)
            return -FDT_ERR_BADVALUE;
    }
    if (child != -FDT_ERR_NOTFOUND)
        return child;
    if (matches != 1)
        return -FDT_ERR_NOTFOUND;

    /* The embedded DTB can be 4-byte aligned, not necessarily 8-byte aligned. */
    k710_fdt_copy_bytes(workspace, payload, (size_t)payload_len);
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

    printk(KERN_INFO, "Kirin710 FDT: selected catalog board %u.%u.%u.%u (%u bytes)\n",
           fdt32_to_cpu(boardid[0]), fdt32_to_cpu(boardid[1]),
           fdt32_to_cpu(boardid[2]), fdt32_to_cpu(boardid[3]),
           (unsigned int)payload_len);
    return 1;
}

int kirin710_patch_dtb(void **dt, const void *bootloader_fdt)
{
    void *out = k710_runtime_fdt;
    int ret;
    uint32_t orig_size, dest_size;

    if (!dt || !*dt || !bootloader_fdt ||
        ((uintptr_t)bootloader_fdt & 7)) {
        printk(KERN_ERR, "Kirin710 FDT: missing/misaligned source DTB\n");
        return -FDT_ERR_BADVALUE;
    }
    ret = fdt_check_header(bootloader_fdt);
    if (ret) {
        printk(KERN_ERR, "Kirin710 FDT: invalid bootloader FDT: %s\n",
               fdt_strerror(ret));
        return ret;
    }
    orig_size = fdt_totalsize(bootloader_fdt);
    if (orig_size < sizeof(struct fdt_header) ||
        orig_size > K710_MAX_SOURCE_FDT_BYTES)
        return -FDT_ERR_BADVALUE;
    ret = fdt_check_header(*dt);
    if (ret)
        return ret;

    /* Select by the original Huawei board ID, or keep the old single DTB. */
    ret = k710_catalog_select(*dt, bootloader_fdt, out);
    if (ret < 0)
        goto error;
    if (ret == 1) {
        dest_size = fdt_totalsize(out);
        if (dest_size < sizeof(struct fdt_header) ||
            dest_size > sizeof(k710_runtime_fdt))
            return -FDT_ERR_NOSPACE;
        /* In-place open is supported by the bundled libfdt. */
        ret = fdt_open_into(out, out, sizeof(k710_runtime_fdt));
    } else {
        dest_size = fdt_totalsize(*dt);
        if (dest_size < sizeof(struct fdt_header) ||
            dest_size > sizeof(k710_runtime_fdt))
            return -FDT_ERR_NOSPACE;
        ret = fdt_open_into(*dt, out, sizeof(k710_runtime_fdt));
    }
    if (ret)
        goto error;
    /* Validate board identity, select LCD and sync actual runtime memory. */
    ret = huawei_kirin710_adapt_fdt(out, bootloader_fdt);
    if (ret)
        goto error;
    /* Replace the entire /chosen subtree; do not import other hardware nodes. */
    ret = k710_replace_chosen(out, bootloader_fdt);
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
    printk(KERN_NOTICE, "[%s:04] OK  FDT adaptation completed\n", kirin710_product_name());
    return 0;

error:
    printk(KERN_ERR, "[%s:04] FAIL  FDT adaptation failed\n", kirin710_product_name());
    printk(KERN_ERR, "Kirin710 FDT: firmware adaptation failed: %s (%d)\n",
           fdt_strerror(ret), ret);
    return ret;
}
