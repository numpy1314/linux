// SPDX-License-Identifier: GPL-2.0
/* Freestanding ARM64 init: exercise the hosted Core's real KVM ioctl ABI.
 * Constants/layouts follow Linux v6.12 UAPI. No libc or userspace headers.
 */
#if !defined(__aarch64__)
#error "This test requires AArch64"
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

#define PAGE_SIZE 4096UL
#ifndef SMOKE_MEMORY_MIB
#define SMOKE_MEMORY_MIB 0
#endif
#ifndef SMOKE_ROUNDS
#define SMOKE_ROUNDS 3
#endif
#ifndef SMOKE_CPU_MASK
#define SMOKE_CPU_MASK 0UL
#endif
#ifndef SMOKE_OUTSIDE_CPU
#define SMOKE_OUTSIDE_CPU 0
#endif
#define GUEST_MEMORY_BYTES (SMOKE_MEMORY_MIB ? (u64)SMOKE_MEMORY_MIB * 1024 * 1024 : PAGE_SIZE)
_Static_assert(SMOKE_MEMORY_MIB >= 0, "nonnegative guest memory");
_Static_assert(SMOKE_ROUNDS > 0, "positive round count");
#define AT_FDCWD (-100)
#define O_RDWR 2
/* ARM64 overrides asm-generic fcntl.h: 0200000 is O_DIRECT here. */
#define O_DIRECTORY 040000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define EFAULT 14
#define EINTR 4
#define EXDEV 18
#define KVM_GET_API_VERSION 0xae00UL
#define KVM_CREATE_VM 0xae01UL
#define KVM_GET_VCPU_MMAP_SIZE 0xae04UL
#define KVM_CREATE_VCPU 0xae41UL
#define KVM_SET_USER_MEMORY_REGION 0x4020ae46UL
#define KVM_RUN 0xae80UL
#define KVM_SET_ONE_REG 0x4010aeacUL
#define KVM_ARM_VCPU_INIT 0x4020aeaeUL
#define KVM_ARM_PREFERRED_TARGET 0x8020aeafUL
#define KVM_CREATE_DEVICE 0xc00caee0UL
#define KVM_SET_DEVICE_ATTR 0x4018aee1UL
#define KVM_DEV_TYPE_ARM_VGIC_V3 7
#define KVM_EXIT_MMIO 6
#define KVM_REG_ARM64_CORE_U64 0x6030000000100000UL
#define GUEST_RAM 0x40000000UL
#define GUEST_MMIO 0x20000000UL
#define GIC_DIST 0x3fff0000UL
#define GIC_REDIST (GIC_DIST - 0x20000UL)

struct memory_region {
	u32 slot, flags;
	u64 guest_phys_addr, memory_size, userspace_addr;
};
struct create_device { u32 type, fd, flags; };
struct device_attr { u32 flags, group; u64 attr, addr; };
struct vcpu_init { u32 target, features[7]; };
struct one_reg { u64 id, addr; };
struct timespec64 { long sec, nsec; };
struct linux_dirent64 {
	u64 ino;
	long off;
	u16 reclen;
	u8 type;
	char name[];
};

_Static_assert(sizeof(struct memory_region) == 32, "memory region ABI");
_Static_assert(sizeof(struct create_device) == 12, "create device ABI");
_Static_assert(sizeof(struct device_attr) == 24, "device attr ABI");
_Static_assert(sizeof(struct vcpu_init) == 32, "vcpu init ABI");
_Static_assert(__builtin_offsetof(struct linux_dirent64, name) == 19, "dirent ABI");

static long sys6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc #0" : "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
		: "memory", "cc");
	return x0;
}

static long sys3(long nr, long a0, long a1, long a2)
{
	return sys6(nr, a0, a1, a2, 0, 0, 0);
}

static void put(const char *s)
{
	u64 n = 0;
	while (s[n])
		n++;
	while (n) {
		long done = sys3(64, 1, (long)s, n);
		if (done == -EINTR)
			continue;
		if (done <= 0)
			return;
		s += done;
		n -= done;
	}
}

static void number(long value)
{
	char buf[32];
	u64 pos = sizeof(buf) - 1;
	u64 n = value < 0 ? -(u64)value : (u64)value;
	buf[pos] = 0;
	do {
		buf[--pos] = '0' + n % 10;
		n /= 10;
	} while (n);
	if (value < 0)
		buf[--pos] = '-';
	put(buf + pos);
}

static void metric(const char *name, long value)
{
	put(name);
	put("=");
	number(value);
	put("\n");
}

__attribute__((noreturn)) static void finish(int success)
{
	put(success ? "ARM64_KVM_SMOKE_RESULT=PASS\n" : "ARM64_KVM_SMOKE_RESULT=FAIL\n");
	long ret = sys6(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
	metric("ARM64_KVM_SMOKE_POWEROFF_ERROR", ret);
	sys3(94, 1, 0, 0);
	for (;;) {}
}

__attribute__((noreturn)) static void fail(const char *step, long ret)
{
	put("ARM64_KVM_SMOKE_FAIL step=");
	put(step);
	put(" value=");
	number(ret);
	put("\n");
	finish(0);
}

static long nonnegative(const char *step, long ret)
{
	if (ret < 0)
		fail(step, ret);
	return ret;
}

static void equal(const char *step, long got, long expected)
{
	if (got != expected) {
		metric("EXPECTED", expected);
		fail(step, got);
	}
}

static u64 observed_ioctl_cpu_mask, created_cpu_mask, run_cpu_mask;
static unsigned int next_ioctl_cpu;

static unsigned int current_cpu(void)
{
	u32 cpu;
	equal("getcpu", sys3(168, (long)&cpu, 0, 0), 0);
	return cpu;
}

static void pin_cpu(unsigned int cpu)
{
	u64 mask = 1UL << cpu;
	equal("sched_setaffinity", sys3(122, 0, sizeof(mask), (long)&mask), 0);
	equal("affinity_actual_cpu", current_cpu(), cpu);
}

static unsigned int pool_cpu(unsigned int index)
{
	unsigned int count = 0;
	for (unsigned int cpu = 0; cpu < 64; cpu++)
		if (SMOKE_CPU_MASK & (1UL << cpu))
			count++;
	if (!count)
		fail("empty_test_cpu_pool", 0);
	index %= count;
	for (unsigned int cpu = 0; cpu < 64; cpu++)
		if (SMOKE_CPU_MASK & (1UL << cpu)) {
			if (!index)
				return cpu;
			index--;
		}
	fail("bad_test_cpu_pool", index);
}

static long raw_ioctl(long fd, long request, const void *arg)
{
	return sys3(29, fd, request, (long)arg);
}

static long ioctl_on_pool_cpu(long fd, long request, const void *arg, unsigned int index)
{
	if (SMOKE_CPU_MASK) {
		pin_cpu(pool_cpu(index));
		observed_ioctl_cpu_mask |= 1UL << current_cpu();
	}
	return raw_ioctl(fd, request, arg);
}

static long ioctl(long fd, long request, const void *arg)
{
	return ioctl_on_pool_cpu(fd, request, arg, next_ioctl_cpu++);
}

static long open_file(const char *path, long flags)
{
	return sys3(56, AT_FDCWD, (long)path, flags);
}

static void close_file(long fd)
{
	equal("close", sys3(57, fd, 0, 0), 0);
}

static void *map(long size, long flags, long fd)
{
	long addr = sys6(222, 0, size, PROT_READ | PROT_WRITE, flags, fd, 0);
	return (void *)nonnegative("mmap", addr);
}

static void unmap(void *addr, long size)
{
	equal("munmap", sys3(215, (long)addr, size, 0), 0);
}

static u64 now_ns(void)
{
	struct timespec64 ts;
	equal("clock_gettime", sys3(113, 1, (long)&ts, 0), 0);
	return ts.sec * 1000000000UL + ts.nsec;
}

static long fd_count(void)
{
	char data[4096] __attribute__((aligned(8)));
	long count = 0;
	long fd = nonnegative("open_fd_directory", open_file("/proc/self/fd", O_DIRECTORY));
	for (;;) {
		long length = nonnegative("getdents64", sys3(61, fd, (long)data, sizeof(data)));
		if (!length)
			break;
		for (long pos = 0; pos < length;) {
			struct linux_dirent64 *entry = (void *)(data + pos);
			if (length - pos < 20 || entry->reclen < 20 || entry->reclen > length - pos)
				fail("invalid_dirent", pos);
			if (entry->name[0] != '.')
				count++;
			pos += entry->reclen;
		}
	}
	close_file(fd);
	return count - 1; /* Exclude this function's directory fd. */
}

static long next_available_fd(void)
{
	long fd = nonnegative("probe_unused_fd", open_file("/dev/console", O_RDWR));
	close_file(fd);
	return fd;
}

static void report_meminfo(void)
{
	char data[4096];
	long fd = nonnegative("open_meminfo", open_file("/proc/meminfo", 0));
	long size = nonnegative("read_meminfo", sys3(63, fd, (long)data, sizeof(data) - 1));
	close_file(fd);
	data[size] = 0;
	const char key[] = "MemAvailable:";
	for (long i = 0; i < size; i++) {
		long k = 0;
		while (key[k] && i + k < size && data[i + k] == key[k])
			k++;
		if (!key[k]) {
			long end = i;
			while (end < size && data[end] != '\n')
				end++;
			data[end] = 0;
			put("HOST_");
			put(data + i);
			put("\n");
			return;
		}
	}
	fail("missing_memavailable", size);
}

static long create_vgic_with_fault_check(long vm)
{
	struct create_device *request = map(PAGE_SIZE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	request->type = KVM_DEV_TYPE_ARM_VGIC_V3;
	request->fd = 0xffffffffU;
	request->flags = 0;
	long before = fd_count();
	long available = next_available_fd();
	equal("mprotect_readonly", sys3(226, (long)request, PAGE_SIZE, PROT_READ), 0);
	equal("create_device_readonly_EFAULT", ioctl(vm, KVM_CREATE_DEVICE, request), -EFAULT);
	equal("create_device_failed_fd_count", fd_count(), before);
	equal("create_device_failed_fd_reservation", next_available_fd(), available);
	equal("create_device_failed_output", request->fd, 0xffffffffU);
	equal("mprotect_readwrite", sys3(226, (long)request, PAGE_SIZE, PROT_READ | PROT_WRITE), 0);
	equal("create_device_retry", ioctl(vm, KVM_CREATE_DEVICE, request), 0);
	long device = request->fd;
	if (device < 0 || device == 0xffffffffU)
		fail("create_device_invalid_fd", device);
	equal("create_device_retry_fd_number", device, available);
	equal("create_device_retry_fd_count", fd_count(), before + 1);
	unmap(request, PAGE_SIZE);
	put("CREATE_DEVICE_EFAULT_RETRY=PASS\n");
	return device;
}

static void set_attr(long fd, u32 group, u64 attr, const void *value)
{
	struct device_attr request = { .flags = 0, .group = group, .attr = attr, .addr = (u64)value };
	equal("set_device_attr", ioctl(fd, KVM_SET_DEVICE_ATTR, &request), 0);
}

static void set_reg(long fd, u64 word_offset, u64 value)
{
	struct one_reg request = { .id = KVM_REG_ARM64_CORE_U64 | word_offset, .addr = (u64)&value };
	equal("set_one_reg", ioctl(fd, KVM_SET_ONE_REG, &request), 0);
}

static void run_round(long kvm, long run_size, long round)
{
	metric("ROUND_BEGIN", round);
	long before = fd_count();
	long vm = nonnegative("create_vm", ioctl_on_pool_cpu(kvm, KVM_CREATE_VM, 0, round - 1));
	if (SMOKE_CPU_MASK) {
		metric("CREATE_VM_CPU", current_cpu());
		created_cpu_mask |= 1UL << current_cpu();
	}
	long device = create_vgic_with_fault_check(vm);
	u64 dist = GIC_DIST, redist = GIC_REDIST;
	u32 nr_irqs = 64;
	set_attr(device, 0, 2, &dist);
	set_attr(device, 0, 3, &redist);
	set_attr(device, 3, 0, &nr_irqs);
	u32 *ram = map(GUEST_MEMORY_BYTES, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	ram[0] = 0xb9000001; /* str w1, [x0]: one MMIO write */
	ram[1] = 0x14000000; /* b .: should never execute in this test */
	struct memory_region region = {
		.slot = 0, .flags = 0, .guest_phys_addr = GUEST_RAM,
		.memory_size = GUEST_MEMORY_BYTES, .userspace_addr = (u64)ram,
	};
	equal("set_user_memory_region", ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region), 0);
	long vcpu = nonnegative("create_vcpu", ioctl(vm, KVM_CREATE_VCPU, 0));
	struct vcpu_init init;
	equal("preferred_target", ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init), 0);
	metric("VCPU_TARGET", init.target);
	for (u64 i = 0; i < 7; i++)
		init.features[i] = 0;
	init.features[0] = 1U << 2; /* KVM_ARM_VCPU_PSCI_0_2 */
	equal("vcpu_init", ioctl(vcpu, KVM_ARM_VCPU_INIT, &init), 0);
	set_attr(device, 4, 0, 0);
	set_reg(vcpu, 0, GUEST_MMIO); /* x0 */
	set_reg(vcpu, 2, 0x12345678UL + round); /* x1 */
	set_reg(vcpu, 64, GUEST_RAM); /* PC: offset in units of u32 */
	set_reg(vcpu, 66, 0x3c5); /* EL1h, DAIF masked */
	u8 *run = map(run_size, MAP_SHARED, vcpu);
	put("KVM_RUN_BEGIN\n");
	long ret = -EINTR;
	for (int attempts = 0; attempts < 100 && ret == -EINTR; attempts++)
		ret = ioctl_on_pool_cpu(vcpu, KVM_RUN, 0, round);
	equal("kvm_run", ret, 0);
	if (SMOKE_CPU_MASK) {
		metric("RUN_CPU", current_cpu());
		run_cpu_mask |= 1UL << current_cpu();
	}
	equal("exit_reason", *(u32 *)(run + 8), KVM_EXIT_MMIO);
	equal("mmio_address", *(u64 *)(run + 32), GUEST_MMIO);
	equal("mmio_length", *(u32 *)(run + 48), 4);
	equal("mmio_write", run[52], 1);
	equal("mmio_value", *(u32 *)(run + 40), 0x12345678UL + round);
	put("MMIO_WRITE=PASS\n");
	if (SMOKE_CPU_MASK)
		pin_cpu(SMOKE_OUTSIDE_CPU);
	u64 start = now_ns();
	unmap(run, run_size);
	close_file(vcpu);
	close_file(device);
	close_file(vm);
	unmap(ram, GUEST_MEMORY_BYTES);
	metric("DESTROY_NS", now_ns() - start);
	equal("round_fd_balance", fd_count(), before);
	put("ROUND_FD_BALANCE=PASS\n");
	if (SMOKE_CPU_MASK)
		put("OFFPOOL_CLOSE=PASS\n");
	report_meminfo();
	metric("ROUND_PASS", round);
}

void _start(void)
{
	/* The archive supplies /dev/console; devtmpfs supplies the registered kvm node. */
	long ret = sys6(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	long console = open_file("/dev/console", O_RDWR);
	if (console >= 0) {
		for (long fd = 0; fd < 3; fd++)
			if (fd != console)
				sys3(24, console, fd, 0);
		if (console > 2)
			sys3(57, console, 0, 0);
	}
	put("ARM64_KVM_SMOKE_BEGIN\n");
	metric("GUEST_MEMORY_BYTES", GUEST_MEMORY_BYTES);
	metric("TEST_ROUNDS", SMOKE_ROUNDS);
	metric("TEST_CPU_MASK", SMOKE_CPU_MASK);
	if (ret != 0 && ret != -16)
		fail("mount_devtmpfs", ret);
	equal("mount_proc", sys6(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0), 0);
	long baseline = fd_count();
	long kvm = -1;
	for (int attempt = 0; attempt < 200; attempt++) {
		kvm = open_file("/dev/kvm", O_RDWR);
		if (kvm >= 0)
			break;
		struct timespec64 delay = { .sec = 0, .nsec = 50000000 };
		sys3(101, (long)&delay, 0, 0);
	}
	nonnegative("open_kvm", kvm);
	if (SMOKE_CPU_MASK) {
		pin_cpu(SMOKE_OUTSIDE_CPU);
		long before = fd_count();
		long available = next_available_fd();
		equal("outside_pool_api", raw_ioctl(kvm, KVM_GET_API_VERSION, 0), -EXDEV);
		equal("outside_pool_create_vm", raw_ioctl(kvm, KVM_CREATE_VM, 0), -EXDEV);
		equal("outside_pool_fd_balance", fd_count(), before);
		equal("outside_pool_fd_reservation", next_available_fd(), available);
		put("OFFPOOL_IOCTL=PASS\n");
	}
	equal("api_version", ioctl(kvm, KVM_GET_API_VERSION, 0), 12);
	long size = nonnegative("vcpu_mmap_size", ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0));
	if (size < (long)PAGE_SIZE || size % PAGE_SIZE)
		fail("invalid_vcpu_mmap_size", size);
	metric("VCPU_MMAP_SIZE", size);
	report_meminfo();
	for (long round = 1; round <= SMOKE_ROUNDS; round++)
		run_round(kvm, size, round);
	if (SMOKE_CPU_MASK) {
		equal("pool_ioctl_coverage", observed_ioctl_cpu_mask, SMOKE_CPU_MASK);
		equal("pool_create_coverage", created_cpu_mask, SMOKE_CPU_MASK);
		equal("pool_run_coverage", run_cpu_mask, SMOKE_CPU_MASK);
		put("POOL_ROTATION=PASS\n");
	}
	close_file(kvm);
	equal("final_fd_balance", fd_count(), baseline);
	put("FD_BALANCE=PASS\n");
	finish(1);
}
