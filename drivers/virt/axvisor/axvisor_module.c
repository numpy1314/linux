// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kthread.h>

#include "axvisor_ffi.h"

#ifdef CONFIG_AXVISOR_LINUX_CONTROL
static bool axvisor_control_mode;
module_param_named(control, axvisor_control_mode, bool, 0444);
MODULE_PARM_DESC(control,
		 "initialize the AxVisor KVM control endpoint instead of static VMs");
#endif

static int axvisor_linux_core_thread(void *unused)
{
	(void)unused;
#ifdef CONFIG_AXVISOR_ARM64_VHE_SELFTEST
	return axvisor_linux_vhe_selftest();
#endif
#ifdef CONFIG_AXVISOR_LINUX_CONFORMANCE
	if (axvisor_linux_conformance_enabled())
		return axvisor_linux_conformance_run();
#endif
#ifdef CONFIG_AXVISOR_LINUX_CONTROL
	if (axvisor_control_mode) {
		int ret = axvisor_linux_core_control_boot();

		if (ret)
			pr_err("axvisor-linux: control initialization failed (%d)\n", ret);
		return ret;
	}
#endif
	axvisor_linux_core_boot();
}

static int __init axvisor_linux_init(void)
{
	struct task_struct *task;
	int ret;

#ifdef CONFIG_ARM64
	ret = axvisor_linux_arm64_vhe_enabled() ? axvisor_linux_vhe_prepare() : -EOPNOTSUPP;
	if (ret) {
		pr_err("axvisor-linux: VHE runtime admission refused (%d)\n", ret);
		return ret;
	}
#endif
	ret = axvisor_linux_console_register_endpoint();
	if (ret) {
		pr_err("axvisor-linux: failed to register console input (%d)\n",
		       ret);
		return ret;
	}

	/* Snapshot the boot FDT address while the architecture's __initdata is
	 * still alive; the AxVisor core runs asynchronously in a kthread. */
	axvisor_linux_arch_capture_host_fdt();
	/* ioremap may sleep; pre-map host DT MMIO before AxVisor's
	 * interrupt-disabled initialization starts. */
	axvisor_linux_memory_prepare_io_maps();
	/* Allocate writable custom-base storage before AxVisor spawns its
	 * per-CPU initialization tasks. */
	ret = axvisor_linux_percpu_prepare();
	if (ret) {
		pr_err("axvisor-linux: failed to prepare per-CPU storage (%d)\n", ret);
		return ret;
	}
	pr_info("axvisor-linux: starting AxVisor core kthread\n");
	task = kthread_create(axvisor_linux_core_thread, NULL, "axvisor-core");
	if (IS_ERR(task)) {
		pr_err("axvisor-linux: failed to start AxVisor core kthread\n");
		return PTR_ERR(task);
	}
	/* Bind the bootstrap thread before it can execute.  Per-CPU
	 * initialization tasks enable virtualization on the remaining CPUs. */
	kthread_bind(task, 0);
	wake_up_process(task);
	return 0;
}
late_initcall(axvisor_linux_init);
