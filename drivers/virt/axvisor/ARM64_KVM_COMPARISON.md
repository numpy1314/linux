# ARM Linux 适配：从 x86 实现解释到 ARM 行为对照

原始审计日期：2026-10-30。下面的固定对象、四方表和第 1–7 节保留为当时的
源码审计快照，使用原样 Core 4152；其中“当前”“候选”和行号均指该固定版本。
该次审计没有新增运行结果，也没有修改 Core。后续共享 Core 的实施范围另列于
第 8 节；旧快照中的缺失能力和阻塞项不能直接当成新版本的现状。

## 固定比较对象

| 对象 | 原始审计固定版本 |
| --- | --- |
| 产品 x86 Linux | `TesseraHypervisor/linux`，`e725a24a0ec62b97a5a7476f102aba268a7682aa`，实际读取该提交的代码 |
| 共享 Core | `Ivans-11/tgoskits`，`4152c3e780feb5e44241fb3fa143fe04bdca0181` |
| ARM Linux 候选 | 本分支 `11e22a67532a732d6e94d73aff7f8dddca0b276a` |
| 原生 x86/ARM KVM | 本 Linux 仓库内的 6.12.0 KVM 源码；不是对所有内核版本的概括 |

下文 `X:` 指上述产品 x86 Linux，`L:` 指本 Linux 仓库，`C:` 指上述 Core 仓库。行号固定在这些版本。
产品 x86 参考入口：[runtime](https://github.com/TesseraHypervisor/linux/blob/e725a24a0ec62b97a5a7476f102aba268a7682aa/drivers/virt/axvisor/axvisor_runtime.c)、[host](https://github.com/TesseraHypervisor/linux/blob/e725a24a0ec62b97a5a7476f102aba268a7682aa/drivers/virt/axvisor/rust/axvisor-linux-host/src/lib.rs)、[最近一次等待和内存生命周期修正](https://github.com/TesseraHypervisor/linux/commit/e725a24a0ec62b97a5a7476f102aba268a7682aa)。

## 后续修改规则

每条路径先完成以下顺序，再决定代码改动：

1. 原生 x86 KVM 如何完成这项职责。
2. 产品 x86 的 adapter、Core 和架构后端分别做什么；明确与 KVM 的差异。
3. 根据源码、注释和提交记录解释差异。只有推断时明确写“推断”，不替作者编造设计动机。
4. 原生 ARM KVM 如何完成同一职责，再检查共享 Core 的 ARM 分支已提供什么。
5. 决定 ARM 直接复用、采用架构对应机制，还是被现有 Core/接口阻塞；记录测试如何验证。

不能把“x86 已能运行”当作它与 KVM 完全一致的证明；不能把 VMX 结论自动推广到 SVM；不能把 static VM 路径的能力算到 KVM control 路径；不能把相同 Core 提交解释为所有架构执行相同代码。

## 四方对照

| 路径 | 原生 x86 KVM | 产品 x86 | 原生 ARM KVM | 原始 ARM 候选及结论 |
| --- | --- | --- | --- | --- |
| 运行边界 | 在进入/退出 guest 的局部窗口保护抢占与 IRQ，外层可调度 | ioctl 直接进入 Core；VMX 在 Core 的 IRQ 保护批次内 bind/run/unbind | 局部保护 VMID、VGIC 和 guest 切换，退出后恢复宿主 | 继续复用 Core 的切换边界；整个 KVM_RUN 关中断不可接受 |
| CPU 迁移 | load/put 加调度 notifier 维护 CPU 状态 | VMX 每批次重新绑定当前 CPU；adapter 未禁止整次 ioctl 迁移 | 使用同一 notifier 框架，并处理 VGIC、timer、FP 等状态 | 候选整次 control ioctl 禁止迁移是新增限制，不能称 VHE 必然要求或 KVM 等价 |
| FP/SIMD | 接入 Linux FPU 所有权与 guest fpstate | VMX Core 自行保存/恢复 FP；adapter 无相同范围的 FPU 包装 | 接入 Linux FPSIMD 元数据和 guest FP 所有权 | Linux kernel SIMD API 有依据；覆盖整个 KVM_RUN 并在回调处交接，是仍需审查的候选接法 |
| 物理 IRQ | VMX 派发已确认的退出向量；SVM 在退出后让 Linux 接 pending IRQ | VMX adapter 将向量交 Linux；SVM 为不同路径 | 恢复宿主状态后开 IRQ，由 Linux 接 pending IRQ | ARM 只窥探 HPPIR、不 ACK，再交 Linux 正常处理；不能照搬 VMX 向量重放 |
| 长跑 guest 交回控制 | 请求、调度和 IRQ 驱动；硬件定时器也有具体用途 | VMX Core 有固定预算抢占定时器；SVM 靠 host IRQ/HLT/PAUSE 等 | IRQ、请求/kick、调度框架 | 5 ms watchdog 与 VMX 定时器有目的对应，但周期、作用 CPU 范围和开销是新增策略 |
| irqfd 等待/取消 | eventfd 回调，原子注入或工作队列；取消时撤销回调并排空 work | 每 irqfd 一个 Core 线程；adapter 阻塞读；取消为标志、写唤醒、join、释放 | 共用 eventfd 框架，架构注入不同 | 直接复用 x86 阻塞读和共享取消协议；与原生 KVM 回调注入仍有差异 |
| 空闲和事件唤醒 | halt polling 后可阻塞，wake 或 IPI kick | control HLT 仍检查加 yield；static 路径另有 WaitQueue | WFI 可陷入、阻塞，并按 guest 定时器期限唤醒 | Core ARM 缺少相同 WFI trap 路径，control 排队也无定向 kick；watchdog 不替代这些语义 |
| 内存槽 | 通常按 fault 建立映射，结合 MMU notifier | x86 Core 按需 pin，支持稀疏槽 | stage-2 fault 建映射，结合 notifier | non-x86 Core 整槽预 pin，是现有策略而非 ARM 硬件要求；销毁已有整段 unmap |
| 关机重启 | triple fault/shutdown 返回 SHUTDOWN；平台 PIO 是另一条路径 | Core 归一为 SystemDown；还特判部分平台关机端口 | PSCI 返回 SYSTEM_EVENT，区分关机/重启并停止所有 vCPU | SYSTEM_OFF 沿用 SHUTDOWN；HVC RESET 不支持；SMC RESET 可到物理固件。adapter 无完整拦截点 |

## 1. 运行、迁移、SIMD 与中断

`X:drivers/virt/axvisor/axvisor_runtime.c:235` 的 ioctl 包装直接进入 Core。VMX/ARM 的 `C:virtualization/axvm/src/vm.rs:638` 使用 `IrqSave`、bind、闭包、unbind；`:685` 的批次最多调用八次 `vcpu.run()`，遇到非 `Nothing` 提前结束。这是调用次数限制，不是运行时间上限。这解释了为什么不应在 adapter 外层另造一个覆盖等待、睡眠和全部重入的关 IRQ 区域。

原生 KVM 的 `L:virt/kvm/kvm_main.c:205`、`:6358` 用 load/put 和 preempt notifier 处理调度进出。ARM 局部切换见 `L:arch/arm64/kvm/arm.c:1168`。产品 VMX 绑定时刷新当前 CPU 的 host state，见 `C:virtualization/x86_vcpu/src/vmx/vcpu.rs:345`、`:2448`。候选 `L:drivers/virt/axvisor/axvisor_arm64.c:324` 则在整次 control ioctl 上 `migrate_disable()`；它允许调度和睡眠，但不是完整迁移支持。CPU 池成员检查与实际 world switch 状态保护必须分别论证。

VMX Core 的 FP 保存/恢复见 `C:virtualization/x86_vcpu/src/vmx/vcpu.rs:2579`；ARM 的寄存器保存/恢复见 `C:virtualization/arm_vcpu/src/vcpu.rs:124`。保存硬件寄存器还不等于同步 Linux 的 FP 所有权元数据。原生 KVM 分别见 `L:arch/x86/kvm/x86.c:11459` 与 `L:arch/arm64/kvm/fpsimd.c:44`、`:103`。Linux 6.12 可调度 kernel FPSIMD 的依据在 `L:arch/arm64/kernel/fpsimd.c:1560`、`:1903`。候选的整次 KVM_RUN 包装及 host callback handoff 见 `L:drivers/virt/axvisor/axvisor_arm64.c:341` 和 `rust/host-call.rs`；当前接口粒度能解释这个接法，但不能证明它已经等价于 KVM。

物理 IRQ 要按硬件是否已经确认来比较。原生 VMX 的 IDT/FRED 派发见 `L:arch/x86/kvm/vmx/vmx.c:7007`；SVM 见 `L:arch/x86/kvm/svm/svm.c:4629` 及公共 `x86.c:11046`；ARM 见 `L:arch/arm64/kvm/arm.c:1219`。候选采用 HPPIR 窥探、保持 pending，待恢复 Linux IRQ 入口后处理，不能追加一次 VMX 式软件派发造成重复处理或错误 ACK。

SVM 在 `C:virtualization/axvm/src/vm.rs:718` 走独立的 `run_vcpu_raw_svm`。本轮未完成 SVM FP 所有权全路径验证，不能引用 VMX 的 FP 保存结论作为 SVM 验收证据。

## 2. watchdog、条件调度与定向 kick 是三件事

产品 VMX Core 的 `VMX_PREEMPTION_TIMER_SET_VALUE = 50_000` 位于 `C:virtualization/x86_vcpu/src/vmx/vcpu.rs:65`；配置和补充预算见 `:934`、`:2085`，退出分类见 `:2863`。该值是硬件计数预算，不能直接解释为 5 ms。

SVM 没有同样的 VMX 定时器。`C:virtualization/x86_vcpu/src/svm/vcpu.rs:363` 配置 `V_INTR_MASKING`，`:2055` 把 host INTR 转为共享退出类型，并说明依靠 host IRQ、HLT、PAUSE 等提供处理机会。因此“x86 adapter 没有 watchdog”不表示 x86 没有相应机制；也不表示 ARM 应无条件复制一个固定周期。

候选 `L:drivers/virt/axvisor/axvisor_arm64.c:58`、`:181`、`:366` 在任何 KVM_RUN 活跃时给所有在线 CPU 启动 5 ms hrtimer。待审查的是必要性、是否只需覆盖运行 CPU/guest CPU 池、事件响应延迟及额外唤醒成本。不能把周期退出机会称作“5 ms 内一定返回用户态”。

调度还存在另一层差异：`C:virtualization/axvisor_core/src/kvm/run/mod.rs:198` 对 ExternalInterrupt/PreemptionTimer 等直接 `yield_now()`，`X:drivers/virt/axvisor/axvisor_runtime.c:1118` 实现为 `yield()`；原生 `L:kernel/entry/kvm.c:6` 根据 need-resched 等工作条件处理。ARM 继承了产品语义，不能由此推导性能与 KVM 相同。

定向唤醒见原生 `L:virt/kvm/kvm_main.c:3883`：唤醒睡眠 vCPU，或使运行 CPU 退出 guest。当前 `C:virtualization/axvisor_core/src/kvm/mod.rs:237` 的 control 队列仅清 halted 并入队。周期 watchdog、条件调度与收到事件后的定向 kick 必须分别测量和验收。

## 3. irqfd：复用有据，但仍不同于原生 KVM

产品 x86 的 `read_fd_ref`（`X:drivers/virt/axvisor/axvisor_runtime.c:450`）与候选 `L:drivers/virt/axvisor/axvisor_runtime.c:457` 均为：读取、EAGAIN 后注册 poll、重新读取、可中断睡眠、清理等待。它避免了无事件时返回 Core 不断 yield 的空转。

共享契约 `C:virtualization/axvisor_api/src/control.rs:107` 明确允许阻塞读；`C:virtualization/axvisor_core/src/kvm/eventfd.rs:364` 建立监听线程，`:382` 以 cancel、写唤醒、join、释放引用取消。线程读取是当前可移植接口已经选择的协议；是否是最优设计不能由接口存在本身证明。

原生 KVM 在 `L:virt/kvm/eventfd.c:195`、`:393` 注册 eventfd 回调并注入，在 `:122`、`:564` 摘除回调与排空工作。x86/ARM 架构注入分别见 `arch/x86/kvm/irq_comm.c:159` 与 `arch/arm64/kvm/vgic/vgic-irqfd.c:105`。本轮可以确认 ARM 阻塞读有 x86 复用依据，但不能把这说成已经获得 KVM 的直接回调注入路径。

## 4. WFI/HLT：不要把 static 能力算给 control

产品 control 的 `C:virtualization/axvisor_core/src/kvm/run/mod.rs:385` 在 HLT 后反复检查事件并 yield，非 runnable 等待在 `:339` 也如此。带自适应 halt polling 和 WaitQueue 的实现位于 static 路径 `C:virtualization/axvisor_core/src/vmm/vcpus.rs:140`，不能混用证据。

原生 ARM `L:arch/arm64/kvm/handle_exit.c:129` 处理 WFx，进入 `L:arch/arm64/kvm/arm.c:929` 的 `kvm_vcpu_wfi()`；公共阻塞见 `L:virt/kvm/kvm_main.c:3716`，定时器后台唤醒及 load/put 见 `L:arch/arm64/kvm/arch_timer.c:571`、`:826`。这条陷入处理路径不是“每次 WFI 必然陷入”的无条件陈述，实际还取决于配置。

当前 Core ARM 的 `C:virtualization/arm_vcpu/src/vcpu.rs:233` 没有设置 TWI/TWE，`exception.rs:89` 没有相应 WFx 分支。adapter 不能单凭一个外层 hrtimer 补出 Core 尚未暴露的 WFI/Halt 协议。空闲 CPU 占用、事件到达延迟、guest 定时器唤醒应分开验证。

## 5. 内存：共享提交内已有架构策略分流

`C:virtualization/axvisor_core/src/kvm/memory.rs:114` 为 x86 登记槽并保留地址空间，`:139` 的注释明确解释：gVisor 会注册数 GiB 的稀疏区域，因此按 nested page fault 取得并 pin 实际访问页。这是源码说明的具体原因，不应一律归因于“接口不够”。

同文件 `:62` 的 non-x86 分支整槽 pin，`:403` 预建映射；`:435` 已对完整 GPA 区间一次 unmap，然后释放 pinned pages，注释明确为了避免逐页 MemorySet 扫描的 O(n²) 行为。x86 稀疏已映射页的逐页释放在 `:451`。这与原生 ARM 的按需 stage-2 映射（`L:arch/arm64/kvm/mmu.c:1422`）不同；批量撤销不是 ARM 硬件专属能力。

产品 x86 `X:drivers/virt/axvisor/axvisor_runtime.c:511` 与候选 `L:drivers/virt/axvisor/axvisor_runtime.c:518` 均使用 mmgrab/mmdrop，只保留 mm 描述符；pin 时暂时取得活地址空间引用。源码说明其目的是避免 VMA 持有 vCPU file、file 释放再销毁槽形成生命周期环。应保留这项已存在的 x86 修正。

## 6. 关机重启：复用职责，不照搬退出编码

原生 VMX triple fault（`L:arch/x86/kvm/vmx/vmx.c:5389`）与 SVM shutdown（`L:arch/x86/kvm/svm/svm.c:2206`）使用 KVM_EXIT_SHUTDOWN。产品分别在 `C:virtualization/x86_vcpu/src/vmx/vcpu.rs:2634` 与 `svm/vcpu.rs:2074` 转为 SystemDown，再由 `C:virtualization/axvisor_core/src/kvm/run/exit.rs:43` 输出该编码；adapter 仅转发 ioctl。

即使 x86 也不能宣称完整等价：产品还特判 `0x604` 上的 `0x2000` 关机写入（VMX `:2842`、SVM `:2019`）；原生通常通过 PIO 设备总线或用户态设备处理。原生 SVM 非 SEV-ES 的 shutdown 分支还重置 vCPU/VMCB，产品该分支仅返回 SystemDown。相同 exit reason 不证明所有状态转换相同。

原生 ARM 的 HVC/合法 SMC 进入受控的 SMCCC/PSCI 分派（`L:arch/arm64/kvm/handle_exit.c:37`、`:56`），`L:arch/arm64/kvm/psci.c:164` 停止所有 vCPU 并输出 SYSTEM_EVENT，`:192` 区分 OFF、RESET、RESET2。产品 ARM SYSTEM_OFF 也转 SystemDown，因此沿用 SHUTDOWN；HVC RESET 在 `C:virtualization/axvisor_core/src/kvm/run/arm64.rs:37` 返回 NOT_SUPPORTED。

SMC 是更早的边界：`C:virtualization/arm_vcpu/src/exception.rs:241` 未实现 RESET，`:281` 将未处理 SMC 直接交 `smc_call`，`smc.rs:24` 执行真实 `smc #0`。源码确认它可以把 guest RESET 送到物理固件；是否导致整板复位取决于固件，未做实板触发实验。即使 guest DT 选择 HVC，guest 仍可执行 SMC。

adapter 看不到被 Core 消化的 HVC RESET，也无法撤回 Core 已执行的物理 SMC。只修改 adapter 的关机 exit 编号，补不齐 RESET、全 VM 停止和固件隔离。保持 Core 不变的前提下，这仍是实板准入阻塞。

另一个容易误读的点：`X:drivers/virt/axvisor/axvisor_runtime.c:833` 的 HostIf::exit 调用 kernel_power_off，但 KVM_RUN 的 SystemDown 并不走它；已检索到的 host::exit 调用来自 Core shell。不能把这个函数当成 ARM guest PSCI 应复用的关机入口。

## 7. CREATE_DEVICE 失败回滚：明确新增补偿的理由

产品 x86 `X:drivers/virt/axvisor/axvisor_runtime.c:665` 直接 anon_inode_getfd；本 Linux 6.12 KVM 的 `L:virt/kvm/kvm_main.c:4804` 也在 copyout 前安装 FD（copyout 在 `:5336`）。因此候选的延后发布不是原样移植这两段代码。

Core ARM `C:virtualization/axvisor_core/src/kvm/device.rs:67` 先 create_user_fd，copyout 失败时 `:74` 仅关闭 Core device id，没有回收已安装 Linux FD 的对应接口。候选 `L:drivers/virt/axvisor/axvisor_runtime.c:146`、`:673` 通过任务局部事务先保留 FD、成功后发布、失败则释放未发布 file/FD，补偿两层对象生命周期不一致。应把它作为有明确失败路径依据的 ARM 增量单独维护，而不是称为“完全照搬 x86/KVM”。

## 原始审计的下一步与验收依据

| 类别 | 处理决定 | 要观察的证据 |
| --- | --- | --- |
| 已有 x86 方案 | 保留阻塞 fd 读取、共享取消顺序、mm 描述符引用修正、Core 现有整段撤销 | 无事件睡眠、取消竞态、失败回滚和资源释放；不能仅看 guest 能启动 |
| ARM 架构交接 | 围绕 Core world switch 复核 SIMD、VGIC/timer、物理 IRQ 所有权 | 用户/guest SIMD 内容、调度后恢复、IRQ 不丢失且不重复处理 |
| 新增限制/策略 | 先论证再决定缩小 ioctl 迁移保护范围、watchdog 覆盖范围与周期 | 同一次 KVM_RUN 中迁移、信号打断、长跑 guest、事件到达延迟、额外唤醒成本 |
| 共享 Core 边界 | 单列 control kick/WFI、PSCI 事件和 SMC 策略；不在 adapter 伪装已完成 | 原始请求如何传递、目标 vCPU 如何退出、全 VM 如何停止；没有接口则明确阻塞 |

当时以 Core `4152c3e…` 原样版本为约束。既有 QEMU 结果仅适用于其原版本；该次源码审计不增加通过项，也未启动实板候选。后续每次逻辑提交应同时记录四方依据、必要性、差异和针对性验证结果。

## 8. 后续实施范围：共享 Core 的 ARM PSCI 生命周期

Linux 的 x86/ARM 依赖统一锁定 `numpy1314/tgoskits` 的
`ce800d4d28fd552441f166c2679f63f8b14d121c`。它在原始
`4152c3e780feb5e44241fb3fa143fe04bdca0181` 上增加六个提交，依次处理 ARM
固件调用隔离与离线 CPU 复位、共享控制入口/停止协议、ARM PSCI 生命周期、
可接受的 preferred-target 配置、仅服务停止/信号的等待接口，以及
SYSTEM_EVENT_DATA 能力查询。没有单独的 ARM Core pin 或本地路径覆盖。
本节记录源码契约和剩余边界，不添加运行结果、功能通过或性能结论。

Firecracker v1.10.1 的收尾同步应按真实顺序理解：先给各 vCPU 投递 Finish，
再发送宿主信号；信号使 Core 的运行循环或可中断等待返回 EINTR，回到用户态
后 handler 才能设置 immediate_exit，vCPU 状态机随后消费 Finish。
VcpuHandle::Drop 的 join 完成后 VMM 才继续销毁。它不等待单独的 all-stop 通知。
Linux 的 ioctl/VMA file 引用保证活动 RUN 的 vCPU file 不会先被 release；
Core 又在最后 vCPU file 释放前保留 VM slots，最终先取消并 join irqfd，再
撤销映射、释放 pinned pages 和页表。仅凭一行退出或清理日志不能代替这些条件。

`tools/axvisor/tests/arm64-firecracker-stop/` 单独覆盖 Finish 对忙循环的打断：
通过有意的非 PSCI UNKNOWN 退出触发 VMM stop，避免 Core 的同步系统事件
停止扩展提前替 VMM 停住另一个 vCPU；同时覆盖分核和共享宿主 CPU。
该用例不是 PSCI 正常退出或 guest 错误退出码的语义验收。

本轮优先完成 Firecracker 的退出链路：准确上报 OFF/RESET、隔离物理固件，
由 VMM 发起 stop，使 KVM_RUN 退出、线程 join 和 VM 回收完成。Firecracker
不以 Core 的全 vCPU 同步停止通知作为开始退出的前提。已经实现的同步停止
保留为独立扩展并单独验收；完整 native PSCI、定时器 deadline 和全 VM
原地重建不作为该 Firecracker 生命周期的当前门槛。

| 路径 | 对照依据与职责选择 | 本次实现 |
| --- | --- | --- |
| HVC/SMC 分派 | 原生 ARM 在架构后端识别调用，再由 PSCI/frontend 决定 VM 行为；4152 的物理 SMC fallback 早于 adapter 可见边界 | `arm_vcpu/src/exception.rs` 将 PSCI 统一报告为现有 Hypercall，未知 SMC 和不支持的 SMC immediate 返回 -1，不向物理固件透传。非 PSCI 私有 HVC 保留原分派；未增加公共 exit 枚举 |
| OFF/RESET 用户态 ABI | 原生 x86 triple fault 的 SHUTDOWN 与原生 ARM PSCI 的 SYSTEM_EVENT 含义不同，复用职责不能照搬编码 | ARM control frontend 输出 `KVM_EXIT_SYSTEM_EVENT`，OFF/RESET 分别使用 SHUTDOWN/RESET 类型，`ndata=1`、`data[0]=0`；x86 SystemDown 映射不变 |
| 全 VM 停止扩展 | 原生 `kvm_prepare_system_event` 停止各 vCPU 并发出请求；原 x86 adapter 的普通 ioctl 转发本身不提供 ARM 同步停止协议；这不是 Firecracker 发起 stop/join 的前提 | Core 在共享锁内发布所有 vCPU 的 STOPPED、取消待处理 CPU_ON；每次真实 entry 检查 gate，包含内部重入；Linux 同步 IPI 确认运行 CPU 已回到 host 后才返回系统事件。周期 watchdog 不承担此完成条件 |
| STOPPED 与恢复 | 原生 ARM 区分电源状态与 VM 对象销毁；事件返回前在 x0 预置 INTERNAL_FAILURE | CPU_OFF 和系统事件停止后，KVM_RUN 可被信号打断；不自行重启 guest。系统事件源 x0 为 -6，用户态显式置 RUNNABLE 后会从原调用后的上下文继续；这不是整 VM 复位 |
| CPU_ON | 原生 ARM 在目标 vCPU 上处理待复位状态；PSCI 0.2 对已运行目标返回 ALREADY_ON | 启动请求在与全 VM 停止相同的锁内排队，目标 runner 应用复位；参数有效时，已运行或已排队目标返回 -4，非法或未初始化目标返回 -2。源已 STOPPED 时拒绝旧请求，防止并发系统停止后重新拉起目标 |
| 寄存器访问顺序 | 原生 `arch/arm64/kvm/arm.c` 在 GET/SET_ONE_REG 前消费待处理 PSCI reset | Core 同样先处理 pending reset，再读写寄存器；GET_REG_LIST 不触发复位。目标的寄存器、FP、系统寄存器和定时器状态复位，保留 MPIDR、stage-2 配置和 counter offset |
| 初始化与版本 | 需要区分所声明版本与实际实现的函数集合 | preferred target 推荐 `KVM_ARM_VCPU_PSCI_0_2`；初次 INIT 要求该位并按 POWER_OFF 设置状态。PSCI_VERSION 和版本 one-reg 固定为 0.2；不接受 1.x 版本写入 |

实现文件位于共享 Core 的 `virtualization/` 下：`arm_vcpu/src/{exception,smc,vcpu}.rs`
处理架构边界；`axvisor_api/src/control.rs` 定义当前 CPU、同步 kick 和可中断等待
接口；`axvisor_core/src/kvm/{state,mod,vcpu,vm}.rs` 与 `run/{mod,arm64}.rs`
管理电源状态、排队复位和 KVM ABI；`arch/aarch64.rs` 解码 PSCI，`vmm/vcpus.rs`
为 static frontend 保留基本 CPU_ON/CPU_OFF/poweroff 用途。static 不因此取得
完整的 control 生命周期功能，Linux 的直接 static VM 启动仍不在支持范围内。

当前仍有明确差异，不能写成“完整 PSCI 0.2 / 原生 KVM 等价”：

- 已实现 VERSION、CPU_OFF、CPU_ON、AFFINITY_INFO、MIGRATE_INFO_TYPE、
  SYSTEM_OFF 和 SYSTEM_RESET。CPU_SUSPEND、PSCI_FEATURES、RESET2 未实现，
  返回 NOT_SUPPORTED；timer-aware suspend 与完整 WFI/idle 协议仍是后续工作。
- 重复 `KVM_ARM_VCPU_INIT` 明确拒绝。系统事件不自动重建寄存器、RAM、设备、
  VGIC 或 VMM 状态；本批不提供全 VM 原地重建。不能把一次 OFF/RESET 的准确
  事件上报当成 guest 完整重启。
- CPU_ON 在发布成功前要求入口 PC 四字节对齐，否则返回 INVALID_PARAMS。
  Linux 6.12 原生 `kvm_psci_vcpu_on` 不做这项预检查；这是当前后端不提供相应
  指令对齐异常注入时的限制，不能说成原生必需条件。
- `SET_MP_STATE` 不取得 KVM_RUN 持有的 operation lock，可从另一线程异步
  stop/wake。原生 6.12 的普通 vCPU ioctl 在 `virt/kvm/kvm_main.c` 的 vCPU
  mutex 内串行，ARM 的该 ioctl 不走 async 分支；产品接受的并发方式更宽。
- 整次 ioctl 禁止迁移、irqfd 阻塞读线程、non-x86 整槽 pin 及 WFI 限制没有
  因本批 PSCI 修正消失。watchdog 已缩至运行 CPU 的引用计数范围，默认 5 ms；
  `axvisor_linux.watchdog=0` 只关闭周期 hrtimer，仍保留 guest virtual-timer PPI。

因此第 6 节的“HVC RESET 不支持、SMC RESET 可达物理固件”只描述原始快照。
新实现的验收入口见 [ARM64_NEXT_VALIDATION.md](../../../tools/axvisor/ARM64_NEXT_VALIDATION.md)：
优先核对 Firecracker OFF/RESET 后的 stop/join/回收，包含其他 vCPU 仍忙循环
的情况；另用隔离 QEMU 的 HVC/SMC 微型 guest 核对 ABI、CPU_ON 和同步全 VM
停止扩展，特别在 `watchdog=0` 下验证定向 kick 不依赖周期退出。完成相应用例
后再进入实板验证。文档中的用例不是执行结果，也不转移旧版本的验收状态。
