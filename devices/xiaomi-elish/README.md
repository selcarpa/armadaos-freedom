# 小米平板 5 Pro（elish）

ArmadaOS Freedom 为小米平板 5 Pro（elish / SM8250）提供 kShimC 多系统启动适配。本页记录
elish 的适配状态、硬件调研与构建入口。当前以 Android-only 启动菜单（M1）为主要里程碑。

| 项目 | 配置 |
|---|---|
| 型号 | Xiaomi Pad 5 Pro（elish，BOE / CSOT 两种面板） |
| SoC | Qualcomm SM8250（Kona） |
| 设备树标识 | 运行时 ABL DT：`qcom,kona-mtp` + model `… xiaomi elish`；mainline DTS：`xiaomi,elish`、`qcom,sm8250` |
| boot 头版本 | **v3**（header_size 1580，128 MiB 分区） |
| 内核 | ArmadaOS Freedom 统一 Linux 7.2.6（mainline 已含 elish DTS 与 nt36523 面板驱动） |
| runtime 配置 | `elish_defconfig`（kShimC 共享源码树） |
| 系统更新 | 尚未接入（`armada-kshim-sync` 仅匹配四代 Elden） |

## 功能状态

| 功能 | 当前状态 |
|---|---|
| Android-only boot 打包（v3） | 离线验证 PASS（128 项检查），`hardware_tested: false` |
| kShimC 启动菜单 | **实机验证通过**：全屏横屏菜单、音量/电源键可选、超时进 Android |
| 原厂 Android 启动 | **实机验证通过**（ramdisk 字节保留，经 `linux,initrd-*` 传递） |
| 显示 | **实机验证通过**：原生竖屏 1600×2560 + `CONFIG_KSHIM_UI_ROTATION_CCW=270`（见下节） |
| 设备信息页 | **实机验证通过**：Board `Mi Pad 5 Pro (elish)`、SoC `SM8250` |
| 高级启动动作 Recovery / Fastboot（M1 fix） | **实机验证通过**：PM8150 PON 暖复位 + IMEM `restart_reason`（见下节） |
| EDL / 9008 | **已知遗留**：菜单项保留，但直接返回“不支持”（`-100`），见下节 |
| 触屏 | 调研完成：Novatek NVT SPI（`novatek,NVT-ts-spi`，QUP4 SPI，GPIO39 中断，带手写笔），待驱动接入 |
| 音频 | 调研完成：WCD938x + 4× Cirrus CS35L41（I2C 0x40/41/42/43），待布线 |
| 机身 Linux（M2） | 统一内核已重建，`sm8250-xiaomi-elish-{boe,csot}.dtb` 已进产物并含 handoff 适配 |

## 显示与设备信息（M1 已验证）

### 运行时 DT 的真实形态

elish 的 stock boot.img 是 **header v3**（没有独立 dtb 段），运行时 DT 由 ABL 现场生成，
因此和 mainline DTS 的根标识**不一致**：

| 来源 | 根 compatible | 根 model |
|---|---|---|
| ABL 运行时 DT（kShimC 实际拿到的） | `qcom,kona-mtp` | `Qualcomm Technologies, Inc. xiaomi elish` |
| mainline Linux DTS（M2 用） | `xiaomi,elish` `qcom,sm8250` | Xiaomi Mi Pad 5 Pro |

`device_info_read()` 按**实际 ABL DT** 识别（`src/dt/device_info.c`）：

- **SoC = SM8250**：compatible 有 `qcom,sm8250`，或任一条目以 `qcom,kona` 为前缀（Kona = SM8250）。
- **Board = elish**：唯一允许用 model 文本的例外——必须同时满足「Kona 家族 compatible」
  与「model 含 `elish`」两个证据，避免把其它 Kona 板误判。
- 设备信息页因此显示 Board `Mi Pad 5 Pro (elish)`、SoC `SM8250`；Model / Compatible /
  RAM / Framebuffer 仍原样显示 DT 值。
- 该识别结果同时被 `boot_settings` 接受（`detected_valid()` 已包含
  `DEVICE_BOARD_ELISH`），因此默认启动项/会话设置可以正常保存；设置页显示板名
  `Mi Pad 5 Pro`、板级代码 `elish`。

### 帧缓冲与旋转

- 面板 K81（双 CPHY video）**原生竖屏 1600×2560**，在横屏机身里旋转 90° 安装；
  SDE 两个 SSPP 各取 800 列、共用同一块缓冲，`YSTRIDE` 为整行 6400 字节。
- 逻辑分辨率必须由 `stride / bpp` 推导（`SRC_SIZE` 只报单 pipe 的 800 宽），
  否则菜单只占左半屏。
- `CONFIG_KSHIM_UI_ROTATION_CCW=270`：LVGL 以 2560×1600 横屏排版，写帧缓冲时旋转
  （`90` 会上下颠倒，已实机排除）。
- **live scanout 回退**（`CONFIG_KSHIM_SPLASH_LIVE_FALLBACK`，仅 elish 开启）：elish 上
  `CrSplashRead` 可能“成功”但返回没人扫描的 `cont_splash_region`，因此直接读每个 SSPP 的
  `SRC_SIZE/SRC0_ADDR/YSTRIDE/FORMAT`，只保留地址、尺寸、stride、格式都合理的缓冲；
  双 DSI 半宽 split 的两路若共用同一块缓冲，会合并成整屏，逻辑宽度最终取 `stride / bpp`。
  回退缓冲按非缓存映射，未使用的 splash reservation 复用为 UI scratch。

### 高级启动动作：Recovery / Fastboot / EDL（M1 fix）

M1 的高级动作要在 Android-only 菜单里直接切换启动模式。elish 的运行时 DT（ABL 现场生成）
根 compatible 是 `qcom,kona-mtp` + `qcom,kona`，**没有** `qcom,scm` 与
`qcom,reboot-reason` 节点，因此不能复用四代（Elden）的 SCM + SPMI/SDAM 路径。实现按根
compatible 分派（`src/drivers/qcom_reboot.c`）：

- 根 compatible 为 `qcom,kona-mtp`、`qcom,kona` 或 `qcom,sm8250` → **Kona IMEM 布局**（本节）；
- 其它（Canoe/Sun 等四代板）→ 原有 SCM + SPMI/SDAM 布局，解析/请求代码未改动。

Kona 路径使用与 mainline `msm-poweroff.c` 相同的 IMEM 协议：

| 目标 | 写入位置 | 值 |
|---|---|---|
| Recovery | IMEM `0x146bf65c`（`restart_reason`） | `0x77665502` |
| Fastboot | IMEM `0x146bf65c`（`restart_reason`） | `0x77665500` |
| EDL | —（暂不支持） | — |

- 地址来自运行时 DT 的 `qcom,msm-imem-restart_reason` 子节点（`0x146bf65c`，经节点的
  `ranges` 翻译）；写入后立即回读校验，校验失败会回滚原值。
- TCSR `qcom,pshold`（`0xC264000`）是**可选**节点；缺失时按 PON 不可用处理（`-106`），
  不会猜测地址。

> **EDL（9008）为已知遗留问题（M1 fix 范围内不解决）**：早期尝试过 IMEM
> `download_mode` cookie（基址 `+0x00`，属推断）、`dload_type`、SCM 安全下载
> （`0xc2000110` 参数 `0x50`）与 TCSR 兜底，均未经验证且实机三轮都只重启回 Android。
> 为避免保留未验证的推测代码，这些实现已全部移除；`imem_request()` 对 EDL 直接返回
> `-100`，不会写 IMEM、PON 或 PSCI。菜单的 9008 项仍保留。后续排查方向：反汇编
> `xbl.elf` 确认其真正读取的 IMEM 偏移（或 `misc` BCB），或改用硬件按键/EDL 线。
- **复位必须是 PMIC 暖复位**：普通 PSCI `SYSTEM_RESET` 是硬复位，会清空 IMEM，bootloader
  只能重新进 Android（前几轮实机三模式全部回 Android 已证实）。改用原厂
  `qpnp-power-on` 的暖复位路径（`src/drivers/qcom_pon.c`）：
  - 先探测 SPMI SID 0 的 PM8150 PON：`PERPH_SUBTYPE(0x805) == 0x04`（GEN2 主 PON）；
    其它子类型或 SPMI 资源缺失都视为不可用。
  - 写 PON 重启原因 `SOFT_RB_SPARE(0x88F)`：GEN2 从 bit1 起，Recovery=1、Fastboot=2；
    写后回读，失败则恢复旧值。
  - 按下游顺序配置 `PS_HOLD_RST_CTL(0x85A)` / `PS_HOLD_RST_CTL2(0x85B)`：
    清 `RESET_EN` → 延时 500µs → 类型写 `0x1`（warm）→ 置 `RESET_EN`。
  - 最后拉低 TCSR `PS_HOLD`（`0xC264000`，取自 `qcom,pshold`）触发；与下游一样先尝试
    SCM `SCM_SVC_PWR/2`（`SCM_DEASSERT_PS_HOLD`，TCSR 可能只允许安全世界写），调用返回后
    再直写兜底。
- 只有 PON 路径不可用（SPMI 资源缺失、PON 子类型无效或寄存器写失败）时才退回
  PSCI `SYSTEM_RESET2` → 普通 `SYSTEM_RESET`，保证设备至少能重启；此时屏上会显示
  `-106` / `-107`。PON 配置成功后**绝不再触发硬复位**；若拉低 `PS_HOLD` 后函数仍返回，
  则撤销 PON 配置、回滚 IMEM，并报 `-104`。
- Android-only 配置没有调试串口，失败时把数值码追加到状态栏，例如
  `切换启动模式失败 (-104)`，这是唯一的现场反馈。

| 码 | 含义 |
|---|---|
| -100 | 不支持（elish 的 EDL 固定返回；其它布局还可能是 PSCI conduit 非 SMC） |
| -101 | DT 无效（找不到 IMEM 地址） |
| -102 | MMIO 读写失败 |
| -103 | 写后回读校验失败 |
| -104 | 复位调用返回（没有真正复位），已撤销 PON 并回滚 |
| -105 | 回滚失败 |
| -106 | PON 暖复位不可用（SPMI 资源、`qcom,pshold` 缺失或 PON 探测失败），已回退 PSCI |
| -107 | PON 寄存器写入/校验失败，已回退 PSCI |

代码：`src/drivers/qcom_reboot.c`（布局分派、IMEM 路径、复位回退）、
`src/drivers/qcom_pon.c`（PM8150 PON 暖复位）、`src/include/qcom_reboot.h` / `qcom_pon.h`、
`src/core/platform.c`（屏上状态码）。host 回归：`qcom_reboot_test` 覆盖 IMEM
写入/校验/回滚、PON 寄存器序列（`0x88F`/`0x85A`/`0x85B`）、SCM PS_HOLD，以及 PON
不可用/写失败时的 PSCI 回退；`platform_advanced_menu_test`、
`platform_menu_console_off_test`、`boot_settings_test` 覆盖菜单与状态行。

```sh
cmake -S devices/lenovo-tb323fu/boot/kshim-success-source/tests -B build/host \
  -G Ninja -DCMAKE_C_COMPILER=clang
# 全量
ctest --test-dir build/host --output-on-failure
# 或只跑本页相关用例
ctest --test-dir build/host --output-on-failure \
  -R 'qcom_reboot|platform_advanced_menu|platform_menu_console_off|boot_settings'
```

## 硬件调研（来自官方 dtbo overlay）

`dtbo.img`（29 个 overlay）中 `xiaomi elish` 专属 overlay 的板级信息：

- **触屏**：`novatek,NVT-ts-spi` @ `qupv3_se4_spi`，`novatek,irq-gpio` GPIO39，`spi-max-frequency 19.2MHz`，
  支持 `novatek,pen-support` / `novatek,wgp-stylus`（手写笔），固件
  `novatek_nt36523_k81a_fw{01,02}_*.bin`（含 `_pen.bin`），`display-maker` 0x42/0x08/0x35
  （对应 BOE/CSOT/其它面板）。仓库已有 `0140` novatek nvt36xxx SPI 驱动与 TB323FU 的笔补丁（0167）可参考。
- **音频**：WCD938x codec（split-codec，RX/TX SoundWire）+ **4× Cirrus CS35L41** 功放
  （I2C 地址 0x40/0x41/0x43/0x42），QUAT MI2S（4 数据线 sd0..sd3）；`wsa_macro` 被禁用，
  实际扬声器不走 WSA SoundWire。CS35L41 驱动在 mainline 已有，需在 sm8250 sound card 上布线。
- **背光**：KTZ8866（`qupv3_se11_i2c` @0x11），与 mainline 一致。
- **电池**：双电芯（2× bq27z561），与 mainline 一致。

## 构建与打包

### runtime 配置（`elish_defconfig`）

| 配置 | 值 / 作用 |
|---|---|
| `CONFIG_QCOM_KEYS` / `CONFIG_QCOM_SPMI` | 音量/电源键（Kona 标准布局） |
| `CONFIG_KSHIM_SPLASH_LIVE_FALLBACK=y` | 只读 SSPP 现场 scanout，解决 elish 上 `cont_splash_region` 未被扫描的问题 |
| `CONFIG_KSHIM_UI_ROTATION_CCW=270` | LVGL 2560×1600 横屏排版 → 1600×2560 竖屏 scanout |
| `CONFIG_KSHIM_LVGL_BUFFER_BYTES=243712` | 按 2560 宽逻辑行分配，控制在 4 MiB runtime 槽内 |
| `CONFIG_KSHIM_MENU_ENTRY_DURATION_MS=0` | 首帧直接显示完整菜单，不做入场动画 |
| `CONFIG_KSHIM_MENU_STYLE_STAR_CHART` / `..._LANGUAGE_SWITCH` / `..._SESSION_SETTINGS` | 星图风格、中英切换、会话设置 |
| `CONFIG_KSHIM_MENU_CHINESE` / `..._ENTRY_DETAILS` / `..._BASE_ANDROID` / `..._POWER_ACTIONS` / `..._ADVANCED_ACTIONS` | 菜单页面与动作（含 Recovery/Fastboot/EDL） |
| `CONFIG_KSHIM_QCOM_REBOOT=y` | 高级启动动作（见上节） |
| `CONFIG_KSHIM_TOUCH` / `CONFIG_KSHIM_SMP` / `CONFIG_KSHIM_DEBUG_CONSOLE` | **关闭**：触屏待驱动接入；Android-only 没有调试串口 |

runtime 源码（`src/`、`startup.S`、MMU/驱动等）改动后必须重新生成 `kShimC.bin` 并重打包
boot，旧 runtime 产物不会反映新源码。直接交接 base 镜像的尾路径（`src/core/main.c`）
在跳转前调用 `kshim_mmu_prepare_handoff()` 关闭早期翻译，与正常 `prepare_boot` 交接保持一致。

### runtime（4 MiB）

```sh
# 在共享源码树 K = devices/lenovo-tb323fu/boot/kshim-success-source 内执行
python3 tools/build_runtime.py --config configs/elish_defconfig --out <runtime-out> --jobs 8
# 产物 <runtime-out>/kShimC.bin，必须恰好 4 MiB；validation.json 记录 sha256
```

### Android-only boot（v3，M1）

```sh
python3 devices/xiaomi-elish/boot/build_android_boot.py \
  --stock-boot <128MiB 原厂 boot.img> \
  --runtime   <4MiB kShimC.bin> \
  --packer    <MultiBootKernelPatcher> \
  --unpacker  <UnpackTool> \
  --avbtool   packages/kshim-tools/vendor/avbtool.py \
  --out       <输出目录> \
  [--repacker devices/xiaomi-elish/boot/repack_boot_v3.py] \
  [--dt-probe-report-slots] \
  [--source-label elish-android-only-v1]
```

- 输入约束：`--stock-boot` 必须是 128 MiB（134217728 字节）、魔数 `ANDROID!`、**header v3**
  （header_size 1580）、内核为 raw ARM64 Image（`ARM\x64`@0x38）、带 AVB footer。
- `--repacker` 默认就是 [repack_boot_v3.py](boot/repack_boot_v3.py)，一般无需显式传入；
  `--dt-probe-report-slots` 仅在需要把 DT 探针结果写入报告槽时启用，且要求原镜像带
  AVB footer。
- `--out` 必须是空目录，且不能与任何输入路径重叠（否则 `prepare` 阶段直接失败）。
- 打包驱动会做字节级校验（manifest ABI、unpack→repack 往返、AVB verify），一次通过产出
  `boot.img` + `report.json` + `repack_report.json`；失败则写 `failure.json`。`report.json`
  记录全部输入/输出的 sha256、manifest（默认条目 `Image-00`、`Timeout=8000`）、原厂
  ramdisk 保留情况、AVB 信息与检查项列表。
- **ramdisk 处理**：原厂 boot ramdisk（gzip，解压约 38 MiB cpio）由
  [repack_boot_v3.py](boot/repack_boot_v3.py) **字节级保留**在重打包 boot 的内核之后。Android
  内核经 bootloader DT 的 `/chosen/linux,initrd-*` 获取 ramdisk（kShimC 的 base 交接只传 DT，
  不传寄存器参数），因此 ramdisk 必须留在 boot.img 内，且 vendor_boot / dtbo / vbmeta 分区保持原样。
- AVB 均为未签名（Algorithm NONE）→ 刷入需解锁 bootloader，无法保留原厂小米签名。
- 产物 `hardware_tested: false`，只是离线验证，**不等于真机可用**。

## 刷入（实机验证）

本机适配阶段**只做临时启动，不刷写任何分区**：

```sh
# 前提：Mi Unlock 解锁 bootloader
fastboot boot <输出目录>/boot.img
```

`fastboot boot` 只把镜像加载到内存运行，重启即恢复原厂 boot；分区、vbmeta、dtbo 全部不动。
AVB 为未签名（Algorithm NONE），刷入分区需要解锁 bootloader。

实机验证清单（M1，均已通过）：

1. ~~菜单显示~~ → 全屏横屏菜单，无黑屏。
2. ~~音量/电源键导航、中英切换~~ → 正常。
3. ~~进入原厂 Android 正常~~ → 8 秒超时或选择后正常进入。
4. ~~菜单方向~~ → 原生竖屏 1600×2560 + `CONFIG_KSHIM_UI_ROTATION_CCW=270`，横屏正确。
5. ~~设备信息页~~ → Board `Mi Pad 5 Pro (elish)`、SoC `SM8250`。

M1 fix（高级启动动作，已实机验证；EDL 除外）：

6. Recovery / Fastboot 切换：PM8150 PON 暖复位（`qcom_pon.c`）**已实机验证**——
   **Recovery / Fastboot 均通过**（暖复位 + IMEM `restart_reason`），**EDL（9008）遗留**：
   三轮尝试（SCM `0x50`、SCM `0x10`+`EDLOAD`、IMEM download_mode cookie）均只回 Android，
   未验证的推测实现已移除，9008 菜单项返回“不支持”；待反汇编 `xbl.elf` 后另开课题。
   失败时按 `-104/-106/-107` 显示在状态栏。

原厂 boot.img 备份与 sha256 全程记录在 `/tmp/opencode/elish/stock/SHA256SUMS`；不要把
`~/Downloads/boot.img`（那是 TB322FC 的）与 elish 的混用。

## 内核（M2 前置）

`packages/kernel/dts/` 已 vendor `sm8250-xiaomi-elish-{common,boe,csot}`（原样拷贝 mainline
7.2.6）与 `sm8250-xiaomi-elish-common.dtsi.patch`（`chosen/framebuffer` 模板化 +
`cont_splash_region` label，供 kShimC simplefb 交接与 splash 发现）。统一内核已全量重建，
产物 `dtb/qcom/sm8250-xiaomi-elish-{boe,csot}.dtb`（127730 B）已确认含上述适配（compatible
`xiaomi,elish qcom,sm8250`、framebuffer 为 disabled 模板、`label = "cont_splash_region"`、
面板 compatible `xiaomi,elish-boe-nt36523` / `xiaomi,elish-csot-nt36523`）。注意：**统一内核构建
必须全量重建**（当前 series 末尾的 Kaanapali/Baldur 补丁 0209–0223 未在旧 FAST 缓存树上应用，
`FAST=1` 复用会在 config merge 阶段因 `CONFIG_LEDS_AW22127` 缺失而失败）。

## 后续里程碑

- M2 机身 Linux：vmlinuz + elish DTB + initramfs，Linux maintenance boot（5 条目）。
- M3 触屏：Novatek NVT SPI 驱动接入（参照 0140/0164–0167）+ runtime 触屏 + 桌面校准 + 笔。
- M4 音频：CS35L41 ×4 + WCD938x 在 sm8250-sndcard 布线 + ALSA UCM。
- M5 充电/电量（双电芯）、WiFi/BT、相机节点、安装流程与 OTA（`armada-kshim-sync` 需按
  `xiaomi,elish` 匹配）。

## 组件来源与许可

板级 DTS 原样取自 mainline Linux 7.2.6（`sm8250-xiaomi-elish-*`，Jianhua Lu，
BSD-3-Clause）；本项目的适配通过独立补丁维护。硬件调研来自官方固件包
`elish_images_OS1.0.2.0.TKYCNXM`（`dtbo.img` 中 elish overlay），仅作板级参考。
