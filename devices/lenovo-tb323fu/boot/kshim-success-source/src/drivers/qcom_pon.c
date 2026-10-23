/* SPDX-License-Identifier: MIT
 * Kona PM8150 PON warm reset.
 *
 * 下游 msm-poweroff 在 reboot 命令带原因（recovery/bootloader）时选择
 * PMIC 暖复位，因为普通 PSCI SYSTEM_RESET 是硬复位，会清掉 IMEM 里的
 * restart_reason，bootloader 只能重新进 Android。暖复位由 PON 的
 * PS_HOLD_RST_CTL/CTL2 配置，随后拉低 TCSR 的 PS_HOLD 触发。
 *
 * 寄存器布局来自下游 qpnp-power-on.c（PM8150 的 PON 位于 SPMI SID 0 的
 * 0x800），仅实现 elish 实机验证过的 GEN2 主 PON：
 *   0x805 PERPH_SUBTYPE
 *   0x85A PS_HOLD_RST_CTL      0x85B PS_HOLD_RST_CTL2
 *   0x88F SOFT_RB_SPARE（重启原因，GEN2 从 bit1 开始）
 */
#if !defined(KSHIM_HOST_TEST)
#include <config.h>
#endif
#include <qcom_pon.h>
#include <qcom_dt.h>
#include <qcom_reboot.h>
#include <string.h>
#include <Library/spmi.h>
#if defined(__aarch64__)
#include <timer.h>
#endif

#if CONFIG_KSHIM_QCOM_REBOOT

#define PON_PERPH_SUBTYPE 0x05
#define PON_PS_HOLD_RST_CTL 0x5A
#define PON_PS_HOLD_RST_CTL2 0x5B
#define PON_SOFT_RB_SPARE 0x8F
/* 下游 qpnp-power-on.h 的 PON_POWER_OFF_WARM_RESET / QPNP_PON_RESET_EN /
 * QPNP_PON_POWER_OFF_MASK。 */
#define PON_POWER_OFF_WARM_RESET 0x01
#define PON_RESET_EN 0x80
#define PON_POWER_OFF_MASK 0x0F
/* GEN1 子类型只有 1..3，GEN2（PM8150 等）是 4/5；其余值不是 PON。 */
#define PON_GEN2_PRIMARY 0x04

/* CrDK SPMI 事务使用的 32 位寄存器边界。只允许时钟/观察者窗口内、并且是
 * 当前事务命令或数据寄存器的写入，避免把通道配置写坏。 */
static struct {
    SpmiDeviceContext ctx;
    int ready, writing, fault;
    uintptr_t read_command, write_command, write_data;
} pon_spmi;

static int pon_region_contains(const SpmiMemoryRegion *region, uintptr_t address,
                               size_t bytes)
{
    return region->BaseAddress && region->Size >= bytes &&
           region->BaseAddress <= UINTPTR_MAX - (region->Size - 1) &&
           address >= region->BaseAddress &&
           address - region->BaseAddress <= region->Size - bytes &&
           !(address & 3U);
}

static int pon_address_valid(uintptr_t address)
{
    for (unsigned i = 0; i < SPMI_MEMORY_REGION_TYPE_MAX; ++i)
        if (pon_region_contains(&pon_spmi.ctx.Regions[i], address, 4))
            return 1;
    return 0;
}

static void pon_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static UINT32 pon_mmio_read(void *context, UINTN address)
{
    (void)context;
    if (!pon_address_valid(address)) {
        pon_spmi.fault = 1;
        return 0;
    }
    uint32_t value = *(volatile uint32_t *)address;
    pon_barrier();
    return value;
}

static void pon_mmio_write(void *context, UINTN address, UINT32 value)
{
    (void)context;
    if (!pon_address_valid(address) ||
        (address != pon_spmi.read_command &&
         !(pon_spmi.writing && (address == pon_spmi.write_command ||
                                address == pon_spmi.write_data)))) {
        pon_spmi.fault = 1;
        return;
    }
    pon_barrier();
    *(volatile uint32_t *)address = value;
    pon_barrier();
}

static void pon_stall(void *context, UINT32 microseconds)
{
    (void)context;
    kshim_qcom_pon_delay_us(microseconds);
}

__attribute__((weak)) void kshim_qcom_pon_delay_us(uint32_t microseconds)
{
#if defined(__aarch64__)
    (void)delay(microseconds);
#else
    (void)microseconds;
#endif
}

static int pon_spmi_prepare(void)
{
    const struct kshim_resources *resources = kshim_resources_get();
    if (!resources || resources->spmi_status || !resources->spmi_count)
        return -1;
    const SpmiDeviceContext *source = &resources->spmi[0].controller;
    memset(&pon_spmi, 0, sizeof(pon_spmi));
    memcpy(pon_spmi.ctx.Regions, source->Regions, sizeof(pon_spmi.ctx.Regions));
    pon_spmi.ctx.ActiveEE = source->ActiveEE;
    pon_spmi.ctx.Channel = source->Channel;
    pon_spmi.ctx.BusId = source->BusId;
    pon_spmi.ctx.Io = (SpmiIoOps){pon_mmio_read, pon_mmio_write, pon_stall, NULL};
    SpmiDeviceContext *context = &pon_spmi.ctx;
    if (CR_ERROR(SpmiLibInit(&context)) || pon_spmi.fault)
        return -1;
    pon_spmi.ready = 1;
    return 0;
}

static int pon_spmi_transfer(uint8_t sid, uint16_t offset, bool write,
                             uint8_t *value)
{
    if (!pon_spmi.ready && pon_spmi_prepare())
        return -1;
    SpmiDeviceContext *ctx = &pon_spmi.ctx;
    uint32_t channel = ctx->PmicArb.Ops->GetChannelOffset(
        ctx, sid, offset, write ? SPMI_ARB_CHANNEL_RW : SPMI_ARB_CHANNEL_OBS);
    uintptr_t base = write ? ctx->PmicArb.WriteAddr : ctx->PmicArb.ReadAddr;
    if (channel == UINT32_MAX || base == 0 ||
        base > UINTPTR_MAX - channel - SPMI_PMIC_ARB_WDATA0)
        return -1;
    pon_spmi.read_command = base + channel + SPMI_PMIC_ARB_CMD;
    pon_spmi.write_command = pon_spmi.read_command;
    pon_spmi.write_data = base + channel + SPMI_PMIC_ARB_WDATA0;
    pon_spmi.fault = 0;
    if (write) {
        pon_spmi.writing = 1;
        CR_STATUS status = SpmiWrite(ctx, sid, offset, value, 1);
        pon_spmi.writing = 0;
        return CR_ERROR(status) || pon_spmi.fault ? -1 : 0;
    }
    CR_STATUS status = SpmiRead(ctx, sid, offset, value, 1);
    return CR_ERROR(status) || pon_spmi.fault ? -1 : 0;
}

__attribute__((weak)) int kshim_qcom_pon_spmi_read(uint8_t sid, uint16_t offset,
                                                   uint8_t *value)
{
    if (!value) return -1;
    return pon_spmi_transfer(sid, offset, false, value);
}

__attribute__((weak)) int kshim_qcom_pon_spmi_write(uint8_t sid, uint16_t offset,
                                                    uint8_t value)
{
    return pon_spmi_transfer(sid, offset, true, &value);
}

static int pon_read(uint16_t offset, uint8_t *value)
{
    return kshim_qcom_pon_spmi_read(KSHIM_QCOM_PON_SID,
                                    (uint16_t)(KSHIM_QCOM_PON_BASE + offset),
                                    value);
}

static int pon_write(uint16_t offset, uint8_t value)
{
    return kshim_qcom_pon_spmi_write(KSHIM_QCOM_PON_SID,
                                     (uint16_t)(KSHIM_QCOM_PON_BASE + offset),
                                     value);
}

static int pon_masked_write(uint16_t offset, uint8_t mask, uint8_t value)
{
    uint8_t current;
    if (pon_read(offset, &current))
        return -1;
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return pon_write(offset, current);
}

int kshim_qcom_pon_probe(void)
{
    uint8_t subtype;
    if (pon_read(PON_PERPH_SUBTYPE, &subtype))
        return KSHIM_QCOM_PON_UNAVAILABLE;
    return subtype == PON_GEN2_PRIMARY ? KSHIM_QCOM_PON_OK :
                                        KSHIM_QCOM_PON_UNAVAILABLE;
}

int kshim_qcom_pon_warm_reset(uint64_t pshold_address, int reason,
                              struct kshim_qcom_pon_state *state)
{
    struct kshim_qcom_pon_state local;
    if (!state)
        state = &local;
    memset(state, 0, sizeof(*state));
    int probe = kshim_qcom_pon_probe();
    if (probe)
        return probe;
    /* PM8150 GEN2：RESET_EN 位于 PS_HOLD_RST_CTL2。 */
    state->reset_enable = PON_PS_HOLD_RST_CTL2;

    /* 重启原因先写：PON 的 spare 位在硬复位/暖复位后都保留（下游驱动同样
     * 在拉低 PS_HOLD 前写入）。GEN2 从 bit1 开始（bit0 保留）。 */
    if (reason >= 0) {
        uint8_t old;
        if (pon_read(PON_SOFT_RB_SPARE, &old))
            return KSHIM_QCOM_PON_IO_FAILED;
        state->old_reason = old;
        uint8_t mask = 0xFE;
        unsigned shift = 1U;
        uint8_t value = (uint8_t)((old & (uint8_t)~mask) |
                                  (((unsigned)reason << shift) & mask));
        if (value != old) {
            if (pon_write(PON_SOFT_RB_SPARE, value))
                return KSHIM_QCOM_PON_IO_FAILED;
            state->reason_changed = true;
            uint8_t actual;
            if (pon_read(PON_SOFT_RB_SPARE, &actual) || actual != value)
                return KSHIM_QCOM_PON_IO_FAILED;
        }
    }

    /* 与下游驱动一致：先清 RESET_EN，等 10 个睡眠时钟周期（500us），再写
     * 复位类型，最后置 RESET_EN。 */
    if (pon_masked_write(state->reset_enable, PON_RESET_EN, 0))
        return KSHIM_QCOM_PON_IO_FAILED;
    kshim_qcom_pon_delay_us(500);
    if (pon_masked_write(PON_PS_HOLD_RST_CTL, PON_POWER_OFF_MASK,
                         PON_POWER_OFF_WARM_RESET))
        return KSHIM_QCOM_PON_IO_FAILED;
    if (pon_masked_write(state->reset_enable, PON_RESET_EN, PON_RESET_EN))
        return KSHIM_QCOM_PON_IO_FAILED;
    state->armed = true;

    /* 拉低 PS_HOLD 即触发复位；与下游 deassert_ps_hold() 一致，先走 SCM
     * 安全调用，返回后再直写 TCSR 兜底。 */
    if (kshim_qcom_reboot_pshold(pshold_address))
        return KSHIM_QCOM_PON_IO_FAILED;
    return KSHIM_QCOM_PON_OK;
}

void kshim_qcom_pon_abort(struct kshim_qcom_pon_state *state)
{
    if (!state)
        return;
    if (state->armed) {
        (void)pon_masked_write(state->reset_enable, PON_RESET_EN, 0);
        state->armed = false;
    }
    if (state->reason_changed) {
        (void)pon_write(PON_SOFT_RB_SPARE, state->old_reason);
        state->reason_changed = false;
    }
}

#endif /* CONFIG_KSHIM_QCOM_REBOOT */
