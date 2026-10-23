/* SPDX-License-Identifier: MIT */
#include <qcom_reboot.h>
#include <qcom_dt.h>
#include <psci.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char blob[16384] __attribute__((aligned(8)));
static struct {
    uint32_t dload, version, dload_values[8];
    uint8_t reason, reason_values[8];
    uint32_t imem_reason;
    struct { uint64_t address; uint32_t value; } imem_hist[16];
    uint32_t pshold;
    uint8_t pon_regs[0x100];
    struct { uint16_t offset; uint8_t value; } pon_hist[16];
    unsigned pon_hist_count, pon_reads, pon_writes, pon_delays;
    unsigned fail_pon_write, pon_unavailable;
    uint32_t dload_scm_values[4];
    unsigned dload_scm_count;
    unsigned dload_tcsr_values[4], dload_tcsr_count;
    unsigned smc_calls, dload_reads, dload_writes, reason_reads, reason_writes;
    unsigned imem_reads, imem_writes, imem_hist_count, pshold_scm;
    unsigned cold, warm, oem_warm, prepared, kona;
    unsigned fail_dload_write, fail_reason_write, corrupt_dload_read;
    unsigned fail_imem_write, corrupt_imem_read;
    int no_scm, no_dload, no_reset2, has_oem, has_pshold_scm, fail_prepare,
        interrupt, resumed, fail_after_write;
} fake;

static int node(const char *path)
{
    int n = fdt_path_offset(blob, path);
    assert(n >= 0);
    return n;
}
static void add(const char *parent, const char *name)
{
    assert(fdt_add_subnode(blob, node(parent), name) >= 0);
}
static void string(const char *path, const char *name, const char *value)
{
    assert(!fdt_setprop_string(blob, node(path), name, value));
}
static void cells(const char *path, const char *name, const uint32_t *values, unsigned count)
{
    fdt32_t values_be[8];
    assert(count <= 8);
    for (unsigned i = 0; i < count; ++i) values_be[i] = cpu_to_fdt32(values[i]);
    assert(!fdt_setprop(blob, node(path), name, values_be, count * 4));
}
#define CELLS(path, name, ...) do { const uint32_t v[] = {__VA_ARGS__}; \
    cells(path, name, v, sizeof(v) / sizeof(v[0])); } while (0)
#define PMIC "/arbiter/spmi/pmic"
#define SDAM PMIC "/sdam"
#define CELL SDAM "/restart"

/* Field hierarchy reproduced from the actual Sun (SM8750-class, "qcom,sunp")
 * hardware DT: a flat (non-nested) SPMI arbiter whose PMIC child owns the
 * SDAM holding the reboot-reason cell, plus a /soc identity bus. */
#define SUN_SOC "/soc"
#define SUN_SYSCON SUN_SOC "/syscon@1fc0000"
#define SUN_SCM "/firmware/qcom_scm"
#define SUN_SPMI SUN_SOC "/qcom,spmi@c42d000"
#define SUN_PMIC SUN_SPMI "/qcom,pmk8550@0"
#define SUN_SDAM SUN_PMIC "/sdam@7100"
#define SUN_CELL SUN_SDAM "/restart@48"
#define SUN_REASON SUN_SOC "/reboot_reason"

static void fixture(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.dload = 0x30; fake.reason = 0xa5; fake.version = 0x10001;
    assert(!fdt_create_empty_tree(blob, sizeof(blob)));
    string("/", "compatible", "qcom,canoep");
    CELLS("/", "#address-cells", 2); CELLS("/", "#size-cells", 2);
    add("/", "syscon"); string("/syscon", "compatible", "syscon");
    CELLS("/syscon", "reg", 0, 0x1fc0000, 0, 0x30000);
    CELLS("/syscon", "phandle", 2);
    add("/", "scm"); string("/scm", "compatible", "qcom,scm");
    CELLS("/scm", "qcom,dload-mode", 2, 0x19000);
    add("/", "arbiter");
    string("/arbiter", "compatible", "qcom,canoe-spmi-pmic-arb");
    add("/arbiter", "spmi"); CELLS("/arbiter/spmi", "qcom,bus-id", 0);
    add("/arbiter/spmi", "pmic"); string(PMIC, "compatible", "qcom,spmi-pmic");
    CELLS(PMIC, "reg", 0, 0); CELLS(PMIC, "#address-cells", 1);
    CELLS(PMIC, "#size-cells", 0);
    add(PMIC, "sdam"); string(SDAM, "compatible", "qcom,spmi-sdam");
    CELLS(SDAM, "reg", 0x7100); CELLS(SDAM, "#address-cells", 1);
    CELLS(SDAM, "#size-cells", 1);
    add(SDAM, "restart"); CELLS(CELL, "reg", 0x48, 1);
    CELLS(CELL, "bits", 1, 7); CELLS(CELL, "phandle", 3);
    add("/", "reason"); string("/reason", "compatible", "qcom,reboot-reason");
    CELLS("/reason", "nvmem-cells", 3);
    string("/reason", "nvmem-cell-names", "restart_reason");
}

/* A present but empty ranges property: /soc is identity-mapped onto the root
 * 2/2-cell bus. dt_reg() rejects an absent ranges, so it must exist. */
static void empty_prop(const char *path, const char *name)
{
    assert(!fdt_setprop(blob, node(path), name, NULL, 0));
}

static void sun_fixture(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.dload = 0x30; fake.reason = 0xa5; fake.version = 0x10001;
    assert(!fdt_create_empty_tree(blob, sizeof(blob)));
    string("/", "compatible", "qcom,sunp");
    CELLS("/", "#address-cells", 2); CELLS("/", "#size-cells", 2);
    add("/", "soc");
    CELLS(SUN_SOC, "#address-cells", 1); CELLS(SUN_SOC, "#size-cells", 1);
    empty_prop(SUN_SOC, "ranges");
    add(SUN_SOC, "syscon@1fc0000");
    string(SUN_SYSCON, "compatible", "syscon");
    CELLS(SUN_SYSCON, "reg", 0x1fc0000, 0x30000);
    CELLS(SUN_SYSCON, "phandle", 2);
    add("/", "firmware"); add("/firmware", "qcom_scm");
    string(SUN_SCM, "compatible", "qcom,scm");
    CELLS(SUN_SCM, "qcom,dload-mode", 2, 0x19000);
    add(SUN_SOC, "qcom,spmi@c42d000");
    string(SUN_SPMI, "compatible", "qcom,spmi-pmic-arb");
    CELLS(SUN_SPMI, "qcom,bus-id", 0);
    CELLS(SUN_SPMI, "qcom,channel", 0);
    CELLS(SUN_SPMI, "qcom,ee", 0);
    CELLS(SUN_SPMI, "#address-cells", 2); CELLS(SUN_SPMI, "#size-cells", 0);
    add(SUN_SPMI, "qcom,pmk8550@0");
    string(SUN_PMIC, "compatible", "qcom,spmi-pmic");
    CELLS(SUN_PMIC, "reg", 0, 0);
    CELLS(SUN_PMIC, "#address-cells", 1); CELLS(SUN_PMIC, "#size-cells", 0);
    add(SUN_PMIC, "sdam@7100");
    string(SUN_SDAM, "compatible", "qcom,spmi-sdam");
    CELLS(SUN_SDAM, "reg", 0x7100);
    CELLS(SUN_SDAM, "#address-cells", 1); CELLS(SUN_SDAM, "#size-cells", 1);
    add(SUN_SDAM, "restart@48");
    CELLS(SUN_CELL, "reg", 0x48, 1);
    CELLS(SUN_CELL, "bits", 1, 7); CELLS(SUN_CELL, "phandle", 3);
    add(SUN_SOC, "reboot_reason");
    string(SUN_REASON, "compatible", "qcom,reboot-reason");
    CELLS(SUN_REASON, "nvmem-cells", 3);
    string(SUN_REASON, "nvmem-cell-names", "restart_reason");
}

/* Kona (SM8250) IMEM reboot reason: an identity-mapped /soc bus holding the
 * qcom,msm-imem node whose offset-only child is the restart_reason register. */
#define KONA_SOC "/soc"
#define KONA_IMEM KONA_SOC "/qcom,msm-imem@146bf000"
#define KONA_REASON KONA_IMEM "/restart_reason@65c"

/* PM8150 PON 默认寄存器模型：GEN2 主 PON（子类型 4），spare 的 bit0 为 1，
 * 表示其余位（重启原因）初始为 0。 */
static void pon_fixture(void)
{
    memset(fake.pon_regs, 0, sizeof(fake.pon_regs));
    fake.pon_regs[0x05] = 0x04;
    fake.pon_regs[0x01] = 0x02;
    fake.pon_regs[0x8f] = 0x01;
    fake.pshold = 0xfeedf00d;
}

static void kona_fixture(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.version = 0x10001; fake.kona = 1;
    fake.imem_reason = 0xa5a5a5a5;
    pon_fixture();
    assert(!fdt_create_empty_tree(blob, sizeof(blob)));
    string("/", "compatible", "qcom,kona-mtp");
    CELLS("/", "#address-cells", 2); CELLS("/", "#size-cells", 2);
    add("/", "soc");
    CELLS(KONA_SOC, "#address-cells", 1); CELLS(KONA_SOC, "#size-cells", 1);
    CELLS(KONA_SOC, "ranges", 0, 0, 0, 0xffffffff);
    add(KONA_SOC, "qcom,msm-imem@146bf000");
    string(KONA_IMEM, "compatible", "qcom,msm-imem");
    CELLS(KONA_IMEM, "reg", 0x146bf000, 0x1000);
    CELLS(KONA_IMEM, "ranges", 0, 0x146bf000, 0x1000);
    CELLS(KONA_IMEM, "#address-cells", 1); CELLS(KONA_IMEM, "#size-cells", 1);
    add(KONA_IMEM, "restart_reason@65c");
    string(KONA_REASON, "compatible", "qcom,msm-imem-restart_reason");
    CELLS(KONA_REASON, "reg", 0x65c, 4);
    /* TCSR ps-hold/boot-misc-detect pair, as in the real Kona DT. */
    add(KONA_SOC, "restart@c264000");
    string(KONA_SOC "/restart@c264000", "compatible", "qcom,pshold");
    CELLS(KONA_SOC "/restart@c264000", "reg",
          0xc264000, 4, 0x1fd3000, 4);
}

int kshim_psci_init(const void *fdt) { assert(fdt == blob); return 0; }
kshim_psci_conduit_t kshim_psci_conduit(void) { return KSHIM_PSCI_CONDUIT_SMC; }
uint32_t kshim_psci_version(void) { return fake.version; }
int64_t kshim_psci_features(uint32_t fid)
{
    assert(fid == 0xc4000012); return fake.no_reset2 ? -1 : 0;
}
int64_t kshim_psci_request_system_reset(void)
{
    ++fake.cold;
    if (!fake.kona) {
        assert(fake.dload == 0);
        assert(fake.reason >= 2 && fake.reason <= 5);
    }
    return 0; /* Even an unexpected success return must roll back. */
}
int64_t kshim_psci_call(uint32_t fid, uint64_t type, uint64_t cookie, uint64_t unused)
{
    assert(fid == 0xc4000012 && type == 0 && cookie == 0 && unused == 0);
    /* The SM8750 EDL path raises the dload syscon first; the Kona path writes
     * IMEM and always reaches the warm reset through SYSTEM_RESET2. */
    if (fake.kona)
        assert(fake.cold == 0 && fake.warm == 0);
    else
        assert(fake.dload == 1);
    ++fake.warm;
    return -3;
}
struct kshim_resources *kshim_resources_get(void) { return NULL; }

/* IMEM-layout memory-mapped I/O boundary: the Kona path touches the
 * restart_reason, dload_type and (optional) download_mode registers. */
#define KONA_REASON_ADDR UINT64_C(0x146bf65c)
#define KONA_PSHOLD_ADDR UINT64_C(0xc264000)

static uint32_t *imem_slot(uint64_t address)
{
    if (address == KONA_REASON_ADDR) return &fake.imem_reason;
    if (address == KONA_PSHOLD_ADDR) return &fake.pshold;
    assert(0);
    return NULL;
}
int kshim_qcom_reboot_mmio_read(uint64_t address, uint32_t *value)
{
    assert(value);
    ++fake.imem_reads;
    *value = *imem_slot(address);
    if (fake.imem_reads == fake.corrupt_imem_read) *value ^= 0x4;
    return 0;
}
int kshim_qcom_reboot_mmio_write(uint64_t address, uint32_t value)
{
    if (address == KONA_PSHOLD_ADDR) {
        fake.pshold = value;
        return 0;
    }
    ++fake.imem_writes;
    assert(fake.imem_hist_count < 16);
    fake.imem_hist[fake.imem_hist_count].address = address;
    fake.imem_hist[fake.imem_hist_count].value = value;
    ++fake.imem_hist_count;
    if (fake.imem_writes == fake.fail_imem_write) return -1;
    *imem_slot(address) = value;
    return 0;
}

/* PM8150 PON 寄存器模型（SID 0，偏移 0x800..0x8ff）。 */
int kshim_qcom_pon_spmi_read(uint8_t sid, uint16_t offset, uint8_t *value)
{
    assert(sid == 0 && offset >= 0x800 && offset <= 0x8ff && value);
    ++fake.pon_reads;
    if (fake.pon_unavailable) return -1;
    *value = fake.pon_regs[offset & 0xff];
    return 0;
}
int kshim_qcom_pon_spmi_write(uint8_t sid, uint16_t offset, uint8_t value)
{
    assert(sid == 0 && offset >= 0x800 && offset <= 0x8ff);
    ++fake.pon_writes;
    assert(fake.pon_hist_count < 16);
    fake.pon_hist[fake.pon_hist_count].offset = offset;
    fake.pon_hist[fake.pon_hist_count].value = value;
    ++fake.pon_hist_count;
    if (fake.pon_writes == fake.fail_pon_write) return -1;
    fake.pon_regs[offset & 0xff] = value;
    return 0;
}
void kshim_qcom_pon_delay_us(uint32_t microseconds)
{
    assert(microseconds == 500);
    ++fake.pon_delays;
}

static void check_config(const struct kshim_qcom_reboot_config *c)
{
    assert(c->fdt == blob && c->dload_address == 0x1fd9000);
    assert(c->controller == 0 && c->bus == 0 && c->sid == 0);
    assert(c->reason_address == 0x7148 && c->reason_mask == 0xfe);
}
int kshim_qcom_reboot_spmi_prepare(const struct kshim_qcom_reboot_config *c)
{
    check_config(c); ++fake.prepared; return fake.fail_prepare ? -1 : 0;
}
int kshim_qcom_reboot_spmi_read(const struct kshim_qcom_reboot_config *c, uint8_t *value)
{
    check_config(c); ++fake.reason_reads; *value = fake.reason; return 0;
}
int kshim_qcom_reboot_spmi_write(const struct kshim_qcom_reboot_config *c, uint8_t value)
{
    check_config(c);
    assert(fake.reason_writes < 8);
    fake.reason_values[fake.reason_writes++] = value;
    if (fake.reason_writes == fake.fail_reason_write) return -1;
    fake.reason = value; return 0;
}
void kshim_qcom_reboot_smccc(uint64_t r[8])
{
    ++fake.smc_calls;
    uint64_t fid = r[0];
    if (fid == 0xc2000601) {
        assert(r[1] == 1 && (r[2] == 0x02000601 || r[2] == 0x02000501 ||
                             r[2] == 0x02000502 || r[2] == 0x03000922 ||
                             r[2] == 0x02000110 || r[2] == 0x02000902));
        r[0] = 0;
        if (r[2] == 0x02000110)
            r[1] = fake.no_dload ? 0 : 1;
        else if (r[2] == 0x02000902)
            r[1] = fake.has_pshold_scm ? 1 : 0;
        else
            r[1] = r[2] == 0x03000922 ? fake.has_oem : !fake.no_scm;
        return;
    }
    /* Kona（IMEM 布局）的 availability 查询：非 atomic SMC64 的 INFO 调用。 */
    if (fid == 0x42000601) {
        assert(r[1] == 1 && (r[2] == 0x02000110 || r[2] == 0x02000902 ||
                             r[2] == 0x02000502));
        r[0] = 0;
        if (r[2] == 0x02000110)
            r[1] = fake.no_dload ? 0 : 1;
        else if (r[2] == 0x02000902)
            r[1] = fake.has_pshold_scm ? 1 : 0;
        else
            r[1] = fake.no_scm ? 0 : 1;
        return;
    }
    if (fid == 0xc2000902) {
        assert(fake.has_pshold_scm && r[1] == 1 && r[2] == 0 && r[3] == 0);
        ++fake.pshold_scm; r[0] = 0; return;
    }
    if (fid == 0xc2000110) {
        assert(r[1] == 2 && (r[2] == 0x50 || r[2] == 0));
        assert(fake.dload_scm_count < 4);
        fake.dload_scm_values[fake.dload_scm_count++] = (uint32_t)r[2];
        r[0] = 0; return;
    }
    if (fid == 0xc2000502 && r[2] == 0x1fd3000) {
        assert(fake.kona && r[1] == 2 && (r[3] == 0x50 || r[3] == 0));
        assert(fake.dload_tcsr_count < 4);
        fake.dload_tcsr_values[fake.dload_tcsr_count++] = (uint32_t)r[3];
        r[0] = 0; return;
    }
    if (fid == 0xc3000922) {
        assert(fake.has_oem && fake.dload == 1);
        for (unsigned i = 1; i < 8; ++i) assert(r[i] == 0);
        ++fake.oem_warm; r[0] = (uint64_t)-3; return;
    }
    if (fid == 1) {
        assert(fake.interrupt && !fake.resumed && r[6] == 0xabcd);
        assert(r[1] == 1 && r[2] == 0x1fd9000 && r[3] == 0);
        fake.resumed = 1; fid = 0xc2000501;
    } else if (fid == 0xc2000501 && fake.interrupt && !fake.resumed) {
        r[0] = 1; r[1] = 99; r[2] = 99; r[3] = 99; r[6] = 0xabcd; return;
    }
    assert(r[2] == 0x1fd9000);
    if (fid == 0xc2000501) {
        assert(r[1] == 1); ++fake.dload_reads;
        r[0] = 0; r[1] = fake.dload;
        if (fake.dload_reads == fake.corrupt_dload_read) r[1] ^= 0x10;
    } else {
        assert(fid == 0xc2000502 && r[1] == 2 && fake.dload_writes < 8);
        fake.dload_values[fake.dload_writes++] = (uint32_t)r[3];
        if (fake.dload_writes == fake.fail_dload_write) {
            if (fake.fail_after_write) fake.dload = (uint32_t)r[3];
            r[0] = (uint64_t)-5; return;
        }
        fake.dload = (uint32_t)r[3]; r[0] = 0;
    }
}

/* The target/preflight/rollback contracts are layout-independent: the Canoe
 * and Sun DTs resolve to the same literal config, so the same assertions run
 * against either fixture through the setup parameter. */
static void test_targets(void (*setup)(void))
{
    setup();
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.cold == 1 && !fake.warm && fake.reason_values[0] == 3);
    assert(fake.dload_values[0] == 0 && fake.dload_values[1] == 0x30);
    assert(fake.reason == 0xa5 && fake.dload == 0x30);
    setup();
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_BOOTLOADER) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.reason_values[0] == 5 && fake.reason_values[1] == 0xa5);
    setup(); fake.reason = 0xa4;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.reason_values[0] == 2 && fake.reason == 0xa4);
    setup(); fake.interrupt = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.warm == 1 && !fake.cold && !fake.prepared && !fake.reason_writes);
    assert(fake.dload_values[0] == 1 && fake.dload == 0x30 && fake.resumed);
    setup(); fake.has_oem = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.oem_warm == 1 && !fake.warm && !fake.cold && fake.dload == 0x30);
    setup(); fake.has_oem = 1; fake.version = 0x10000;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.oem_warm == 1 && !fake.warm && fake.dload == 0x30);
}

static void test_preflight_and_rollback(void (*setup)(void))
{
    setup(); fake.version = 0x10000;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) == KSHIM_QCOM_REBOOT_UNSUPPORTED);
    assert(!fake.dload_writes && !fake.reason_writes && !fake.warm && !fake.oem_warm);
    setup(); fake.no_reset2 = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) == KSHIM_QCOM_REBOOT_UNSUPPORTED);
    assert(!fake.dload_writes && !fake.reason_writes);
    setup(); fake.no_scm = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) == KSHIM_QCOM_REBOOT_UNSUPPORTED);
    assert(!fake.dload_writes && !fake.reason_writes);
    setup(); fake.fail_prepare = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) == KSHIM_QCOM_REBOOT_IO_FAILED);
    assert(!fake.dload_writes);
    setup(); fake.fail_dload_write = 1; fake.fail_after_write = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) == KSHIM_QCOM_REBOOT_IO_FAILED);
    assert(fake.dload_writes == 2 && fake.dload == 0x30 && !fake.warm);
    setup(); fake.corrupt_dload_read = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) == KSHIM_QCOM_REBOOT_VERIFY_FAILED);
    assert(fake.dload == 0x30 && !fake.warm);
    setup(); fake.fail_reason_write = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) == KSHIM_QCOM_REBOOT_IO_FAILED);
    assert(fake.reason_writes == 2 && fake.reason == 0xa5 && fake.dload == 0x30 && !fake.cold);
    setup(); fake.fail_reason_write = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) == KSHIM_QCOM_REBOOT_ROLLBACK_FAILED);
    assert(fake.reason == 3 && fake.dload == 0x30);
    setup(); fake.fail_dload_write = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) == KSHIM_QCOM_REBOOT_ROLLBACK_FAILED);
    assert(fake.dload == 1);
}

static void test_dt_validation(void)
{
    struct kshim_qcom_reboot_config c;
    fixture(); assert(!kshim_qcom_reboot_parse(blob, &c)); check_config(&c);
    /* Addresses follow the supplied DT rather than board constants. */
    CELLS("/syscon", "reg", 0, 0x2200000, 0, 0x30000);
    CELLS(SDAM, "reg", 0x7200);
    assert(!kshim_qcom_reboot_parse(blob, &c));
    assert(c.dload_address == 0x2219000 && c.reason_address == 0x7248);
    fixture(); string("/", "compatible", "qcom,sm8750");
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_UNSUPPORTED);
    fixture(); CELLS("/scm", "qcom,dload-mode", 2, 0x30000);
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    fixture(); CELLS("/scm", "qcom,dload-mode", 2, 3);
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    fixture(); CELLS(CELL, "bits", 0, 8);
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    fixture(); CELLS(CELL, "reg", 0x48, 2);
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    fixture(); string("/arbiter", "status", "disabled");
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    fixture(); CELLS("/reason", "nvmem-cells", 999);
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) == KSHIM_QCOM_REBOOT_INVALID_DT);
    assert(!fake.smc_calls && !fake.dload_writes && !fake.reason_writes);
}

/* Sun success: exact root compatible qcom,sunp with the unchanged DT,
 * capability, version and rollback checks must resolve to the same config.
 * Old production rejects it with KSHIM_QCOM_REBOOT_UNSUPPORTED (-100); the
 * stderr line below records that observation before the assertion trips. */
static void test_sun_parse(void)
{
    struct kshim_qcom_reboot_config c;
    sun_fixture();
    const char *root = fdt_getprop(blob, 0, "compatible", NULL);
    int rc = kshim_qcom_reboot_parse(blob, &c);
    fprintf(stderr, "sun fixture root=%s parse rc=%d (expected 0 = success; "
            "want dload=0x1fd9000 controller=0 bus=0 sid=0 reason=0x7148 "
            "mask=0xfe shift=1)\n", root ? root : "(none)", rc);
    assert(rc == 0);
    assert(c.fdt == blob && c.dload_address == 0x1fd9000);
    assert(c.controller == 0 && c.bus == 0 && c.sid == 0);
    assert(c.reason_address == 0x7148 && c.reason_mask == 0xfe &&
           c.reason_shift == 1);
    assert(!fake.smc_calls && !fake.prepared && !fake.reason_reads &&
           !fake.reason_writes && !fake.dload_reads && !fake.dload_writes);
}

static void test_sun_invalid_dt(void)
{
    struct kshim_qcom_reboot_config c;
    /* Bad bits: not the published <1 7> reboot-reason window. */
    sun_fixture(); CELLS(SUN_CELL, "bits", 0, 8);
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    assert(!fake.smc_calls && !fake.dload_writes && !fake.reason_writes);
    /* Disabled flat controller: no enabled SPMI bus owns the SDAM. */
    sun_fixture(); string(SUN_SPMI, "status", "disabled");
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    assert(!fake.smc_calls && !fake.dload_writes && !fake.reason_writes);
    /* Missing reason phandle: nvmem-cells names an absent restart cell. */
    sun_fixture(); CELLS(SUN_REASON, "nvmem-cells", 999);
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_INVALID_DT);
    assert(!fake.smc_calls && !fake.dload_writes && !fake.reason_writes);
}

/* Kona IMEM layout: parse maps the registers, and every target writes the
 * documented cookie, verifies it and reaches the warm reset before rolling
 * back. The IMEM twin of the Canoe/Sun target contract. */
static void test_kona(void)
{
    struct kshim_qcom_reboot_config c;
    kona_fixture();
    assert(!kshim_qcom_reboot_parse(blob, &c));
    assert(c.layout == KSHIM_QCOM_REBOOT_LAYOUT_IMEM);
    assert(c.imem_reason_address == KONA_REASON_ADDR);
    assert(c.imem_pshold_address == KONA_PSHOLD_ADDR);
    assert(!fake.imem_reads && !fake.imem_writes && !fake.smc_calls && !fake.prepared);

    /* Recovery: 写 0x77665502、校验、写 PON 重启原因（GEN2 bit1），配置
     * 暖复位并拉低 PS_HOLD；复位返回后撤销 PON 并回滚 IMEM。 */
    kona_fixture();
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.imem_hist[0].address == KONA_REASON_ADDR &&
           fake.imem_hist[0].value == 0x77665502);
    assert(fake.imem_reason == 0xa5a5a5a5);
    assert(fake.pshold == 0 && fake.pon_delays == 1);
    assert(fake.pon_hist_count == 6);
    assert(fake.pon_hist[0].offset == 0x88f && fake.pon_hist[0].value == 0x03);
    assert(fake.pon_hist[1].offset == 0x85b && fake.pon_hist[1].value == 0x00);
    assert(fake.pon_hist[2].offset == 0x85a && fake.pon_hist[2].value == 0x01);
    assert(fake.pon_hist[3].offset == 0x85b && fake.pon_hist[3].value == 0x80);
    assert(fake.pon_hist[4].offset == 0x85b && fake.pon_hist[4].value == 0x00);
    assert(fake.pon_hist[5].offset == 0x88f && fake.pon_hist[5].value == 0x01);
    assert(fake.pon_regs[0x8f] == 0x01);
    /* PON 路径成功时不再走 PSCI（硬复位会清掉 cookie）。 */
    assert(!fake.warm && !fake.cold && !fake.oem_warm && !fake.prepared);
    assert(!fake.dload_scm_count && !fake.dload_tcsr_count);

    /* PS_HOLD 拉低优先走 SCM 安全调用；返回后仍直写 TCSR 兜底。 */
    kona_fixture(); fake.has_pshold_scm = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.pshold_scm == 1 && fake.pshold == 0 && !fake.warm && !fake.cold);

    /* Fastboot: 写 0x77665500，PON 原因为 2（GEN2 0x04）。 */
    kona_fixture();
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_BOOTLOADER) ==
           KSHIM_QCOM_REBOOT_RESET_RETURNED);
    assert(fake.imem_hist[0].address == KONA_REASON_ADDR &&
           fake.imem_hist[0].value == 0x77665500);
    assert(fake.pon_hist[0].offset == 0x88f && fake.pon_hist[0].value == 0x05);
    assert(fake.imem_reason == 0xa5a5a5a5 && fake.pshold == 0);
    assert(!fake.warm && !fake.cold);

    /* EDL (9008) 在 elish 上仍不可用，相关推测机制已移除：直接报告不支持，
     * 不写 IMEM、不碰 PON 或 PSCI。 */
    kona_fixture();
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_EDL) ==
           KSHIM_QCOM_REBOOT_UNSUPPORTED);
    assert(!fake.imem_writes && fake.pshold == 0xfeedf00d && !fake.pon_writes &&
           !fake.warm && !fake.cold);

    /* PON 不可用时退回 PSCI 链，并报告 -106。 */
    kona_fixture(); fake.pon_unavailable = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_PON_UNAVAILABLE);
    assert(fake.warm == 1 && fake.cold == 1);
    assert(fake.imem_reason == 0xa5a5a5a5 && fake.pshold == 0xfeedf00d);
    assert(!fake.pon_writes && fake.pon_delays == 0);

    /* PON 寄存器写失败时退回 PSCI 链，并报告 -107。写入原因后失败也要恢复。 */
    kona_fixture(); fake.fail_pon_write = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_PON_FAILED);
    assert(fake.warm == 1 && fake.cold == 1);
    assert(fake.pon_regs[0x8f] == 0x01 && fake.pshold == 0xfeedf00d);

    /* Without SYSTEM_RESET2, the plain system reset is the fallback. */
    kona_fixture(); fake.no_reset2 = 1; fake.pon_unavailable = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_PON_UNAVAILABLE);
    assert(fake.cold == 1 && !fake.warm && fake.imem_reason == 0xa5a5a5a5);

    /* Failed verify rolls back the attempted register. Reads for recovery are
     * the initial snapshot (1) and the verify (2). */
    kona_fixture(); fake.corrupt_imem_read = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_VERIFY_FAILED);
    assert(fake.imem_reason == 0xa5a5a5a5 && !fake.warm &&
           fake.pshold == 0xfeedf00d && !fake.pon_writes);

    /* Failed write is included in the rollback span: the first reason write
     * fails, the rollback write restores the old value. */
    kona_fixture(); fake.fail_imem_write = 1;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_IO_FAILED);
    assert(fake.imem_reason == 0xa5a5a5a5 && !fake.warm &&
           fake.pshold == 0xfeedf00d);

    /* Rollback failure is reported distinctly. */
    kona_fixture(); fake.fail_imem_write = 2;
    assert(kshim_qcom_reboot_request(blob, KSHIM_QCOM_REBOOT_RECOVERY) ==
           KSHIM_QCOM_REBOOT_ROLLBACK_FAILED);
    assert(fake.imem_reason == 0x77665502 && !fake.warm && fake.pshold == 0);

    /* Missing the restart_reason node is an invalid DT, never a reset. */
    kona_fixture(); assert(!fdt_del_node(blob, node(KONA_REASON)));
    assert(kshim_qcom_reboot_parse(blob, &c) == KSHIM_QCOM_REBOOT_INVALID_DT);
    assert(!fake.warm && !fake.cold && !fake.imem_writes);
}

static int test_hardware_dtb(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length < (long)sizeof(struct fdt_header) || length > INT_MAX ||
        fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "Invalid DTB file length\n");
        fclose(file); return 1;
    }
    void *hardware = malloc((size_t)length);
    if (!hardware) { fclose(file); return 1; }
    size_t bytes = fread(hardware, 1, (size_t)length, file);
    int close_result = fclose(file);
    if (bytes != (size_t)length || close_result || dt_validate(hardware, bytes)) {
        fprintf(stderr, "Cannot read a complete valid DTB\n");
        free(hardware); return 1;
    }

    /* Parse-only regression: never enter request(), PSCI, SCM or SPMI. */
    struct kshim_qcom_reboot_config c;
    int result = kshim_qcom_reboot_parse(hardware, &c);
    if (result) {
        fprintf(stderr, "Hardware DT reboot parse failed: %d\n", result);
        free(hardware); return 1;
    }
    int matches;
    if (c.layout == KSHIM_QCOM_REBOOT_LAYOUT_IMEM) {
        printf("Hardware DT reboot parse (IMEM): reason=0x%llx pshold=0x%llx\n",
               (unsigned long long)c.imem_reason_address,
               (unsigned long long)c.imem_pshold_address);
        matches = c.imem_reason_address == 0x146bf65c &&
                  c.imem_pshold_address == 0xc264000;
    } else {
        printf("Hardware DT reboot parse: dload=0x%llx controller=%u bus=%u sid=%u "
               "reason=0x%04x mask=0x%02x shift=%u\n",
               (unsigned long long)c.dload_address, (unsigned)c.controller,
               (unsigned)c.bus, (unsigned)c.sid, (unsigned)c.reason_address,
               (unsigned)c.reason_mask, (unsigned)c.reason_shift);
        matches = c.controller == 0 && c.dload_address == 0x1fd9000 &&
                  c.bus == 0 && c.sid == 0 &&
                  c.reason_address == 0x7148 && c.reason_mask == 0xfe &&
                  c.reason_shift == 1;
    }
    assert(!fake.smc_calls && !fake.prepared && !fake.reason_reads &&
           !fake.reason_writes && !fake.cold && !fake.warm && !fake.oem_warm);
    free(hardware);
    if (!matches) {
        fprintf(stderr, "Hardware DT reboot values differ from the supported "
                        "board DT evidence\n");
        return 1;
    }
    puts("qcom_reboot_test: hardware DT parse-only regression passed");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--hardware-dtb"))
        return test_hardware_dtb(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--hardware-dtb PATH]\n", argv[0]);
        return 2;
    }
    test_targets(fixture); test_preflight_and_rollback(fixture);
    test_dt_validation();
    /* Run the Sun parse-success check before its shared target/rollback
     * checks so a rejecting build prints the diagnostic and fails here. */
    test_sun_parse(); test_sun_invalid_dt();
    test_targets(sun_fixture); test_preflight_and_rollback(sun_fixture);
    test_kona();
    puts("qcom_reboot_test: DT, target ABI, capability checks and rollback passed");
    return 0;
}
