# ARM Linux 适配：从 x86 实现解释到 ARM 行为对照

审计日期：2026-10-30。本轮是源码审计，没有新增运行结果，也没有修改 Core。

## 固定比较对象

| 对象 | 本轮版本 |
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

| 路径 | 原生 x86 KVM | 产品 x86 | 原生 ARM KVM | 当前 ARM 候选及结论 |
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

## 下一步与验收依据

| 类别 | 处理决定 | 要观察的证据 |
| --- | --- | --- |
| 已有 x86 方案 | 保留阻塞 fd 读取、共享取消顺序、mm 描述符引用修正、Core 现有整段撤销 | 无事件睡眠、取消竞态、失败回滚和资源释放；不能仅看 guest 能启动 |
| ARM 架构交接 | 围绕 Core world switch 复核 SIMD、VGIC/timer、物理 IRQ 所有权 | 用户/guest SIMD 内容、调度后恢复、IRQ 不丢失且不重复处理 |
| 新增限制/策略 | 先论证再决定缩小 ioctl 迁移保护范围、watchdog 覆盖范围与周期 | 同一次 KVM_RUN 中迁移、信号打断、长跑 guest、事件到达延迟、额外唤醒成本 |
| 共享 Core 边界 | 单列 control kick/WFI、PSCI 事件和 SMC 策略；不在 adapter 伪装已完成 | 原始请求如何传递、目标 vCPU 如何退出、全 VM 如何停止；没有接口则明确阻塞 |

继续以 Core `4152c3e…` 原样版本为约束。保留既有 QEMU 结果的适用范围；本次源码审计不增加通过项，不启动实板候选。后续每次逻辑提交应同时记录四方依据、必要性、差异和针对性验证结果。
