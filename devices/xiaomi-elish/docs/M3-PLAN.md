# elish M3 实施计划：触屏与手写笔

> 状态：计划稿（2026-10-30）。设备：Xiaomi Pad 5 Pro（elish / SM8250 / Kona）。
> 前置：M2 已完成（5 条目 maintenance boot，CSOT 显示链路 + 原厂 Android 回退均实机通过）。
> 依据：[设备 README](../README.md)、官方 dtbo overlay、`nt36536` 内核驱动与 0164–0167
> 补丁、`device-env` / 桌面校准框架、kShimC runtime touch 代码。

## 1. 目标与验收

### 1.1 范围

M3 = 触屏与手写笔，分三层：

1. **Linux 内核层**：NVT SPI 触屏驱动绑定，多点触控、坐标范围正确；
2. **桌面层**：libinput/KWin 设备识别、旋转与校准矩阵、手写笔（tablet）行为；
3. **kShimC 运行时层**：启动菜单触摸（无键盘时也能操作）。

### 1.2 验收标准

**内核层（可在当前 M2 镜像上验证）**

- `/dev/input/event*` 出现 `NVTCapacitiveTouchScreen`（以及 `NVTCapacitivePen`）；
- 10 点触控、按下/移动/抬起事件完整，坐标覆盖四角与中心；
- 触摸坐标与显示方向一致（或校准后一致）；
- 笔：`BTN_TOUCH` / `BTN_TOOL_PEN` / `BTN_STYLUS` / `BTN_STYLUS2`、压力、倾斜、距离；
- 屏幕 off/on 与 suspend/resume 后触屏仍工作，无 IRQ storm。

**桌面层（需要可启动的 Linux rootfs）**

- `libinput list-devices` 把触屏列为 touchscreen，笔列为 tablet；
- `libinput debug-events` 手指/笔轨迹与屏幕一致，四角无偏移；
- Plasma 下点击、拖拽、双指滚动/缩放、长按正常；
- 笔在支持压感的绘图应用中可用，桶形按键有映射。

**运行时层（可选/独立阶段）**

- 在 kShimC 菜单里用触摸选择条目、翻页、确认；
- 触摸失败时仍可回退音量/电源键（不能因触摸初始化失败卡住菜单）。

### 1.3 非目标

音频（M4）、安装/OTA（M5）、触摸压力曲线调优、第三方笔兼容性矩阵。

## 2. 现状与事实

### 2.1 硬件（来自官方 dtbo overlay）

elish 的触摸节点在 `dtbo.img` 的 elish overlay（第 12 个 FDT）中：

| 项目 | 值 |
|---|---|
| 总线 | `qupv3_se4_spi`（mainline `spi4: spi@990000`） |
| SPI 引脚 | gpio8/9/10（`qup4`），CS gpio11 |
| 频率 | 19,200,000 Hz（`0x124f800`） |
| 中断 | gpio39（vendor `novatek,irq-gpio = <&tlmm 0x27 0x2001>`） |
| 复位 | **无** reset-gpio 属性（依赖上电/CS 时序） |
| 手写笔 | `novatek,pen-support`、`novatek,wgp-stylus` |
| 控制器 | Novatek，兼容 `novatek,NVT-ts-spi`，`reg = <0>` |
| 固件 | `novatek_nt36523_k81a_fw01/fw02.bin`，另有 `_pen.bin`、`_mp*.bin` |
| 面板区分 | `display-maker` 0x42 / 0x08 / 0x35，对应 cfg_0/1/2 |

参数 `novatek,swrst-n8-addr = <0x3f0fe>`、`novatek,spi-rd-fast-addr = <0x3f310>`。

### 2.2 mainline DTS / 内核

- 面板节点已有 label：`display_panel: panel@0`，`rotation = <90>`，
  `backlight = <&backlight>`；板级 DTS 打开 compatible `xiaomi,elish-{boe,csot}-nt36523`。
- **没有触摸节点**；`spi4` 在 `sm8250.dtsi` 中默认 `disabled`，elish DTS 未启用。
- pinctrl 已存在：`qup_spi4_data_clk`（gpio8/9/10，function `qup4`）、
  `qup_spi4_cs`（gpio11，function `qup4`）。
- 内核已有两个 Novatek 驱动：
  - `0140` `novatek-nvt36xxx-spi.c`，compatible **`novatek,nt36536-ts`**（不匹配 elish）；
  - `0164–0167` `nt36536/` host-download 驱动，compatible **`novatek,NVT-ts-spi`**
    （匹配 elish），含笔、面板 follower、坐标范围修复与 0167 笔按钮/分辨率补丁。
- 当前配置：`CONFIG_TOUCHSCREEN_NT36536_SPI=m`、`CONFIG_TOUCHSCREEN_NOVATEK_NVT36XXX_SPI=m`。

### 2.3 固件现状（重要）

- 在官方固件包中检索过 `super.img`（system/vendor/odm/product/mi_ext）、`vendor_boot.img`
  与 boot ramdisk，**没有找到** `novatek_nt36523_k81a_fw*.bin`；`vendor/firmware` 只有
  `NT36xxx_MP_Setting_Criteria_*.csv`。
- `nt36536` 驱动的固件下载是 **probe 之后的异步 work**；`request_firmware()` 失败只重试并
  记录日志，不会阻止 probe/输入上报。因此**可以先依赖 IC 内已有固件**做触摸 bring-up，
  固件更新作为后续可选项。
- 仓库现有 `system_files/usr/lib/firmware/novatek_ts_csot_3k_fw.bin` 是 TB323FU 的固件，
  不能用于 elish；固件分发还需确认许可（LICENSING.md 明确固件不在默认 GPL 覆盖内）。

### 2.4 桌面档案与校准框架

- `device-env` 按 DT `model` 选 `system_files/usr/lib/armada/devices/<profile>.conf`，
  目前没有 elish 分支；也没有 `xiaomi-elish.conf`。
- 已有字段：`ARMADA_PRIMARY_TOUCHSCREEN`、`ARMADA_DESKTOP_TOUCHSCREEN_CALIBRATION`、
  `ARMADA_PANEL_ORIENTATION`、`ARMADA_PRIMARY_CONNECTOR`。
- `desktop-bootstrap` 把 `ARMADA_PANEL_ORIENTATION` 转成 `kscreen-doctor rotation`，
  `armada_configure_desktop_touchscreen()` 把 4×4 校准矩阵写入 `kcminputrc` 并实时设置
  KWin `calibrationMatrix`（TB323FU 用单位矩阵）。
- 已有 `tests/desktop-touchscreen-test.sh` 覆盖 profile 选择与矩阵持久化，可直接扩展。

### 2.5 kShimC runtime touch

- `CONFIG_KSHIM_TOUCH` 实现只支持 **ST FTM5**（I2C），`r->touch.generation != 5` 直接跳过；
  elish 的 `elish_defconfig` 当前关闭。
- 但 runtime 已有 QUP/SPI 抽象：`st_ftm5.h` 定义 `KSHIM_FTM5_BUS_SPI`、
  `kshim_spi_transfer`（`CrGeniSpiTransfer`）、CS hold 等，SPI 传输层可复用。
- 缺口：Novatek 协议解析、`novatek,NVT-ts-spi` 的 DT 资源解析、菜单事件接入。

### 2.6 测试环境限制

M2 镜像只有 7.5 MiB 的最小 initramfs，没有桌面 rootfs；elish 的安装流程在 M5。
因此：

- **内核层**可用自定义 initramfs（evdev 测试程序）完整验证；
- **桌面层（libinput/Plasma/笔）**需要一块可启动的 Linux rootfs：等 M5，或临时用
  USB/外置介质启动一个测试 rootfs（elish USB-C 的 host 模式与供电需先验证）。

## 3. 方案

### 3.1 内核 DT 节点（草案）

新增 `dts/sm8250-xiaomi-elish-touch.dtsi`（或扩展现有 common dtsi patch）：

```dts
&spi4 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&qup_spi4_data_clk>, <&qup_spi4_cs>;

    touchscreen: touchscreen@0 {
        compatible = "novatek,NVT-ts-spi";
        reg = <0>;
        spi-max-frequency = <19200000>;

        pinctrl-names = "default";
        pinctrl-0 = <&ts_int_default>;

        irq-gpios = <&tlmm 39 GPIO_ACTIVE_HIGH>;
        novatek,pen-support;
        novatek,wgp-stylus;
        novatek,spi-rd-fast-addr = <0x3f310>;

        panel = <&display_panel>;
        firmware-name = "novatek/novatek_nt36523_k81a_fw02.bin";
    };
};

&tlmm {
    ts_int_default: ts-int-default-state {
        pins = "gpio39";
        function = "gpio";
        input-enable;
        bias-pull-up;
    };
};
```

板级 DTS 按面板覆盖 `firmware-name`（BOE/CSOT 各自 variant）。初始不加
`novatek,swap-xy`，坐标不对再加。

### 3.2 驱动与配置

- 使用 `nt36536`（`CONFIG_TOUCHSCREEN_NT36536_SPI`），不是 0140。
- bring-up 阶段建议改为 `=y`（内置），这样最小测试 initramfs 也能 probe；
  稳定后再评估是否回到 `=m`（由 rootfs 加载）。
- 需要面板 follower：`panel = <&display_panel>`，让驱动在 panel prepare 后初始化/恢复。
- 中断走 `irq-gpios`（驱动用 `devm_gpiod_get(dev, "irq")` + `gpio_to_irq`），
  vendor 的 `novatek,irq-gpio` 不是 mainline 属性。

### 3.3 固件策略

1. 先不安装固件，依赖 IC 内固件启动，验证触摸/笔基础功能；
2. 如固件版本过旧导致坐标/笔异常，再从设备侧取：
   `adb shell ls -l /vendor/firmware /odm/firmware /mnt/vendor/persist`（必要时 root），
   或从 `vendor_dlkm`/persist 提取；确认文件名与面板 variant；
3. 固件入库前确认分发许可；否则只提供本地提取脚本/文档，不把 bin 提交到仓库。

### 3.4 轴、旋转与校准

- 面板原生竖屏 1600×2560，`rotation = <90>`，桌面横屏 2560×1600；
- 先用测试 initramfs 读原始坐标，判断是否需要 `novatek,swap-xy` / 翻转；
- 桌面校准矩阵按 KWin 4×4 格式写入 `xiaomi-elish.conf`，候选（需实测确认）：
  - 0°：`1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1`
  - 90°：`0,-1,1,0,1,0,0,0,0,0,1,0,0,0,0,1`（或反向，取决于旋转方向）
- 用四角点击验证后固化到 profile；同时扩展
  `tests/desktop-touchscreen-test.sh` 断言 elish profile。

### 3.5 手写笔

- 0167 已声明 `BTN_STYLUS/BTN_STYLUS2` 与 ABS 分辨率，但标注“未用笔验证”；
- M3 需要实测：hover、压力、倾斜、桶形按键、笔尖/橡皮；
- 若 libinput 仍拒绝，检查 `INPUT_PROP_DIRECT`、`ABS_TILT_*`、分辨率与 `BTN_TOOL_PEN`；
- 桌面侧确认 Plasma/Krita 识别为 tablet，按键映射符合预期。

### 3.6 kShimC runtime touch（独立阶段，建议最后做）

- 新增 `src/drivers/novatek_spi.c`（协议 + 报告解析），复用 `CrQupInit`/`CrGeniSpiTransfer`
  与 `kshim_touch_update()` 的旋转/翻转/多点逻辑；
- `src/dt/touch.c` 增加 `novatek,NVT-ts-spi` 资源解析（QUP SPI、IRQ gpio39、无 reset）；
- `platform.c` 的 `start_touch()` 按 `r->touch.generation` 分派，新增 Novatek 分支；
- `elish_defconfig` 打开 `CONFIG_KSHIM_TOUCH`；
- host 回归沿用 `tests/touch_test.c` / `qcom_touch_platform_test.c` 的模式，加合成报文测试；
- 固件：runtime 不做 host-download，依赖 IC 内固件；失败必须回退按键并继续菜单。

### 3.7 测试 initramfs（内核层）

新增 `min_initramfs/init-touch-test.c`（或独立目录）：

- 枚举 `/dev/input/event*`，打印设备名、ABS 范围、支持的按键；
- 对触摸/笔事件画十字或圆点，坐标与压力打印到 fbcon / `/dev/kmsg`；
- 支持 `armada.reboot_after=<秒>`，方便自动退出；
- 用 `build_min_initramfs.py --source ...` 打包，配合 M2 的 5 条目 boot 打包器生成
  `boot-ktz-touch-test.img`。

这样不需要 rootfs 就能验证驱动、坐标、笔和 suspend 基本行为。

## 4. 工作分解

### M3.1 内核 DTS + 驱动启用（0.5–1 天）

- [ ] 新增 touch dtsi / 扩展 common dtsi patch；启用 `&spi4` + `touchscreen@0`；
- [ ] 板级 DTS 设置 BOE/CSOT 的 `firmware-name`；
- [ ] `CONFIG_TOUCHSCREEN_NT36536_SPI=y`（bring-up 阶段）；
- [ ] 全量重编内核，确认 `spi4`/驱动 probe 日志（`dmesg | grep -i nvt`）；
- [ ] `dtc` 反编译核对节点/中断/频率。

验收：`/dev/input` 出现触摸设备，`evtest` 有事件。

### M3.2 触摸测试 initramfs + 坐标验证（0.5–1 天）

- [ ] 编写 `init-touch-test.c`，打包测试镜像；
- [ ] 实测四角/中心、10 点、坐标范围；
- [ ] 判断 swap-xy/翻转需求，必要时打补丁；
- [ ] 记录原始坐标到屏幕坐标的映射，供校准矩阵使用。

验收：四角点击落在屏幕对应角，10 点同时上报。

### M3.3 桌面档案与校准（0.5–1 天）

- [ ] 新增 `system_files/usr/lib/armada/devices/xiaomi-elish.conf`：
      `ARMADA_DEVICE_ID=xiaomi-elish`、`ARMADA_SOC_CLASS=SM8250`、
      `ARMADA_PRIMARY_TOUCHSCREEN='NVTCapacitiveTouchScreen'`、
      `ARMADA_PANEL_ORIENTATION=right`（实测确认）、校准矩阵；
- [ ] `device-env` 增加 model 匹配 `Xiaomi Mi Pad 5 Pro (CSOT)/(BOE)`，并加
      `xiaomi,elish` compatible 兜底（与 elden 同模式）；
- [ ] 扩展 `tests/desktop-touchscreen-test.sh` 的 elish 断言；
- [ ] 若暂时没有 rootfs，先用测试 app 验证矩阵数学，rootfs 就绪后回归。

验收：`device-env` 输出正确 profile；矩阵在测试 app 中把四角映射正确。

### M3.4 手写笔（0.5–1 天）

- [ ] 测试 hover/压力/倾斜/按键；
- [ ] 如 libinput 拒绝，补 `INPUT_PROP_DIRECT`/分辨率/按键；
- [ ] 桌面就绪后验证 Plasma/Krita。

验收：笔被识别为 tablet，压力/按键可用。

### M3.5 电源管理与恢复（0.5 天）

- [ ] 屏幕 off/on、suspend/resume 后触摸恢复；
- [ ] 面板 follower 与 IRQ wake 配置；
- [ ] 无 IRQ storm/无法唤醒。

### M3.6 kShimC runtime touch（3–5 天，独立）

- [ ] Novatek SPI runtime 驱动 + DT 解析 + 菜单接入 + host 测试；
- [ ] 失败回退按键；
- [ ] 实机验证菜单触摸。

### M3.7 文档与提交（0.5 天）

- [ ] README 功能表/硬件节更新；
- [ ] 提交拆分：DTS/驱动配置、device profile/校准、测试 initramfs、runtime touch；
- [ ] 记录固件来源/许可与实机结果。

## 5. 验证命令

```sh
# 内核/DTB
dtc -I dtb -O dts -o /tmp/elish.dts <dtb>
grep -n "spi4\|novatek\|NVT-ts" /tmp/elish.dts

# 测试镜像
fastboot boot _build/elish-m3/boot-ktz-touch-test.img

# 有 rootfs 后
libinput list-devices
libinput debug-events
libinput debug-tablet
evtest
bash tests/desktop-touchscreen-test.sh

# 设备侧固件检查
adb shell ls -l /vendor/firmware /odm/firmware /mnt/vendor/persist 2>/dev/null
```

## 6. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 固件不在固件包里 | 只能靠 IC 内固件；版本旧可能坐标/笔异常 | 先无固件 bring-up；必要时从设备/persist 提取；更新失败不阻塞 probe |
| 面板 variant 映射不确定（0x42/0x08/0x35） | 用错固件可能初始化失败 | 先不下载固件；需要时按 BOE/CSOT 分别试 fw01/fw02 |
| 轴/旋转/校准矩阵错误 | 触摸方向或角落偏移 | 用测试 app 量化四角映射；矩阵写入 profile 前先离线验证 |
| 无 rootfs | 桌面/笔无法完整验收 | 内核层用测试 initramfs；桌面层等 M5 或临时 USB rootfs |
| runtime touch 工作量大 | M3 周期拉长 | 拆成独立阶段，先交付 Linux 触摸/笔，菜单触摸可后补 |
| `=y` vs `=m` | 模块在最小 initramfs 不加载 | bring-up 用 `=y`；稳定后评估 `=m` + rootfs |
| IRQ/CS 时序、无 reset | probe 失败或丢中断 | 先按 vendor 参数（gpio39、gpio11、19.2MHz）；必要时加 reset 时序/延时 |
| suspend/resume | 触摸不恢复 | 依赖 panel follower；测试屏幕 off/on 与 s2idle |

## 7. 待决问题

1. **runtime touch 是否纳入 M3**：建议拆成 M3 独立阶段，Linux 触摸/笔先交付。
2. **桌面验证路径**：等 M5，还是先做 USB/外置 rootfs？需要先验证 elish USB-C host 模式。
3. **固件策略**：是否可以接受“不更新固件”；若必须更新，固件能否随仓库分发。
4. **面板映射**：CSOT 对应 0x08/fw02 是否成立，需要实机读 panel ID 或试错确认。
5. **驱动内置/模块**：bring-up 后是否改回 `=m`。

## 8. 建议排期

- 第 1 周：M3.1 + M3.2 + M3.3（内核触摸、坐标、profile/矩阵）
- 第 2 周：M3.4 + M3.5（笔、电源），并准备桌面 rootfs 验证条件
- 第 3 周起：M3.6（runtime touch，可与 M4 并行）
