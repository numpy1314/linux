// SPDX-License-Identifier: GPL-2.0
/* Linux ownership boundary. The shared Core performs the guest world switch. */
#include <linux/errno.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <clocksource/arm_arch_timer.h>
#include <linux/of_address.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/preempt.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <asm/kvm_arm.h>
#include <asm/neon.h>
#include <asm/simd.h>
#include <asm/sysreg.h>

#include "axvisor_ffi.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
#error "The VHE adapter requires schedulable kernel FPSIMD support from Linux 6.12"
#endif

static unsigned long axvisor_gic_info[4];
static unsigned int axvisor_pa_range;
static u64 axvisor_ich_vtr;
static char *axvisor_guest_cpus;
static cpumask_t axvisor_guest_cpu_pool;
module_param_named(guest_cpus, axvisor_guest_cpus, charp, 0444);
MODULE_PARM_DESC(guest_cpus,
		"CPU list for control ioctls; default all online CPUs; each ioctl is migration-pinned");

/* These raw CPU facts must agree within the execution pool. In particular,
 * Core reads PFR0/PFR1/ISAR1 while constructing the VM to select HCR.TID3.
 * Keep physical CPU ids unchanged: Core per-CPU storage covers all online
 * Linux CPUs, including CPUs used only for host work and teardown.
 */
struct axvisor_vhe_guest_profile {
	u64 midr, revidr, ctr, clidr, cntfrq;
	u64 pfr0, pfr1, isar0, isar1, isar2;
	u64 mmfr0, mmfr1, mmfr2, dfr0, dfr1, afr0, afr1;
};
static struct axvisor_vhe_guest_profile axvisor_guest_profile;
static DEFINE_PER_CPU(struct hrtimer, axvisor_vhe_watchdog);
static DEFINE_PER_CPU(unsigned int, axvisor_vhe_cpu_runs);
static DEFINE_MUTEX(axvisor_vhe_run_lock);
static unsigned int axvisor_vhe_running;
static bool axvisor_vhe_ready;
static int axvisor_vtimer_irq;
static unsigned int axvisor_vtimer_irq_flags;
static DEFINE_PER_CPU(unsigned int, axvisor_vtimer_irq_cookie);

#define AXVISOR_VHE_WATCHDOG_NS 5000000

static enum hrtimer_restart axvisor_vhe_watchdog_expired(struct hrtimer *timer)
{
	/* Linux owns the physical IRQ and its ACK. Keep a future deadline armed
	 * even when a previous expiry was serviced before the next guest entry.
	 * Only CPUs hosting an active KVM_RUN have a timer. Multiple sleeping
	 * vCPU tasks on one CPU share it until the last run on that CPU ends.
	 */
	hrtimer_forward_now(timer, ns_to_ktime(AXVISOR_VHE_WATCHDOG_NS));
	return HRTIMER_RESTART;
}

static void axvisor_vhe_mask_guest_timer(void)
{
	/* In VHE the EL02 accessor selects the guest EL1 virtual timer. Linux
	 * itself uses the separate hypervisor physical timer, which is untouched.
	 */
	write_sysreg_s(read_sysreg_s(SYS_CNTV_CTL_EL02) | ARCH_TIMER_CTRL_IT_MASK,
		       SYS_CNTV_CTL_EL02);
	isb();
}

static irqreturn_t axvisor_vhe_timer_irq(int irq, void *cookie)
{
	(void)irq;
	(void)cookie;
	/* Core saved the expired guest timer, queued its virtual PPI and masked
	 * its physical source before restoring Linux IRQs. A pending GIC level
	 * may still reach Linux; mask defensively so it cannot become a storm.
	 * IRQ acknowledgement/completion remains in Linux's normal GIC path.
	 */
	axvisor_vhe_mask_guest_timer();
	return IRQ_HANDLED;
}

static void axvisor_vhe_read_guest_profile(struct axvisor_vhe_guest_profile *p)
{
	p->midr = read_sysreg(midr_el1);
	p->revidr = read_sysreg_s(SYS_REVIDR_EL1);
	p->ctr = read_sysreg(ctr_el0);
	p->clidr = read_sysreg(clidr_el1);
	p->cntfrq = read_sysreg(cntfrq_el0);
	p->pfr0 = read_sysreg(id_aa64pfr0_el1);
	p->pfr1 = read_sysreg_s(SYS_ID_AA64PFR1_EL1);
	p->isar0 = read_sysreg(id_aa64isar0_el1);
	p->isar1 = read_sysreg(id_aa64isar1_el1);
	p->isar2 = read_sysreg_s(SYS_ID_AA64ISAR2_EL1);
	p->mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	p->mmfr1 = read_sysreg(id_aa64mmfr1_el1);
	p->mmfr2 = read_sysreg_s(SYS_ID_AA64MMFR2_EL1);
	p->dfr0 = read_sysreg(id_aa64dfr0_el1);
	p->dfr1 = read_sysreg_s(SYS_ID_AA64DFR1_EL1);
	p->afr0 = read_sysreg_s(SYS_ID_AA64AFR0_EL1);
	p->afr1 = read_sysreg_s(SYS_ID_AA64AFR1_EL1);
}

static void axvisor_vhe_capture_guest_profile(void *result)
{
	int *ret = result;

	if (read_sysreg(CurrentEL) != CurrentEL_EL2 ||
	    SYS_FIELD_GET(ID_AA64MMFR1_EL1, VH, read_sysreg(id_aa64mmfr1_el1)) != 1) {
		*ret = -EOPNOTSUPP;
		return;
	}
	axvisor_vhe_read_guest_profile(&axvisor_guest_profile);
	axvisor_pa_range = axvisor_guest_profile.mmfr0 & 15;
	axvisor_ich_vtr = read_sysreg(ich_vtr_el2);
	*ret = 0;
}

static bool axvisor_vhe_cpu_supported(void)
{
	u64 hcr, mmfr0, pfr0;
	unsigned int gic, granule, granule2, lrs;

	if (read_sysreg(CurrentEL) != CurrentEL_EL2)
		return false;
	hcr = read_sysreg(hcr_el2);
	if ((hcr & HCR_HOST_VHE_FLAGS) != HCR_HOST_VHE_FLAGS || (hcr & HCR_VM))
		return false;
	if (SYS_FIELD_GET(ID_AA64MMFR1_EL1, VH, read_sysreg(id_aa64mmfr1_el1)) != 1)
		return false;
	mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	if ((mmfr0 & 15) != axvisor_pa_range)
		return false;
	pfr0 = read_sysreg(id_aa64pfr0_el1);
	gic = SYS_FIELD_GET(ID_AA64PFR0_EL1, GIC, pfr0);
	granule = SYS_FIELD_GET(ID_AA64MMFR0_EL1, TGRAN4, mmfr0);
	granule2 = SYS_FIELD_GET(ID_AA64MMFR0_EL1, TGRAN4_2, mmfr0);
	if ((gic != 1 && gic != 3) || granule > 1 || granule2 == 1 || granule2 > 3)
		return false;
	if (read_sysreg(ich_vtr_el2) != axvisor_ich_vtr)
		return false;
	/* Four LRs are the minimum supported virtual GIC CPU interface. */
	lrs = (axvisor_ich_vtr & 31) + 1;
	if (lrs < 4 || lrs > 16 || (read_sysreg(ich_hcr_el2) & 1) ||
	    (read_sysreg(ich_elrsr_el2) & ((1UL << lrs) - 1)) != (1UL << lrs) - 1)
		return false;
	return true;
}

static void axvisor_vhe_sample_cpu(void *failed)
{
	struct axvisor_vhe_guest_profile profile;

	if (!axvisor_vhe_cpu_supported()) {
		pr_err("axvisor-linux: CPU%u lacks common VHE/PA/GIC capabilities\n",
		       smp_processor_id());
		atomic_set(failed, 1);
		return;
	}
	if (!cpumask_test_cpu(smp_processor_id(), &axvisor_guest_cpu_pool))
		return;
	axvisor_vhe_read_guest_profile(&profile);
	if (memcmp(&profile, &axvisor_guest_profile, sizeof(profile))) {
		pr_err("axvisor-linux: CPU%u differs from guest CPU pool profile\n",
		       smp_processor_id());
		atomic_set(failed, 1);
	}
}

static void axvisor_vhe_start_watchdog(void)
{
	axvisor_vhe_mask_guest_timer();
	enable_percpu_irq(axvisor_vtimer_irq, axvisor_vtimer_irq_flags);
	hrtimer_start(this_cpu_ptr(&axvisor_vhe_watchdog),
		      ns_to_ktime(AXVISOR_VHE_WATCHDOG_NS),
		      HRTIMER_MODE_REL_PINNED);
}

/* KVM_RUN is migration-pinned; the caller holds axvisor_vhe_run_lock. */
static void axvisor_vhe_stop_watchdog(void)
{
	disable_percpu_irq(axvisor_vtimer_irq);
	axvisor_vhe_mask_guest_timer();
	hrtimer_cancel(this_cpu_ptr(&axvisor_vhe_watchdog));
}

int axvisor_linux_vhe_prepare(void)
{
	struct device_node *node;
	struct resource dist, redist;
	struct arch_timer_kvm_info *timer_info;
	void __iomem *base;
	atomic_t failed = ATOMIC_INIT(0);
	unsigned int cpu;
	int ret, profile_ret = -EOPNOTSUPP;

	if (IS_ENABLED(CONFIG_KVM) || PAGE_SHIFT != 12 ||
	    !IS_ENABLED(CONFIG_KERNEL_MODE_NEON) ||
	    IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) ||
	    IS_ENABLED(CONFIG_ARM64_POE) || IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    IS_ENABLED(CONFIG_ARM64_SME) || IS_ENABLED(CONFIG_ARM64_MTE))
		return -EOPNOTSUPP;
	/* Core initializes ids 0..get_host_cpu_num(). Preserve that contract
	 * rather than silently renumbering a sparse host topology.
	 */
	if (num_online_cpus() != nr_cpu_ids) {
		pr_err("axvisor-linux: VHE requires dense online CPU ids starting at zero\n");
		return -EOPNOTSUPP;
	}
	if (axvisor_guest_cpus && *axvisor_guest_cpus) {
		ret = cpulist_parse(axvisor_guest_cpus, &axvisor_guest_cpu_pool);
		if (ret)
			return ret;
	} else {
		cpumask_copy(&axvisor_guest_cpu_pool, cpu_online_mask);
	}
	if (cpumask_empty(&axvisor_guest_cpu_pool) ||
	    !cpumask_subset(&axvisor_guest_cpu_pool, cpu_online_mask))
		return -EINVAL;

	/* The boot CPU may be outside the pool (e.g. an RK3588 Cortex-A55).
	 * Sample the reference on an admitted guest CPU, not on the caller.
	 */
	ret = smp_call_function_single(cpumask_first(&axvisor_guest_cpu_pool),
			axvisor_vhe_capture_guest_profile, &profile_ret, 1);
	if (ret || profile_ret)
		return ret ? ret : profile_ret;
	if (axvisor_pa_range < 2 || axvisor_pa_range > 6)
		return -EOPNOTSUPP;
	/* Core checks this before starting its per-CPU initialization. Reject a
	 * mismatched build here, while Linux can still return an ordinary error.
	 */
	if (axvisor_linux_arm64_guest_levels() != (axvisor_pa_range >= 4 ? 4 : 3)) {
		pr_err("axvisor-linux: Core page-table level does not match host PA range\n");
		return -EOPNOTSUPP;
	}
	node = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!node || !of_device_is_available(node) ||
	    of_address_to_resource(node, 0, &dist) ||
	    of_address_to_resource(node, 1, &redist)) {
		of_node_put(node);
		return -EOPNOTSUPP;
	}
	of_node_put(node);
	base = ioremap(dist.start, PAGE_SIZE);
	if (!base)
		return -ENOMEM;
	axvisor_gic_info[0] = dist.start;
	axvisor_gic_info[1] = redist.start;
	axvisor_gic_info[2] = readl(base + 4);
	axvisor_gic_info[3] = readl(base + 8);
	iounmap(base);
	for_each_possible_cpu(cpu) {
		struct hrtimer *timer = per_cpu_ptr(&axvisor_vhe_watchdog, cpu);

		hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		timer->function = axvisor_vhe_watchdog_expired;
	}
	cpus_read_lock();
	on_each_cpu(axvisor_vhe_sample_cpu, &failed, 1);
	cpus_read_unlock();
	if (atomic_read(&failed))
		return -EOPNOTSUPP;

	/* This information is populated by the architectural timer driver even
	 * with CONFIG_KVM=n. Request the guest virtual timer PPI so its deadline
	 * causes an exit directly instead of waiting for the liveness watchdog.
	 */
	timer_info = arch_timer_get_kvm_info();
	if (timer_info->virtual_irq <= 0)
		return -ENODEV;
	axvisor_vtimer_irq = timer_info->virtual_irq;
	axvisor_vtimer_irq_flags = irq_get_trigger_type(axvisor_vtimer_irq);
	if (axvisor_vtimer_irq_flags != IRQ_TYPE_LEVEL_HIGH &&
	    axvisor_vtimer_irq_flags != IRQ_TYPE_LEVEL_LOW)
		return -EINVAL;
	ret = request_percpu_irq(axvisor_vtimer_irq, axvisor_vhe_timer_irq,
				"axvisor guest vtimer", &axvisor_vtimer_irq_cookie);
	if (ret)
		return ret;
	/* No further fallible initialization follows. Later module-init errors
	 * release this registration through axvisor_linux_vhe_cleanup().
	 */
	WRITE_ONCE(axvisor_vhe_ready, true);
	pr_info("axvisor-linux: guest CPU pool=%*pbl; control ioctls stay on their entry CPU\n",
		cpumask_pr_args(&axvisor_guest_cpu_pool));
	return 0;
}

unsigned long axvisor_linux_arm64_gic_info(unsigned int which)
{
	return which < ARRAY_SIZE(axvisor_gic_info) ? axvisor_gic_info[which] : 0;
}

/*
 * Pin migration, not scheduling or IRQ delivery. A concurrent affinity change
 * may wait until this ioctl returns; unlike native KVM, migration within one
 * KVM_RUN is not supported by this first pool profile. Separate calls may run
 * on different pool CPUs. Userspace must set its affinity before entering.
 * Apply this to all control ioctls: KVM_CREATE_VM already constructs the
 * architectural vCPUs and samples hardware facts before KVM_CREATE_VCPU.
 */
int axvisor_linux_vhe_control_begin(void)
{
	if (!READ_ONCE(axvisor_vhe_ready))
		return -ENODEV;
	migrate_disable();
	if (!cpumask_test_cpu(smp_processor_id(), &axvisor_guest_cpu_pool)) {
		migrate_enable();
		return -EXDEV;
	}
	return 0;
}

void axvisor_linux_vhe_control_end(void)
{
	migrate_enable();
}

/*
 * This brackets a KVM_RUN ioctl, including its sleep/yield paths. Linux 6.12
 * tracks kernel FPSIMD ownership with TIF_KERNEL_FPSTATE and saves/restores it
 * across scheduling. Do not disable preemption or IRQs for this whole scope.
 * The unmodified Core's with_bound_vcpu/IrqSave protects each bounded guest
 * batch, and restores host EL2/FPSIMD state before releasing that guard.
 */
int axvisor_linux_vhe_run_begin(void)
{
	int ret;

	if (!READ_ONCE(axvisor_vhe_ready) || !may_use_simd())
		return -EOPNOTSUPP;
	/* A user ioctl is not a nested kernel SIMD context. Reject an unexpected
	 * caller instead of hitting kernel_neon_begin()'s nesting BUG_ON.
	 */
	if (test_thread_flag(TIF_KERNEL_FPSTATE))
		return -EBUSY;
	ret = mutex_lock_interruptible(&axvisor_vhe_run_lock);
	if (ret)
		return ret;
	if (!axvisor_vhe_ready) {
		mutex_unlock(&axvisor_vhe_run_lock);
		return -ENODEV;
	}
	/* control_begin() pins migration, while the mutex serializes tasks
	 * sharing a CPU. Like KVM's per-vCPU timer load/put ownership, leave
	 * host-only CPUs alone. The periodic budget itself is still specific
	 * to this adapter: shared Core has no request/kick callback.
	 */
	if (!*this_cpu_ptr(&axvisor_vhe_cpu_runs))
		axvisor_vhe_start_watchdog();
	(*this_cpu_ptr(&axvisor_vhe_cpu_runs))++;
	axvisor_vhe_running++;
	mutex_unlock(&axvisor_vhe_run_lock);
	kernel_neon_begin();
	return 0;
}

void axvisor_linux_vhe_run_end(void)
{
	kernel_neon_end();
	mutex_lock(&axvisor_vhe_run_lock);
	if (WARN_ON_ONCE(!axvisor_vhe_running ||
			 !*this_cpu_ptr(&axvisor_vhe_cpu_runs))) {
		mutex_unlock(&axvisor_vhe_run_lock);
		return;
	}
	if (!--(*this_cpu_ptr(&axvisor_vhe_cpu_runs)))
		axvisor_vhe_stop_watchdog();
	axvisor_vhe_running--;
	mutex_unlock(&axvisor_vhe_run_lock);
}

/*
 * Core's softfloat build has no live SIMD values outside its IRQ-disabled
 * world-switch code. A sleepable Linux callback may itself enter kernel NEON
 * (for example a filesystem crypto path), so release our ownership for that
 * call and reacquire it before returning to Core. Hard IRQs and IRQ-disabled
 * callbacks retain the existing ownership and cannot use kernel NEON.
 */
bool axvisor_linux_vhe_pause_simd(void)
{
	if (!in_task() || irqs_disabled() ||
	    !test_thread_flag(TIF_KERNEL_FPSTATE))
		return false;
	kernel_neon_end();
	return true;
}

void axvisor_linux_vhe_resume_simd(bool paused)
{
	if (paused)
		kernel_neon_begin();
}

/* Only used before a failed initialization has exposed the control endpoint. */
void axvisor_linux_vhe_cleanup(void)
{
	mutex_lock(&axvisor_vhe_run_lock);
	if (WARN_ON_ONCE(axvisor_vhe_running)) {
		mutex_unlock(&axvisor_vhe_run_lock);
		return;
	}
	if (axvisor_vhe_ready) {
		WRITE_ONCE(axvisor_vhe_ready, false);
		free_percpu_irq(axvisor_vtimer_irq, &axvisor_vtimer_irq_cookie);
	}
	mutex_unlock(&axvisor_vhe_run_lock);
}
