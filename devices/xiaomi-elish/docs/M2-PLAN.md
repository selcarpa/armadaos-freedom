# elish M2 实施计划：Linux maintenance boot

> 状态：计划稿（2026-10-30）。设备：Xiaomi Pad 5 Pro（elish / SM8250 / Kona）。
> 依据：[设备 README](../README.md)、AGENTS.md §5–§9，以及 2026-10-26 已实现并离线通过的
> M2 源码（源码在 10-30 工作区重置时被移入回收站，本计划以“恢复 + 补缺 + 重验 + 实机”为序）。
>
> **执行记录（2026-10-30）**：W1 / W2 / W3 / W5 已完成——6 个源文件已恢复，repack 三选项
> 已补齐，7 个单测全过；runtime / packer 已重建，`boot-csot`、`boot-boe`、`boot-csot-debug`、
> `boot-csot-probe30` 四个镜像与 Android-only 回归均离线 PASS（249 / 130 项检查）。
> W4 实机：首版内核进 Linux 后黑屏；用 timing/probe 镜像确认内核与 initramfs 正常（20 s
> 自动重启），定位为缺 `CONFIG_BACKLIGHT_KTZ8866`（nt36523 面板 `drm_panel_of_backlight()`
> 永久 `-EPROBE_DEFER`）。补编内核后 `boot-ktz-csot.img` 在 CSOT 实机点亮，彩条 + fbcon
> 文字验证通过；同镜像菜单回选原厂 Android 也正常启动，M2 验收完成。产物在
> `_build/elish-m2/`（`.gitignore` 的 `_build*`，不入库）。

## 1. 范围与验收

### 1.1 目标（取自 README「后续里程碑」）

M2 = 机身 Linux 的第一阶段：**vmlinuz + elish DTB + initramfs，Linux maintenance boot（5 条目）**。
即在一个 `boot.img` 里同时保留原厂 Android 与 Linux 维护入口，由 kShimC 菜单选择；
本阶段不安装 rootfs、不接触屏/音频。

### 1.2 交付物

| # | 交付物 | 说明 |
|---|---|---|
| D1 | `boot-csot.img` | CSOT 面板优先的 5 条目 maintenance boot |
| D2 | `boot-boe.img` | BOE 面板备选（仅 DTB 不同） |
| D3 | `initramfs.cpio.gz` / `initramfs-debug.cpio.gz` | 最小 canary；debug 版带 USB ACM + 定时重启 |
| D4 | `report.json` / `repack_report.json` | 每个 boot 的离线校验报告，记录全部输入输出 sha256 |
| D5 | 单元测试 | `test_m2_boot_v3.py`（5 例）、`test_m2_initramfs.py`（2 例） |
| D6 | 文档与提交 | 源码入仓；README 增加 M2 状态与构建命令 |

产物一律放在仓库外（建议 `/var/tmp/elish-m2-<date>/`，遵循 AGENTS §9），不入库。

### 1.3 验收标准

**离线（必须全过）**

- 两套 unittest PASS；
- `build_linux_boot_v3.py` 的 `report.json`：`offline_verified: true`，`checks` 无 `ok:false`，
  `boot.kernel_size == PackedKernel 长度`、ramdisk 字节保留、AVB `verify_image` 通过且算法 `NONE`；
- `repack_report.json`：`output_length`、`partition_limit`、`avb.partition_size/algorithm`、
  `ramdisk_preserved` 与 builder 断言一致；
- 输出镜像必须以 `AVBf` footer 结尾，且官方 `avbtool verify_image` 通过（Algorithm NONE）。

**实机（M2 的真正完成条件）**

- `fastboot boot boot-csot.img` 后屏上出现彩条 canary（或 debug 版从 USB ACM 看到
  `ARMADA-ELISH-M2-CANARY: userspace init reached`）；
- 菜单可回选「原厂 Android」并正常启动；
- `fastboot reboot` 后设备回原厂 boot（M2 阶段只 `fastboot boot`，不刷分区）。

**明确非目标**：rootfs 安装、UFS 分区、触屏、音频、WiFi/BT、OTA（属 M3–M5）。

## 2. 当前状态（2026-10-30）

### 2.1 已完成

- **M1**：Android-only v3 boot 已实机验证（菜单、显示、Recovery/Fastboot）。
- **M2 离线**：2026-10-26 已完整实现并全绿：
  - `/var/tmp/elish-m2/gates.json` 内 gate 全部 PASS（含 128 MiB 回归、192 MiB 精确填充、
    超限拒绝、v3 header、5 条目布局、raw budget、AVB、ramdisk）；
  - `/var/tmp/elish-m2/boot-csot-v4/report.json`：247/247 checks PASS。
- **内核（M2 前置）**：`/var/tmp/elish-fw-verify/kernel/lib/modules/7.2.6/`
  - `vmlinuz` 64,506,368 B，sha256 `5dc7256d…`；
  - `sm8250-xiaomi-elish-{boe,csot}.dtb` 各 127,730 B；
  - 已反编译核对：`framebuffer` 为 disabled 模板、`cont_splash_region` label 均在，
    与 `packages/kernel/dts/sm8250-xiaomi-elish-common.dtsi.patch` 一致。

### 2.2 当前仓库缺件

10-30 15:33 工作区重置后，M2 的 6 个源文件在仓库中消失（回收站保留），
`repack_boot_v3.py` 回退为 M2 之前的版本：

| 文件 | 现状 |
|---|---|
| `devices/xiaomi-elish/boot/build_linux_boot_v3.py` | 缺（回收站 55,801 B） |
| `devices/xiaomi-elish/boot/build_min_initramfs.py` | 缺（回收站 7,273 B） |
| `devices/xiaomi-elish/boot/min_initramfs/init.c` | 缺（回收站 7,751 B） |
| `devices/xiaomi-elish/boot/min_initramfs/init-debug.c` | 缺（回收站 15,560 B） |
| `devices/xiaomi-elish/tests/test_m2_boot_v3.py` | 缺（回收站 6,049 B） |
| `devices/xiaomi-elish/tests/test_m2_initramfs.py` | 缺（回收站 3,925 B） |
| `devices/xiaomi-elish/boot/repack_boot_v3.py` | 在，但缺 `--max-output-size` / `--avb-partition-size` / `--minimal-avb` |

已核对：回收站 4 个 `.py` 与 `devices/xiaomi-elish/**/__pycache__/*.pyc` 的 code object
（code/consts/names/varnames）完全一致，确认就是产出 10-26 通过产物的源码。

### 2.3 基线资产（重跑时可直接复用）

| 资产 | 路径 | 关键值 |
|---|---|---|
| stock boot | `/var/tmp/elish-fw-verify/boot.img` | 134,217,728 B；v3 / header 1580；kernel 54,835,216 B；ramdisk 19,848,135 B；sha256 `336f36fe…` |
| 内核 | 见 §2.1 | `vmlinuz` 64,506,368 B |
| DTB | 同上 | `sm8250-xiaomi-elish-csot.dtb` / `-boe.dtb` |
| M2 boot（参考） | `/var/tmp/elish-m2/boot-csot-v4/boot-csot-v4.img` | 157,024,256 B，sha256 `33df4e1b…` |
| 最小 initramfs | `/var/tmp/elish-m2/initrd/` | 259,816 B gz；cpio 534,040 B；limit 7,680,000 |

GPT 事实：`boot_a` / `boot_b` 均 196,608 KB = **192 MiB**，而原厂 `boot.img` 只有 128 MiB。

### 2.4 5 条目布局（已验证）

| # | 名称 | 类型 | flags | 对齐 | 偏移（参考产物） | 大小 |
|---|---|---|---|---|---|---|
| 0 | 原厂 Android | Linux | BASE\|EXEC | 2 MiB | 0 | 54,835,216 |
| 1 | Linux 维护模式 | Linux | EXEC\|HAS_DTB | 2 MiB | 71,303,168 | 65,404,928 |
| 2 | Elish DTB | DTB | — | 8 | 136,708,096 | 127,846 |
| 3 | M2 initramfs | Blob | — | 64 KiB | 136,839,168 | 259,816 |
| 4 | Attached Shim | Shim | EXEC | 4 | 66,207,744 | 4,194,304 |

默认条目 `Image-01`（Linux），超时 8000 ms；manifest 仍保留 Android 入口。

## 3. 源码盘点（恢复目标与改造点）

### 3.1 `build_linux_boot_v3.py`（55,801 B，恢复即可用）

**CLI**

```
--stock-boot --linux-image --dtb --initrd --bootargs --runtime
--packer --unpacker --avbtool [--repacker] --out [--source-label]
[--output-name boot.img] [--panel auto|csot|boe]
[--raw-profile conservative|xbl-evidence]
[--max-output-size N] [--pad-to-max-output]
```

**阶段**：`prepare → validate-inputs → dtb → config → pack → assert-pack → unpack →
assert-unpack → repack → assert-boot → avb → assert-repack-report → recheck-sources → report`。
每阶段写 `failure.json` 并用稳定的 check 名称记录，适合 CI 门禁。

**关键行为（实机正确性依赖）**

1. **DTB 派生**：只允许改 `/chosen` 的 `bootargs`、`armada,boot-stage`、
   `kshim,initrd-entry`、`linux,initrd-*`；写入 `bootargs` 与 `kshim,initrd-entry=3`；
   校验根 compatible 必须含 `xiaomi,elish` + `qcom,sm8250`，禁止 Elden/TB323FU/SM8750/SM8850/Kaanapali；
   通过面板 compatible（`xiaomi,elish-csot-nt36523` / `-boe-`）识别 panel。
2. **Linux Image 重定位**：mainline `vmlinuz` 的 `text_offset=0x0`，而原厂 Android Image 是
   `0x80000`；打包副本里把该 header 字段改成 `0x80000` 以满足 MBKP 的 2 MiB 放置规则，
   **源 vmlinuz 不改**（kShimC 启动条目时不用该字段）。
3. **Packer 配置**：5 个 section，`Image-01` 的 `DeviceTreeBlob=Image-02`、
   shim 由 packer 自动追加；`Align=21/3/16` 分别对应 2 MiB / 8 B / 64 KiB。
4. **Raw budget**：
   - `conservative`：128 MiB —— 当前打包内核 137,099,400 B **会直接失败**；
   - `xbl-evidence`：192 MiB —— 取自 XBL memory map 的
     `0xA0000000,0x10000000,"Kernel"` 256 MiB reservation，是“证据”而非实测
     （证据文本写进 report 的 `loader_budget`）。
5. **输出/AVB**：默认 `--max-output-size` = 192 MiB，配合 `--minimal-avb`；
   输出名不是 `boot.img` 时创建临时 symlink 供 `avbtool verify_image` 使用（因为
   avbtool 会按 hash descriptor 的 `partition_name` 去找 `boot.img`）。

### 3.2 `repack_boot_v3.py`（需补 3 个选项）

**现状**（HEAD 版本）：`limit = len(orig)` 同时充当“大小上限”和 `avbtool --partition_size`，
最终强制 `len(final) == len(orig)`；因此只能产出 128 MiB 镜像。

**M2 需要的语义**（从 10-26 的 pyc 反推，且被 `test_m2_boot_v3.py` 5 个用例锁定）：

| 选项 | 语义 | 默认 |
|---|---|---|
| `--max-output-size N` | 最终镜像大小上限；必须是 4096 倍数、≥ 原镜像长度；`N` 不能小于内容长度 | `len(orig)`（向后兼容） |
| `--avb-partition-size N` | 精确的 `avbtool --partition_size` 与最终文件长度；4096 倍数；不得大于 `--max-output-size`；与 `--minimal-avb` 互斥 | `len(orig)` |
| `--minimal-avb` | 取能容纳“内容 + avbtool 最大元数据预留”的最小 partition_size，不补齐到原镜像长度；与 `--avb-partition-size` 互斥 | 关 |

实测参考：`original_image_size = 156,954,624` → minimal `partition_size = 157,024,256`
（差值 69,632 B = 64 KiB + 4096，即 avbtool 元数据上界；builder 侧断言
`len(final) >= ois + 64 KiB + 4096`）。

**实现要点**

```
limit = args.max_output_size or len(orig)
target = args.avb_partition_size or (minimal ? calc_minimal(ois) : len(orig))
assert align(new_len) <= target <= limit
写 raw（补齐到 align(new_len)）→ avbtool add_hash_footer --partition_size target
final 长度 == target；report: output_length / partition_limit=limit / avb.partition_size=target
```

`report` 必须新增/保持 `avb.partition_size`、`avb.metadata_algorithm`（builder 会读取）。
不带任何新参数时必须与旧行为**逐字节一致**（128 MiB 输出），由 `test_default_keeps_128_style_length`
守护。

### 3.3 `build_min_initramfs.py` + `min_initramfs/init.c`、`init-debug.c`

- 编译单个静态 aarch64 `init`（`aarch64-linux-gnu-gcc -static`），生成确定性的 newc cpio +
  零 mtime gzip；条目固定：`.`、`dev/{console,kmsg,null,tty0}`、`etc/armada-m2-canary`、
  `init`、`proc`、`run`、`sys`、`tmp`；上限 `7,500 KiB`（7,680,000 B），输出 `report.json`。
- `init.c`：挂 devtmpfs/proc/sys、打印 canary、读 `/proc/cmdline`、按 `fb_var_screeninfo`
  在 framebuffer 上画彩条后 `pause()` 常驻 —— 无 UART 时的**唯一可视验收信号**。
- `init-debug.c`：额外配置 configfs USB gadget ACM（USB 串口日志）并支持
  `armada.reboot_after=<秒>` 自动重启，用于实机试错（避免长按电源）。

### 3.4 单元测试

| 文件 | 用例 | 断言要点 |
|---|---|---|
| `test_m2_boot_v3.py` | 5 例 | 默认仍 128 MiB；`--minimal-avb` 用真实内容长度；`--avb-partition-size` 精确填充；超限失败且不留输出；`--minimal-avb` 与 `--avb-partition-size` 互斥 |
| `test_m2_initramfs.py` | 2 例 | 条目/静态 aarch64 ELF/`/dev/console rdev 5:1`/canary 文本/`under_limit`/sha256；两次构建可复现 |

测试自带合成 v3 镜像（不依赖 stock boot 与 MBKP），可在 CI 跑。

## 4. 工作分解（WBS）

### W1 恢复源码与一致性校验（0.5 h）

```sh
T=/home/sel/.local/share/Trash/files
cp "$T/build_linux_boot_v3.py" "$T/build_min_initramfs.py" devices/xiaomi-elish/boot/
cp "$T/init.c" "$T/init-debug.c" devices/xiaomi-elish/boot/min_initramfs/
cp "$T/test_m2_boot_v3.py" "$T/test_m2_initramfs.py" devices/xiaomi-elish/tests/
```

- [ ] 与 `__pycache__/*.pyc` 再做一次 code object 对比（防止拷贝到错误版本）
- [ ] `python3 -m unittest -v devices/xiaomi-elish/tests/test_m2_initramfs.py` 通过
- [ ] `git status` 确认只新增预期文件（`min_initramfs/` 目前为空目录）

### W2 补齐 repack 选项（2–3 h）

- [ ] 按 §3.2 实现 3 个选项、校验、互斥与 report 字段
- [ ] `python3 -m unittest -v devices/xiaomi-elish/tests/test_m2_boot_v3.py`（5 例）
- [ ] 手工回归：用 v1 参数（无新选项）重打包 Android-only，输出长度必须仍为 128 MiB
- [ ] 手工回归：`--max-output-size 201326592` → 长度 < 上限；`--avb-partition-size 201326592`
      → 精确 192 MiB；超限内核 → 非零退出且不产生输出文件

### W3 重建依赖与产物（1–2 h，不含内核重编）

前提：本机已有 `vmlinuz` + 两个 DTB、stock boot、`/tmp/opencode` 已失效需重建 runtime/packer。

- [ ] 编译 packer/unpacker：
      `cmake -S packages/kshim-tools/vendor/packer -B <build> -DBUILD_SHIMS=OFF && cmake --build <build> --parallel`
- [ ] 重建 runtime（4 MiB，在共享源码树内执行）：
      `cd devices/lenovo-tb323fu/boot/kshim-success-source && \
        python3 tools/build_runtime.py --config configs/elish_defconfig --out <runtime> --jobs 8`
- [ ] 生成 initramfs（canary + debug 两版）并核对条目/limit/sha
- [ ] 生成两个 boot（CSOT 优先，BOE 备选）：

```sh
python3 devices/xiaomi-elish/boot/build_linux_boot_v3.py \
  --stock-boot /var/tmp/elish-fw-verify/boot.img \
  --linux-image <kernel>/vmlinuz \
  --dtb <kernel>/sm8250-xiaomi-elish-csot.dtb \
  --initrd <initrd>/initramfs.cpio.gz \
  --bootargs "console=ttyMSM0,115200n8 earlycon rdinit=/init loglevel=7" \
  --runtime <runtime>/kShimC.bin \
  --packer <build>/MultiBootKernelPatcher --unpacker <build>/UnpackTool \
  --avbtool packages/kshim-tools/vendor/avbtool.py \
  --panel csot --raw-profile xbl-evidence \
  --max-output-size 201326592 \
  --output-name boot-csot.img --out <out-csot> --source-label elish-m2-csot-v1
```

- [ ] 核对 `report.json`：`offline_verified: true`、checks 全 PASS、shim sha256 与新 runtime 一致、
      `loader_budget.profile == elish-xbl-256m-evidence`、`boot` 布局与 §2.4 同构
- [ ] 对 BOE 重复（换 `--dtb` / `--panel boe`）

> 注意：`--out` 必须为空目录且不与任何输入重叠；每次迭代用新目录（`-v2`…）。
> 内核若因 DTS/补丁变更需要重编，走 `packages/kernel/scripts/build-kernel.sh`（全量，勿 FAST）。

### W4 实机验证（每轮 0.5–1 h）

1. `adb reboot bootloader`（已解锁）；
2. `fastboot boot boot-csot.img`（157 MB，< 192 MiB 分区）；
3. 观察：
   - 屏上彩条 → kShimC→Linux→userspace 链路通过；
   - 黑屏但设备未重启 → 换 `boot-boe.img`；仍黑屏 → `probe-nodrm` / `probe-nolva` 变体
     （提供 `loglevel=8 drm.debug=0x1e`、`initcall_blacklist=msm_drm_init`、`arm64.nolva` 等）；
   - 需要日志 → debug initramfs，USB 端应出现 ACM 串口（`/dev/ttyACM*`），可见
     `ARMADA-ELISH-M2-CANARY` 日志；
4. 退出：`armada.reboot_after=30` 变体自动重启，或长按电源 10 s；
5. 记录：panel、是否出画、卡住阶段、串口最后一行、ABL 是否报错（fastboot 输出）。

> 关键未知：192 MiB 只是 XBL memory-map 证据；若 ABL 按 128 MiB 或 PCD
> `RamdiskEndAddress=0x05600000` 限制，会表现为“下载成功但重启回 Android / 挂住”。
> 这一轮实机就是把该 profile 变成事实。

### W5 文档与提交（0.5 h）

- [ ] README 更新：功能状态表 M2 行、M2 专节（构建/刷入/判据/caveat）、里程碑勾选
- [ ] 提交拆分建议：
  1. `feat(elish): add M2 minimal initramfs builder and canary init`
  2. `feat(elish): add v3 Linux maintenance boot packaging driver`
  3. `fix(elish): extend v3 repacker with output-size and minimal-AVB modes`
  4. `test(elish): cover M2 initramfs and repacker modes`
  5. `docs(elish): record M2 bring-up status and commands`
- [ ] 产物、日志、stock/kernel 输入不入库

## 5. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| 192 MiB loader budget 只是证据推断 | 镜像可能被 ABL 拒绝或加载后挂死 | 先用 `fastboot boot`（不刷分区）；失败则记录 ABL 输出，回到 128 MiB 方案（裁内核或分区内不放完整 Linux Image） |
| 打包内核 137,099,400 B > 128 MiB | conservative profile 直接失败 | 显式 `--raw-profile xbl-evidence`，并在 report/README 标注“未实测” |
| 157 MB 输出 > stock 128 MB | 不能按原镜像长度刷写 | 目标分区实际 192 MiB；只 `fastboot boot`；需要刷写时用 `--avb-partition-size 201326592` 或 `--pad-to-max-output` |
| 面板 CSOT/BOE 不确定 | 黑屏误判为内核问题 | 两个 DTB 各出一版；先 CSOT；黑屏先换 BOE 再调 bootargs |
| 无 UART，`ttyMSM0` 可能不可达 | 无法看内核日志 | framebuffer canary + debug 版 USB ACM + pstore/ramoops（DT 已有） |
| AVB 未签名（NONE） | 必须解锁 BL | 只 boot 不 flash；文档写明 |
| runtime/initrd 尺寸 | 4 MiB runtime、7.5 MiB initrd 上限 | builder 已断言 runtime == 4 MiB、initrd ≤ limit |
| 内核产物过期 | DTS/补丁变更后 dtb 不匹配 | 每次改 `packages/kernel/dts/**` 后按 AGENTS §8.3 判断是否重编并重出 dtb |

## 6. 待决问题

1. **默认 profile**：是否把 `--raw-profile xbl-evidence` 设为设备默认（当前 CLI 默认 conservative，
   会导致默认构建失败），还是在 README 明确必须显式传？
2. **默认面板**：`--panel auto` 从 DTB 识别即可，但交付默认给 CSOT 还是同时给两版？
3. **initramfs 路线**：M2 继续用最小 canary（当前方案），还是提前接 dracut/ostree？
   建议 M2 保持 canary，全量 initramfs 留给 M5 安装流程。
4. **验证产物位置**：固定 `/var/tmp/elish-m2/` 还是按日期分目录（建议后者，AGENTS §9）。
5. **是否把 `repack_boot_v3.py` 的新选项设计成通用 v3 能力**（未来其它 v3 设备可复用）
   —— 建议是，选项本身与设备无关。

## 7. 命令速查

```sh
# 单测
python3 -m unittest -v devices/xiaomi-elish/tests/test_m2_initramfs.py
python3 -m unittest -v devices/xiaomi-elish/tests/test_m2_boot_v3.py

# 最小 initramfs
python3 devices/xiaomi-elish/boot/build_min_initramfs.py \
  --out /var/tmp/elish-m2-<date>/initrd/initramfs.cpio.gz
python3 devices/xiaomi-elish/boot/build_min_initramfs.py \
  --source devices/xiaomi-elish/boot/min_initramfs/init-debug.c \
  --out /var/tmp/elish-m2-<date>/initrd/initramfs-debug.cpio.gz

# 取内核/DTB（已存在时）
K=/var/tmp/elish-fw-verify/kernel/lib/modules/7.2.6

# 实机
fastboot boot /var/tmp/elish-m2-<date>/boot-csot/boot-csot.img
fastboot reboot
```
