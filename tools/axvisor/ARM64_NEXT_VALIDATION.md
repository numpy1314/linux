# ARM 功能验收与 MiBench/lmbench 对照入口

以 `drivers/virt/axvisor/ARM64_KVM_COMPARISON.md` 为实现依据。x86 与 ARM 使用
`numpy1314/tgoskits` 的同一 Core pin：
`ce800d4d28fd552441f166c2679f63f8b14d121c`，在原始 4152 上增加六个提交。
本文只记录执行条件、用例和剩余边界，不包含性能数据或功能通过声明。

## 本轮优先范围：Firecracker 退出、join 与回收

本轮 Firecracker 生命周期需要准确的 guest OFF/RESET 事件、guest SMC 不触及
物理固件，以及 VMM 发起停止后各 vCPU 的 KVM_RUN 能退出、线程能 join、VM
能释放。Firecracker 不依赖 Core 先给出“所有 vCPU 已同步停止”的通知才开始
自己的退出处理；不能把这两个协议混为一谈。当前 Core 的同步全 VM 停止保留
为独立扩展，其定向用例另列。CPU_SUSPEND 的定时器 deadline、重复 INIT 和
完整原地重建不属于本轮 Firecracker 退出路径的前提。

以下 `tests/` 命令从 `tools/axvisor/` 目录执行。分别构建实际 Linux guest：

使用 `tests/arm64-linux-guest/build-assets.py --rounds 1`：

- `--vcpus 1 --guest-shutdown poweroff --fc-cpus 2-3`。
- `--vcpus 2 --guest-shutdown poweroff --fc-cpus 2-3`。
- `--vcpus 2 --guest-shutdown poweroff --fc-cpus 2`，检查同核并存的 KVM_RUN。
- `--vcpus 1 --guest-shutdown restart --fc-cpus 2-3`，独立报告重启结果。

用 `tests/arm64-linux-guest/run-qemu.py --cpus 4 --guest-cpus 2-3` 执行，
每项使用新的输出目录。guest 会校验 CPU 数量，在每个 CPU 上运行并等待
定时器，再检查整数计算、4 MiB 文件写入、fsync、重新打开和逐字节读取。
主机必须观察到 Firecracker 正常退出和 VM 回收才算该生命周期通过。
重启用例核对 RESET 事件驱动的 VMM 退出和清理，不把它称作同一 VM 内重新
引导 Linux。还要覆盖一个 vCPU 发起结束、其他 vCPU 持续忙循环的退出路径，
观察 VMM stop 请求、各 KVM_RUN 返回、join 完成和资源回收；仅看到 guest
控制台的结束行不足以完成验收。

`tests/arm64-liveness/` 用一轮不含 WFI、SMC 或 MMIO 的忙循环 guest，
检查同核发信号的另一个进程确实能被调度，并使 KVM_RUN 返回 EINTR。
这是功能检查，不以 QEMU 时间作为性能数据。

`tests/arm64-firecracker-stop/` 使用真实 Firecracker 和双 vCPU 微客户机：
一个 vCPU 保持忙循环，另一个有意触发非 PSCI UNKNOWN 退出，使 VMM 自己走
Finish→宿主信号→join 路径。分别设置整个 VMM 的宿主亲和性为 2-3 和 2。
runner 默认关闭 watchdog，并要求自然回收子进程、正确的退出原因、VM 清理、
宿主继续运行与 fd 平衡；不能用系统事件预先停止全体 vCPU来替代本项。
该错误注入用例独立于正常 poweroff/restart 的成功退出验收。

## 已实施契约与独立的同步停止扩展

以下是当前共享 Core 的实现契约，不是运行结果。原始 4152 的 HVC RESET
缺失和物理 SMC fallback 已在架构后端与 Core frontend 修正，不能再写成
“adapter 等待 Core 批准”的现行阻塞。

| 位置 | 实施契约 | 依据与边界 |
| --- | --- | --- |
| `virtualization/arm_vcpu/src/exception.rs` 与 `smc.rs` | HVC/SMC PSCI 经现有 Hypercall 上报；未知 SMC、不支持的 immediate 返回 -1，不调用物理固件 | 未增加公共 exit 枚举；非 PSCI 私有 HVC 保留原分派 |
| `virtualization/axvisor_core/src/kvm/run/arm64.rs` | OFF/RESET 输出 ARM `KVM_EXIT_SYSTEM_EVENT` 与相应类型，`ndata=1`、`data[0]=0`，源 x0 预置 -6 | 区分 x86 triple fault 的 SHUTDOWN；事件通知不等于完成 guest 全系统复位 |
| `virtualization/axvisor_core/src/kvm/{state,mod}.rs` 与 `run/mod.rs` | 同步停止扩展：发布全 VM STOPPED、撤销排队启动、每次 entry 检查 gate、对活跃 CPU 同步 kick；STOPPED 等待可被信号打断 | 参考 native ARM 停止请求机制；该更强完成条件独立验收，不作为 Firecracker stop/join 的必要前提 |
| `virtualization/axvisor_core/src/kvm/vcpu.rs` 与 ARM reset helper | CPU_ON 由目标 runner 应用复位；GET/SET_ONE_REG 先消费 pending reset；初次 INIT 使用 PSCI_0_2 | 寄存器访问不能读到旧状态或被下一次 RUN 覆盖；重复 INIT 本批明确拒绝 |

定向微型 guest 入口从仓库根目录执行：

```sh
python3 tools/axvisor/tests/arm64-psci/build-initramfs.py --output /tmp/psci-assets
python3 tools/axvisor/tests/arm64-psci/run-qemu.py \
  --kernel /absolute/path/to/Image --config /absolute/path/to/.config \
  --assets /tmp/psci-assets --output /tmp/psci-results --watchdog off
```

输出目录必须新建。runner 使用隔离四核 Cortex-A76 QEMU，VMM 工作者在 CPU2/3；
`--watchdog off` 设置 `axvisor_linux.watchdog=0`，关闭周期 5 ms hrtimer，保留
guest virtual-timer PPI。定向检查以下契约：

- HVC 与合法 SMC 的 OFF/RESET，核对完整 system-event 字段；未知 PSCI、
  未知 SMC、非零 SMC immediate 返回 -1，私有 HVC 保留原路径。
- CPU_ON 已运行目标返回 ALREADY_ON；成功排队后，在目标首次 RUN 前做
  GET_ONE_REG，再做 SET_ONE_REG，核对 pending reset 的顺序。
- 另一 vCPU 关 guest IRQ 并持续更新共享计数器时，系统事件返回后两者均为
  STOPPED，计数器不再变化；关闭 watchdog 时仍须满足同步停止扩展。
- STOPPED 的 RUN 被信号打断；用户态显式设 RUNNABLE 后，从 PSCI 后的
  上下文继续并观察 x0=-6。此恢复用例不代表全 VM 重新初始化。
- 结束工作线程和 VM 后检查 fd 平衡，并保留 x86 构建/相关回归入口。

用 `--watchdog on` 另做默认策略检查，不能拿周期退出掩盖定向 kick 缺失。
`--backend kvm` 可使用同 Linux 6.12 的原生 KVM 内核做行为对照；私有 HVC
的 unknown-exit 是 Tessera 专有契约，应单独解释，不能要求原生执行相同分派。
先在隔离 QEMU 验证固件隔离与生命周期，再进入实板功能验证；本页不记录
这两阶段已经执行或通过。

## 本批未实现与保留差异

- 声明 PSCI 0.2，仅实现 VERSION、CPU_OFF、CPU_ON、AFFINITY_INFO、
  MIGRATE_INFO_TYPE、SYSTEM_OFF、SYSTEM_RESET；CPU_SUSPEND、FEATURES、
  RESET2 返回 NOT_SUPPORTED。timer-aware suspend 和 WFI/idle 优化另行处理。
- 重复 INIT、完整寄存器/缓存重新初始化、RAM/设备/VGIC 的全 VM 原地重建
  均未纳入本批；Firecracker RESET 后退出不要求这些能力。
- CPU_ON 入口必须四字节对齐，错误在排队前返回 INVALID_PARAMS；native
  Linux 6.12 不做这项预检查。非法或未初始化目标返回 INVALID_PARAMS，
  已运行或已排队目标返回 ALREADY_ON。
- `SET_MP_STATE` 可在另一线程的 KVM_RUN 活跃时异步 stop/wake，比 native
  6.12 的 vCPU mutex 串行语义更宽。不要把该用法称作所有 KVM 实现共同保证。
- 整次 ioctl 禁止迁移、irqfd 线程协议和 non-x86 整槽 pin 仍保持既有边界；
  本批不将完整原生 KVM 能力作为 Firecracker 退出验收门槛。

## 已恢复的实板性能输入

健康系统上的评测套件：`/home/orangepi/fh-recovery-20261030/native-smoke`。
`work/bench/` 与 `work/guests/bench.ext4`、`bench.json` 已从只读旧卡恢复，
独立于 `historical-build/` 中的旧 ARM 内核。

bench 镜像的预期 SHA-256（来自其原始 receipt）：
`5d5a683561b112416dc25ed4b252025492dcc7b3ea36e9b0bb57527eca344f19`。
使用前仍需核对二进制、guest kernel、Firecracker 和 suite.py 的输入哈希。
这些资产的恢复不赋予旧 Core 或旧性能结果新的验收状态。

旧 guest `/fh-init.sh` 在 suite 结束后调用 `reboot -f`，因此不能靠忽略
Firecracker 未退出就将一次性能运行判为成功，也不能把 timeout 当作正常回收。

## 快速性能对照配置

| 项目 | 配置 |
| --- | --- |
| 两个后端 | 同 Linux 6.12 基础版本的原生 ARM KVM / Tessera；分别启动，明确记录 Image、config、Core 哈希与 boot id |
| 恢复系统 | 当前 6.1.99 系统仅作恢复与资产准备，不直接与 6.12 候选组成公平性能对照 |
| guest | 相同 Firecracker、guest kernel、bench rootfs、1 vCPU、1024 MiB |
| CPU | 整个 VMM 及其线程均限制在 CPU4，两个后端一致；另留宿主 CPU 处理遥测 |
| 频率 | CPU4-7 公共频率 2.208 GHz，记录实际频率与温度，保留自动风扇 |
| 退出策略 | 记录 Tessera watchdog 配置及两边 boot 参数；`watchdog=0` 的定向同步停止检查不自动成为性能配置 |
| MiBench | 3 个外部样本、1 次预热、1 个 VM 生命周期 |
| lmbench | 3 个外部样本，保留已有各子项参数和内部校准；1 个 VM 生命周期 |
| 输出 | 保留每项原始输出、错误与环境数据，比较中位数；先用于定位优化方向 |
| 比值 | 延迟/耗时使用 Tessera÷KVM，越低越好；吞吐使用 Tessera÷KVM，越高越好，分别标注 |

两套实板内核功能验收完成、输入一致性和锁频检查通过后，在评测套件目录分别执行：

```sh
python3 eval.py run --stack kvm --suite mibench --vcpus 1 --memory-mib 1024 --cpus 4 --samples 3 --warmups 1 --repeat 1 --timeout 1200
python3 eval.py run --stack kvm --suite lmbench --vcpus 1 --memory-mib 1024 --cpus 4 --samples 3 --warmups 1 --repeat 1 --timeout 3600
```

在 Tessera 内核下将 `--stack kvm` 换成 `--stack fh`，其余参数一致。
套件本身不负责更换启动内核；在恢复系统上只改 stack 名称不构成后端切换。
不得使用 QEMU TCG 结果作为 RK3588 性能比值，不复用旧轮次的数字填充新对照。
本次不用包含大量重复生命周期或 fio/STREAM 的 `run-rk3588-one-pass.sh` 全套入口。
