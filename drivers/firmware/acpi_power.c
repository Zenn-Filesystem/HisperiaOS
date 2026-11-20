#include <stdint.h>
#include <stddef.h>
#include "acpi.h"

#define ACPI_SIG_RSDP 0x2052545020445352ULL
#define ACPI_SIG_FACP 0x50434146U
#define ACPI_SIG_DSDT 0x54445344U
#define ACPI_PM1_STS_PWRBTN (1U << 8)
#define ACPI_PM1_EN_PWRBTN  (1U << 8)
#define ACPI_PM1_CNT_SCI_EN (1U << 0)
#define ACPI_PM1_CNT_SLP_TYP_MASK (7U << 10)
#define ACPI_PM1_CNT_SLP_EN (1U << 13)
#define ACPI_FADT_HW_REDUCED (1U << 20)

struct rsdp_v1 {
    char signature[8];
    uint8_t checksum, oemid[6], revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
    uint32_t signature;
    uint32_t length;
    uint8_t revision, checksum;
    uint8_t oemid[6], oem_table_id[8];
    uint32_t oem_revision, creator_id, creator_revision;
} __attribute__((packed));

static uint64_t g_hhdm;
static uint16_t g_pm1a_evt;
static uint16_t g_pm1b_evt;
static uint16_t g_pm1a_cnt;
static uint16_t g_pm1b_cnt;
static uint8_t g_evt_len;
static uint8_t g_cnt_len;
static uint8_t g_s5a, g_s5b;
static int g_ready;

static inline uint16_t inw(uint16_t p) {
    uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline void outw(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}

static void *phys_ptr(uint64_t p) {
    return (void *)(uintptr_t)(g_hhdm + p);
}

static int checksum_ok(const void *ptr, uint32_t len) {
    const uint8_t *p = ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; ++i) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static struct sdt_header *find_table(struct sdt_header *root, uint32_t sig, int wide) {
    if (!root || root->length < sizeof(*root) || root->length > 0x100000U || !checksum_ok(root, root->length))
        return 0;
    uint32_t bytes = root->length - sizeof(*root);
    uint32_t stride = wide ? 8U : 4U;
    if (bytes % stride) return 0;
    uint8_t *entries = (uint8_t *)root + sizeof(*root);
    for (uint32_t off = 0; off < bytes; off += stride) {
        uint64_t address = 0;
        for (uint32_t j = 0; j < stride; ++j) address |= (uint64_t)entries[off + j] << (j * 8);
        if (!address) continue;
        struct sdt_header *h = phys_ptr(address);
        if (h->signature == sig && h->length >= sizeof(*h) && h->length <= 0x100000U && checksum_ok(h, h->length))
            return h;
    }
    return 0;
}

/* Decode AML PkgLength and the integer encodings used by the _S5 package. */
static int aml_pkg_length(const uint8_t *p, uint32_t *value, uint32_t *used) {
    uint8_t lead = p[0];
    uint32_t follow = lead >> 6;
    if (follow == 0) { *value = lead & 0x3fU; *used = 1; return 1; }
    uint32_t v = lead & 0x0fU;
    for (uint32_t i = 0; i < follow; ++i) v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *value = v; *used = follow + 1; return 1;
}

static int aml_integer(const uint8_t **cursor, const uint8_t *end, uint8_t *out) {
    const uint8_t *p = *cursor;
    if (p >= end) return 0;
    if (*p == 0x00) { *out = 0; *cursor = p + 1; return 1; }
    if (*p == 0x01) { *out = 1; *cursor = p + 1; return 1; }
    if (*p == 0x0A && p + 1 < end) { *out = p[1]; *cursor = p + 2; return 1; }
    if (*p == 0x0B && p + 2 < end) { *out = (uint8_t)(p[1] | ((uint16_t)p[2] << 8)); *cursor = p + 3; return 1; }
    if (*p == 0x0C && p + 4 < end) { *out = (uint8_t)(p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24)); *cursor = p + 5; return 1; }
    return 0;
}

static int find_s5(struct sdt_header *dsdt) {
    if (!dsdt || dsdt->signature != ACPI_SIG_DSDT || dsdt->length <= sizeof(*dsdt) || !checksum_ok(dsdt, dsdt->length)) return 0;
    const uint8_t *p = (const uint8_t *)dsdt + sizeof(*dsdt);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;
    for (; p + 8 < end; ++p) {
        if (p[0] != 0x08 || p[1] != '_' || p[2] != 'S' || p[3] != '5' || p[4] != '_') continue;
        if (p[5] != 0x12) continue; /* PackageOp */
        uint32_t pkg_len, pkg_bytes;
        if (!aml_pkg_length(p + 6, &pkg_len, &pkg_bytes)) continue;
        if (pkg_len <= pkg_bytes) continue;
        const uint8_t *q = p + 6 + pkg_bytes;
        const uint8_t *pkg_end = q + pkg_len - pkg_bytes;
        if (q >= end || pkg_end > end || *q++ < 2) continue;
        if (!aml_integer(&q, pkg_end, &g_s5a) || !aml_integer(&q, pkg_end, &g_s5b)) continue;
        if (g_s5a > 7 || g_s5b > 7) continue;
        return 1;
    }
    return 0;
}

int acpi_power_init(uint64_t rsdp_phys, uint64_t hhdm_offset) {
    g_ready = 0; g_hhdm = hhdm_offset;
    if (!rsdp_phys || !hhdm_offset) return 0;
    struct rsdp_v2 *r = phys_ptr(rsdp_phys);
    if (*(const uint64_t *)r != ACPI_SIG_RSDP || !checksum_ok(r, sizeof(struct rsdp_v1))) return 0;
    uint64_t root_phys = r->v1.rsdt_address;
    int wide = 0;
    if (r->v1.revision >= 2 && r->length >= sizeof(*r) && r->length <= 4096U && checksum_ok(r, r->length) && r->xsdt_address) {
        root_phys = r->xsdt_address; wide = 1;
    }
    struct sdt_header *root = phys_ptr(root_phys);
    if (root->signature != (wide ? 0x54445358U : 0x54445352U)) return 0; /* XSDT / RSDT */
    struct sdt_header *fadt = find_table(root, ACPI_SIG_FACP, wide);
    if (!fadt || fadt->length < 90) return 0;
    const uint8_t *f = (const uint8_t *)fadt;
    uint32_t flags = *(const uint32_t *)(f + 112);
    if (flags & ACPI_FADT_HW_REDUCED) return 0;
    g_pm1a_evt = (uint16_t)*(const uint32_t *)(f + 56);
    g_pm1b_evt = (uint16_t)*(const uint32_t *)(f + 60);
    g_pm1a_cnt = (uint16_t)*(const uint32_t *)(f + 64);
    g_pm1b_cnt = (uint16_t)*(const uint32_t *)(f + 68);
    g_evt_len = f[88]; g_cnt_len = f[89];
    if (!g_pm1a_evt || !g_pm1a_cnt || g_evt_len < 4 || g_cnt_len < 2) return 0;

    uint64_t dsdt_phys = *(const uint32_t *)(f + 40);
    if (fadt->length >= 148) {
        uint64_t xdsdt = *(const uint64_t *)(f + 140);
        if (xdsdt) dsdt_phys = xdsdt;
    }
    struct sdt_header *dsdt = phys_ptr(dsdt_phys);
    if (!find_s5(dsdt)) return 0;

    /* Enable ACPI mode when firmware exposes the legacy SMI command. */
    uint32_t smi_cmd = fadt->length >= 53 ? *(const uint32_t *)(f + 48) : 0;
    uint8_t acpi_enable = fadt->length >= 53 ? f[52] : 0;
    if (!(inw(g_pm1a_cnt) & ACPI_PM1_CNT_SCI_EN) && smi_cmd && acpi_enable) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (uint32_t i = 0; i < 1000000U; ++i) {
            if (inw(g_pm1a_cnt) & ACPI_PM1_CNT_SCI_EN) break;
            __asm__ volatile("pause");
        }
    }
    if (!(inw(g_pm1a_cnt) & ACPI_PM1_CNT_SCI_EN)) return 0;

    /* Clear stale power-button status, then enable the fixed event. */
    outw(g_pm1a_evt, ACPI_PM1_STS_PWRBTN);
    outw((uint16_t)(g_pm1a_evt + 2), (uint16_t)(inw((uint16_t)(g_pm1a_evt + 2)) | ACPI_PM1_EN_PWRBTN));
    g_ready = 1;
    return 1;
}

int acpi_power_available(void) { return g_ready; }

int acpi_power_button_poll(void) {
    if (!g_ready) return 0;
    uint16_t status = inw(g_pm1a_evt);
    if (!(status & ACPI_PM1_STS_PWRBTN)) return 0;
    outw(g_pm1a_evt, ACPI_PM1_STS_PWRBTN); /* W1C */
    acpi_poweroff();
    return 1;
}

void acpi_poweroff(void) {
    if (!g_ready) return;
    __asm__ volatile("cli" ::: "memory");
    uint16_t a = inw(g_pm1a_cnt);
    a &= (uint16_t)~ACPI_PM1_CNT_SLP_TYP_MASK;
    a |= (uint16_t)(g_s5a << 10) | ACPI_PM1_CNT_SLP_EN;
    if (g_pm1b_cnt) {
        uint16_t b = inw(g_pm1b_cnt);
        b &= (uint16_t)~ACPI_PM1_CNT_SLP_TYP_MASK;
        b |= (uint16_t)(g_s5b << 10) | ACPI_PM1_CNT_SLP_EN;
        outw(g_pm1b_cnt, b);
    }
    outw(g_pm1a_cnt, a);
    for (;;) __asm__ volatile("hlt");
}
