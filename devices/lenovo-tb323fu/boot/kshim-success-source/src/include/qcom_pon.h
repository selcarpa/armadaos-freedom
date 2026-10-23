/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Kona 的 PM8150 PON 外围位于 SPMI SID 0、偏移 0x800。下游 qpnp-power-on
 * 驱动用这里的寄存器把 PS_HOLD 拉低配置成暖复位（warm reset）：暖复位保留
 * IMEM / DDR 内容，bootloader 才能看到刚写入的 restart_reason。 */
#define KSHIM_QCOM_PON_BASE 0x800U
#define KSHIM_QCOM_PON_SID 0U

enum kshim_qcom_pon_result {
    KSHIM_QCOM_PON_OK = 0,
    /* SPMI 资源或 PON 子类型不可用，调用方应退回 PSCI 复位链。 */
    KSHIM_QCOM_PON_UNAVAILABLE = -1,
    /* PON 寄存器读写或回读校验失败。 */
    KSHIM_QCOM_PON_IO_FAILED = -2,
};

struct kshim_qcom_pon_state {
    bool armed;
    bool reason_changed;
    uint8_t old_reason;
    uint16_t reset_enable;
};

/* 探测 PON 子类型（0x805）。仅支持 elish 实机验证过的 PM8150 GEN2 主 PON；
 * 其它子类型返回 KSHIM_QCOM_PON_UNAVAILABLE。成功返回 0。 */
int kshim_qcom_pon_probe(void);

/* 拉低 PS_HOLD 前最多写入一次的 PON 重启原因：
 *   reason < 0  不写
 *   reason >= 0 写入 SOFT_RB_SPARE（恢复=1、fastboot=2，与下游枚举一致）。
 * 成功后 PON 已配置为暖复位且 PS_HOLD 已拉低，正常情况不会返回；若返回 0
 * 表示复位没有发生，调用方必须用 kshim_qcom_pon_abort() 撤销配置。 */
int kshim_qcom_pon_warm_reset(uint64_t pshold_address, int reason,
                              struct kshim_qcom_pon_state *state);

/* 复位未发生时的回滚：撤销 RESET_EN 并恢复 PON 重启原因旧值。 */
void kshim_qcom_pon_abort(struct kshim_qcom_pon_state *state);

/* 弱边界：host 测试替换为寄存器模型。默认实现走 CrDK SPMI。 */
int kshim_qcom_pon_spmi_read(uint8_t sid, uint16_t offset, uint8_t *value);
int kshim_qcom_pon_spmi_write(uint8_t sid, uint16_t offset, uint8_t value);
void kshim_qcom_pon_delay_us(uint32_t microseconds);
