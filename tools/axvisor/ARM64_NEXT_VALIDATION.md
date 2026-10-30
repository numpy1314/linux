# ARM 功能验收与 MiBench/lmbench 对照入口

以 `drivers/virt/axvisor/ARM64_KVM_COMPARISON.md` 为实现依据。Core 仍锁定
`4152c3e780feb5e44241fb3fa143fe04bdca0181`。本文记录下一步执行条件和范围，
不包含性能数据或功能通过声明。

## 当前可复现的快速功能检查

使用 `tests/arm64-linux-guest/build-assets.py --rounds 1` 分别构建：

- `--vcpus 1 --guest-shutdown poweroff --fc-cpus 2-3`。
- `--vcpus 2 --guest-shutdown poweroff --fc-cpus 2-3`。
- `--vcpus 2 --guest-shutdown poweroff --fc-cpus 2`，检查同核并存的 KVM_RUN。
- `--vcpus 1 --guest-shutdown restart --fc-cpus 2-3`，独立报告重启结果。

用 `tests/arm64-linux-guest/run-qemu.py --cpus 4 --guest-cpus 2-3` 执行，
每项使用新的输出目录。guest 会校验 CPU 数量，在每个 CPU 上运行并等待
定时器，再检查整数计算、4 MiB 文件写入、fsync、重新打开和逐字节读取。
主机必须观察到 Firecracker 正常退出和 VM 回收才算该生命周期通过。

`tests/arm64-liveness/` 用一轮不含 WFI、SMC 或 MMIO 的忙循环 guest，
检查同核发信号的另一个进程确实能被调度，并使 KVM_RUN 返回 EINTR。
这是功能检查，不以 QEMU 时间作为性能数据。

## 需要批准的共享 Core 改动范围

当前适配接口无法修复以下路径。后续应在备份 Core 中形成一个共同提交，
让 x86 与 ARM 均使用同一依赖版本，避免私有 ARM Core 分叉。

| 位置 | 应完成的行为 | 依据与边界 |
| --- | --- | --- |
| `virtualization/arm_vcpu/src/exception.rs` | 对 HVC/SMC 的 PSCI 系统关机、重启做明确的虚拟机内处理；未实现的调用返回不支持 | 不将 guest SYSTEM_RESET 或其他未审查的调用直接转给物理固件；需要核对现有受支持的固件调用，不能一律替换后声称兼容 |
| `virtualization/axvcpu/src/exit.rs` 与 ARM 后端 | 保留关机/重启原因，传给共享 Core；先核对能否通过现有 Hypercall 表达，确需扩展再加系统事件类型 | x86 triple fault 的 SHUTDOWN 行为保持原语义；不将所有架构退出统一改成 RESET |
| `virtualization/axvisor_core/src/kvm/run/arm64.rs`、`run/exit.rs`、运行状态管理 | 输出 ARM `KVM_EXIT_SYSTEM_EVENT` 与 SHUTDOWN/RESET 类型，停止同 VM 的所有 vCPU，避免再次进入 guest | 参考 Linux 6.12 `arch/arm64/kvm/psci.c::kvm_prepare_system_event`；审查正在其他 CPU 运行的 vCPU 如何接收停止请求。周期 watchdog 与原生定向 kick 的差异仍需保留 |

验收至少包括 HVC 与 SMC 两个 conduit 的 OFF/RESET、未知调用返回值、双 vCPU
系统事件后的停止与重新进入行为、VM 清理，以及 x86 构建/相关回归。先在隔离
QEMU 中验证宿主仍存活，再允许候选内核进入实板验证。Core 批准前不应用这些改动。

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
