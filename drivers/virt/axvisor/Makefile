obj-$(CONFIG_AXVISOR_ARM64_VHE_PROBE) += axvisor_arm64_probe.o
obj-$(CONFIG_AXVISOR_LINUX_WAIT) += axvisor_wait.o

obj-$(CONFIG_AXVISOR_LINUX_BRIDGE) += axvisor_linux.o
axvisor_linux-y := axvisor_module.o axvisor_runtime.o axvisor_linux_host.o axvisor_linux_core.o
axvisor_linux-$(CONFIG_ARM64) += axvisor_arm64.o
axvisor_linux-$(CONFIG_AXVISOR_LINUX_CONFORMANCE) += axvisor_conformance.o
