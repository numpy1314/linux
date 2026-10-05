// SPDX-License-Identifier: GPL-2.0
/* M0 diagnostics only. No EL2 writes, ownership transfer, or guest entry. */
#include <linux/bitfield.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/string.h>

#include <asm/kvm_arm.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

struct axvisor_arm64_sample {
	u64 mmfr0;
	u64 mmfr1;
	u64 pfr0;
	u64 hcr;
	unsigned int el;
	const char *reason;
};

static DEFINE_PER_CPU(struct axvisor_arm64_sample, axvisor_arm64_samples);

static void axvisor_arm64_sample_cpu(void *unused)
{
	struct axvisor_arm64_sample *s = this_cpu_ptr(&axvisor_arm64_samples);
	unsigned int vh, gic, granule, granule2;

	s->el = read_sysreg(CurrentEL) >> 2;
	s->mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	s->mmfr1 = read_sysreg(id_aa64mmfr1_el1);
	s->pfr0 = read_sysreg(id_aa64pfr0_el1);
	s->reason = "ok";
	/* Do not touch even read-only EL2 state when Linux executes at EL1. */
	if (s->el != 2) {
		s->reason = "host_not_el2";
		return;
	}
	s->hcr = read_sysreg(hcr_el2);
	vh = SYS_FIELD_GET(ID_AA64MMFR1_EL1, VH, s->mmfr1);
	gic = SYS_FIELD_GET(ID_AA64PFR0_EL1, GIC, s->pfr0);
	granule = SYS_FIELD_GET(ID_AA64MMFR0_EL1, TGRAN4, s->mmfr0);
	granule2 = SYS_FIELD_GET(ID_AA64MMFR0_EL1, TGRAN4_2, s->mmfr0);
	if (vh != ID_AA64MMFR1_EL1_VH_IMP)
		s->reason = "no_vhe";
	else if ((s->hcr & HCR_HOST_VHE_FLAGS) != HCR_HOST_VHE_FLAGS)
		s->reason = "host_hcr_not_vhe";
	else if (s->hcr & HCR_VM)
		s->reason = "stage2_already_enabled";
	else if (gic != ID_AA64PFR0_EL1_GIC_IMP &&
		 gic != ID_AA64PFR0_EL1_GIC_V4P1)
		s->reason = "no_gic_sysregs";
	else if (granule > ID_AA64MMFR0_EL1_TGRAN4_SUPPORTED_MAX ||
		 granule2 == ID_AA64MMFR0_EL1_TGRAN4_2_NI ||
		 granule2 > ID_AA64MMFR0_EL1_TGRAN4_2_52_BIT)
		s->reason = "no_4k_stage2";
}

static int __init axvisor_arm64_probe_init(void)
{
	struct device_node *node;
	const char *platform_reason = "ok";
	unsigned int cpu, sampled = 0, rejected = 0;
	bool dt_gicv3;

	node = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	dt_gicv3 = node && of_device_is_available(node);
	of_node_put(node);
	if (IS_ENABLED(CONFIG_KVM))
		platform_reason = "native_kvm_configured";
	else if (PAGE_SHIFT != 12)
		platform_reason = "unsupported_host_page_size";
	else if (!dt_gicv3 || !IS_ENABLED(CONFIG_ARM_GIC_V3))
		platform_reason = "dt_gicv3_required";
	else if (!is_hyp_mode_available())
		platform_reason = "boot_el2_unavailable";

	/* Snapshot a stable online set; this is not a hotplug admission guard. */
	cpus_read_lock();
	on_each_cpu(axvisor_arm64_sample_cpu, NULL, 1);
	for_each_online_cpu(cpu) {
		const struct axvisor_arm64_sample *s;

		s = per_cpu_ptr(&axvisor_arm64_samples, cpu);
		sampled++;
		if (strcmp(s->reason, "ok"))
			rejected++;
		pr_info("axvisor-vhe-probe: cpu=%u el=%u mmfr0=%#llx mmfr1=%#llx pfr0=%#llx hcr_read=%u hcr=%#llx reason=%s\n",
			cpu, s->el, s->mmfr0, s->mmfr1, s->pfr0,
			s->el == 2, s->hcr, s->reason);
	}
	cpus_read_unlock();
	pr_info("axvisor-vhe-probe: result=%s cpus=%u rejected=%u page_shift=%u kvm_config=%u dt_gicv3=%u platform_reason=%s runtime_ready=0\n",
		!rejected && !strcmp(platform_reason, "ok") ? "CANDIDATE" : "UNSUPPORTED",
		sampled, rejected, PAGE_SHIFT, IS_ENABLED(CONFIG_KVM),
		dt_gicv3, platform_reason);
	return 0;
}
late_initcall(axvisor_arm64_probe_init);
