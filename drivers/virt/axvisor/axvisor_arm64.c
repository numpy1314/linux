// SPDX-License-Identifier: GPL-2.0
/* Linux ownership boundary. The shared Core performs the guest world switch. */
#include <linux/errno.h>
#include <linux/cpu.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/preempt.h>
#include <asm/daifflags.h>
#include <asm/kvm_arm.h>
#include <asm/neon.h>
#include <asm/simd.h>
#include <asm/sysreg.h>

#include "axvisor_ffi.h"

static unsigned long axvisor_gic_info[4];
static unsigned int axvisor_pa_range;
static u64 axvisor_midr, axvisor_isar0, axvisor_ctr, axvisor_ich_vtr;
static DEFINE_PER_CPU(struct hrtimer, axvisor_vhe_watchdog);

static enum hrtimer_restart axvisor_vhe_watchdog_expired(struct hrtimer *timer)
{
	/* The physical IRQ has already forced a guest exit. Linux owns its ACK. */
	return HRTIMER_NORESTART;
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
	if ((mmfr0 & 15) != axvisor_pa_range ||
	    read_sysreg(midr_el1) != axvisor_midr ||
	    read_sysreg(id_aa64isar0_el1) != axvisor_isar0 ||
	    read_sysreg(ctr_el0) != axvisor_ctr)
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
	if (!axvisor_vhe_cpu_supported())
		atomic_set(failed, 1);
}

int axvisor_linux_vhe_prepare(void)
{
	struct device_node *node;
	struct resource dist, redist;
	void __iomem *base;
	atomic_t failed = ATOMIC_INIT(0);
	unsigned int cpu, gic;

	if (IS_ENABLED(CONFIG_KVM) || PAGE_SHIFT != 12 ||
	    IS_ENABLED(CONFIG_ARM64_POE) || IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    IS_ENABLED(CONFIG_ARM64_SME) || IS_ENABLED(CONFIG_ARM64_MTE))
		return -EOPNOTSUPP;
	axvisor_pa_range = read_sysreg(id_aa64mmfr0_el1) & 15;
	if (axvisor_pa_range < 2 || axvisor_pa_range > 6)
		return -EOPNOTSUPP;
	/* Snapshot guest-visible CPU facts; the profile requires homogeneous CPUs. */
	if (read_sysreg(CurrentEL) != CurrentEL_EL2 ||
	    SYS_FIELD_GET(ID_AA64MMFR1_EL1, VH, read_sysreg(id_aa64mmfr1_el1)) != 1)
		return -EOPNOTSUPP;
	gic = SYS_FIELD_GET(ID_AA64PFR0_EL1, GIC, read_sysreg(id_aa64pfr0_el1));
	if (gic != 1 && gic != 3)
		return -EOPNOTSUPP;
	axvisor_midr = read_sysreg(midr_el1);
	axvisor_isar0 = read_sysreg(id_aa64isar0_el1);
	axvisor_ctr = read_sysreg(ctr_el0);
	axvisor_ich_vtr = read_sysreg(ich_vtr_el2);
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
	return atomic_read(&failed) ? -EOPNOTSUPP : 0;
}

unsigned long axvisor_linux_arm64_gic_info(unsigned int which)
{
	return which < ARRAY_SIZE(axvisor_gic_info) ? axvisor_gic_info[which] : 0;
}

long axvisor_linux_vhe_enter(void)
{
	unsigned long token;

	if (IS_ENABLED(CONFIG_KVM) || PAGE_SHIFT != 12 || !may_use_simd())
		return -EOPNOTSUPP;
	preempt_disable();
	if (!axvisor_vhe_cpu_supported())
		goto unsupported;
	/* Saves any live userspace FPSIMD/SVE state and invalidates Linux's
	 * lazy ownership before the shared Core touches the vector registers. */
	kernel_neon_begin();
	/* Mask before arming: an already serviced watchdog must never precede
	 * guest entry. A pending physical IRQ will instead force the guest out. */
	token = local_daif_save();
	hrtimer_start(this_cpu_ptr(&axvisor_vhe_watchdog), ns_to_ktime(5000000),
		      HRTIMER_MODE_REL_PINNED);
	return token;
unsupported:
	preempt_enable();
	return -EOPNOTSUPP;
}

void axvisor_linux_vhe_exit(unsigned long token)
{
	/* Guest vectors, HCR and all shared registers have already been restored.
	 * Pending physical IRQs now enter Linux's normal vector/GIC path. */
	local_daif_restore(token);
	hrtimer_cancel(this_cpu_ptr(&axvisor_vhe_watchdog));
	kernel_neon_end();
	preempt_enable();
}
