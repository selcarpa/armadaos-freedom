/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

enum kshim_qcom_reboot_target {
    KSHIM_QCOM_REBOOT_RECOVERY,
    KSHIM_QCOM_REBOOT_BOOTLOADER,
    KSHIM_QCOM_REBOOT_EDL,
};
enum kshim_qcom_reboot_error {
    KSHIM_QCOM_REBOOT_UNSUPPORTED = -100,
    KSHIM_QCOM_REBOOT_INVALID_DT = -101,
    KSHIM_QCOM_REBOOT_IO_FAILED = -102,
    KSHIM_QCOM_REBOOT_VERIFY_FAILED = -103,
    KSHIM_QCOM_REBOOT_RESET_RETURNED = -104,
    KSHIM_QCOM_REBOOT_ROLLBACK_FAILED = -105,
    /* Kona IMEM layout only: the PMIC PON warm reset could not be used, so the
     * call fell back to the PSCI chain (which is a hard reset). */
    KSHIM_QCOM_REBOOT_PON_UNAVAILABLE = -106,
    KSHIM_QCOM_REBOOT_PON_FAILED = -107,
};

/* Which hardware reboot-reason layout a parsed config describes:
 *   SCM_SDAM: SM8750/SM8850 Lenovo boards, an SCM dload syscon plus the SPMI
 *             SDAM restart-reason cell the stock bootloader reads.
 *   IMEM:     SM8250/Kona boards, the older IMEM restart_reason register the
 *             downstream msm-poweroff driver writes for reboot
 *             bootloader/recovery. */
enum kshim_qcom_reboot_layout {
    KSHIM_QCOM_REBOOT_LAYOUT_SCM_SDAM = 0,
    KSHIM_QCOM_REBOOT_LAYOUT_IMEM = 1,
};

struct kshim_qcom_reboot_config {
    const void *fdt;
    uint8_t layout; /* enum kshim_qcom_reboot_layout */
    /* SCM/SDAM layout. */
    uint64_t dload_address;
    uint16_t reason_address;
    uint8_t controller, bus, sid;
    uint8_t reason_shift, reason_mask;
    /* IMEM layout (zero when layout is SCM_SDAM). */
    uint64_t imem_reason_address;
    /* TCSR "pshold-base" from the qcom,pshold node; writing zero deasserts
     * PS_HOLD and triggers the reset type armed in the PMIC PON. 0 when the
     * board does not describe it. */
    uint64_t imem_pshold_address;
};

/* Caller supplies the already validated, immutable hardware DT used to
 * initialize resources. Only an explicit menu confirmation may call request.
 * Successful reset never returns; every returned value is a failure. */
int kshim_qcom_reboot_parse(const void *hardware_fdt,
                          struct kshim_qcom_reboot_config *config);
int kshim_qcom_reboot_request(const void *hardware_fdt,
                            enum kshim_qcom_reboot_target target);

/* Weak hardware boundaries allow host tests to observe exact transactions.
 * The default SPMI adapter permits only the DT-selected SDAM byte. */
void kshim_qcom_reboot_smccc(uint64_t registers[8]);
/* Weak 32-bit memory-mapped I/O boundary for the IMEM layout; the default
 * implementation touches the mapped register and host tests override it to
 * observe the exact transactions. */
int kshim_qcom_reboot_mmio_read(uint64_t address, uint32_t *value);
int kshim_qcom_reboot_mmio_write(uint64_t address, uint32_t value);
/* 拉低 PS_HOLD 触发 PMIC 复位。TCSR 可能只允许安全世界写，因此与下游
 * deassert_ps_hold() 一样先尝试 SCM（SCM_SVC_PWR/2），调用返回后再直写
 * 兜底。成功（写已提交）返回 0；返回即表示复位没有发生。 */
int kshim_qcom_reboot_pshold(uint64_t address);
int kshim_qcom_reboot_spmi_prepare(const struct kshim_qcom_reboot_config *config);
int kshim_qcom_reboot_spmi_read(const struct kshim_qcom_reboot_config *config,
                              uint8_t *value);
int kshim_qcom_reboot_spmi_write(const struct kshim_qcom_reboot_config *config,
                               uint8_t value);
