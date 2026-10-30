// SPDX-License-Identifier: GPL-2.0
/* Freestanding ARM64 init: detect host scheduling starvation in KVM_RUN.
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
#ifndef LIVENESS_ROUNDS
#define LIVENESS_ROUNDS 3
#endif
#ifndef LIVENESS_CPU
#define LIVENESS_CPU 0
#endif
#define GUEST_MEMORY_BYTES PAGE_SIZE
#define SIGNAL_DELAY_NS 100000000UL
_Static_assert(LIVENESS_ROUNDS > 0, "positive round count");
_Static_assert(LIVENESS_CPU >= 0 && LIVENESS_CPU < 64, "CPU mask range");
#define AT_FDCWD (-100)
#define O_RDWR 2
/* ARM64 overrides asm-generic fcntl.h: 0200000 is O_DIRECT here. */
#define O_DIRECTORY 040000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define EINTR 4
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
#define KVM_REG_ARM64_CORE_U64 0x6030000000100000UL
#define GUEST_RAM 0x40000000UL
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
	put(success ? "ARM64_LIVENESS_RESULT=PASS\n" : "ARM64_LIVENESS_RESULT=FAIL\n");
	long ret = sys6(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
	metric("ARM64_LIVENESS_POWEROFF_ERROR", ret);
	sys3(94, 1, 0, 0);
	for (;;) {}
}

__attribute__((noreturn)) static void fail(const char *step, long ret)
{
	put("ARM64_LIVENESS_FAIL step=");
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

static long ioctl(long fd, long request, const void *arg)
{
	return sys3(29, fd, request, (long)arg);
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


#define SIGUSR1 10
#define SIGCHLD 17
#define SA_RESTORER 0x04000000UL
struct signal_action {
	void (*handler)(int);
	u64 flags;
	void (*restorer)(void);
	u64 mask;
};
struct shared_state {
	u32 ready;
	u32 child_cpu;
	u64 sent_ns;
	long kill_result;
};
static volatile int signal_seen;

static void signal_handler(int signal)
{
	if (signal == SIGUSR1)
		signal_seen++;
}

extern void signal_restorer(void);
__asm__(".text\n.align 2\n.type signal_restorer, %function\n"
	"signal_restorer:\nmov x8, #139\nsvc #0\n"
	".size signal_restorer, .-signal_restorer\n");

static unsigned int current_cpu(void)
{
	u32 cpu;
	equal("getcpu", sys3(168, (long)&cpu, 0, 0), 0);
	return cpu;
}

static void pin_test_cpu(void)
{
	u64 mask = 1UL << LIVENESS_CPU;
	equal("sched_setaffinity", sys3(122, 0, sizeof(mask), (long)&mask), 0);
	equal("parent_cpu", current_cpu(), LIVENESS_CPU);
}

static void setup_signals(void)
{
	struct signal_action action = {
		.handler = signal_handler, .flags = SA_RESTORER,
		.restorer = signal_restorer, .mask = 0,
	};
	/* No SA_RESTART: successful cancellation must return -EINTR. */
	equal("rt_sigaction", sys6(134, SIGUSR1, (long)&action, 0, 8, 0, 0), 0);
	/* SIGCHLD must not accidentally account for the KVM_RUN interruption. */
	u64 mask = 1UL << (SIGCHLD - 1);
	equal("block_sigchld", sys6(135, 0, (long)&mask, 0, 8, 0, 0), 0);
}

__attribute__((noreturn)) static void signal_child(long parent, struct shared_state *shared)
{
	struct timespec64 poll = { .sec = 0, .nsec = 1000000 };
	while (!__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE))
		equal("child_wait_ready", sys3(101, (long)&poll, 0, 0), 0);
	struct timespec64 delay = { .sec = 0, .nsec = SIGNAL_DELAY_NS };
	equal("child_delay", sys3(101, (long)&delay, 0, 0), 0);
	shared->child_cpu = current_cpu();
	shared->sent_ns = now_ns();
	shared->kill_result = sys3(129, parent, SIGUSR1, 0);
	sys3(94, shared->kill_result == 0 ? 0 : 1, 0, 0);
	for (;;) {}
}

static void run_round(long kvm, long run_size, long round)
{
	metric("ROUND_BEGIN", round);
	long before = fd_count();
	long vm = nonnegative("create_vm", ioctl(kvm, KVM_CREATE_VM, 0));
	struct create_device device_arg = { .type = KVM_DEV_TYPE_ARM_VGIC_V3 };
	equal("create_vgic", ioctl(vm, KVM_CREATE_DEVICE, &device_arg), 0);
	long device = device_arg.fd;
	u64 dist = GIC_DIST, redist = GIC_REDIST;
	u32 nr_irqs = 64;
	set_attr(device, 0, 2, &dist);
	set_attr(device, 0, 3, &redist);
	set_attr(device, 3, 0, &nr_irqs);
	u32 *ram = map(GUEST_MEMORY_BYTES, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	ram[0] = 0x14000000; /* b .: no HLT/WFI, MMIO, SMC or cooperative exit. */
	struct memory_region region = {
		.slot = 0, .flags = 0, .guest_phys_addr = GUEST_RAM,
		.memory_size = GUEST_MEMORY_BYTES, .userspace_addr = (u64)ram,
	};
	equal("set_memory", ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region), 0);
	long vcpu = nonnegative("create_vcpu", ioctl(vm, KVM_CREATE_VCPU, 0));
	struct vcpu_init init;
	equal("preferred_target", ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init), 0);
	for (u64 i = 0; i < 7; i++)
		init.features[i] = 0;
	equal("vcpu_init", ioctl(vcpu, KVM_ARM_VCPU_INIT, &init), 0);
	set_attr(device, 4, 0, 0);
	set_reg(vcpu, 64, GUEST_RAM);
	set_reg(vcpu, 66, 0x3c5); /* EL1h, DAIF masked; host IRQs still exit. */
	void *run = map(run_size, MAP_SHARED, vcpu);
	struct shared_state *shared = map(PAGE_SIZE, MAP_SHARED | MAP_ANONYMOUS, -1);
	shared->kill_result = -1;
	signal_seen = 0;
	long parent = sys3(172, 0, 0, 0);
	/* Fork-style clone: the child is a distinct task, pinned by inherited affinity. */
	long child = nonnegative("clone", sys6(220, SIGCHLD, 0, 0, 0, 0, 0));
	if (!child)
		signal_child(parent, shared);
	metric("SIGNAL_CHILD_PID", child);
	put("BUSY_KVM_RUN_BEGIN\n");
	u64 started = now_ns();
	__atomic_store_n(&shared->ready, 1, __ATOMIC_RELEASE);
	long ret = ioctl(vcpu, KVM_RUN, 0);
	u64 stopped = now_ns();
	equal("kvm_run_eintr", ret, -EINTR);
	equal("sigusr1_handler", signal_seen, 1);
	int status = -1;
	equal("wait4_child", sys6(260, child, (long)&status, 0, 0, 0, 0), child);
	equal("child_exit_status", status, 0);
	equal("child_kill", shared->kill_result, 0);
	equal("same_cpu_sender", shared->child_cpu, LIVENESS_CPU);
	equal("parent_still_pinned", current_cpu(), LIVENESS_CPU);
	if (shared->sent_ns < started + SIGNAL_DELAY_NS || stopped < shared->sent_ns ||
	    stopped - shared->sent_ns > 2000000000UL || stopped - started > 5000000000UL)
		fail("signal_timing", stopped - started);
	metric("BUSY_RUN_NS", stopped - started);
	metric("SIGNAL_TO_EXIT_NS", stopped - shared->sent_ns);
	put("SAME_CPU_SIGNAL_EINTR=PASS\n");
	u64 teardown = now_ns();
	unmap(run, run_size);
	close_file(vcpu);
	close_file(device);
	close_file(vm);
	unmap(ram, GUEST_MEMORY_BYTES);
	unmap(shared, PAGE_SIZE);
	u64 destroyed = now_ns() - teardown;
	metric("DESTROY_NS", destroyed);
	if (destroyed > 2000000000UL)
		fail("cleanup_timeout", destroyed);
	equal("round_fd_balance", fd_count(), before);
	put("ROUND_FD_BALANCE=PASS\n");
	metric("ROUND_PASS", round);
}

void _start(void)
{
	long ret = sys6(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	long console = open_file("/dev/console", O_RDWR);
	if (console >= 0) {
		for (long fd = 0; fd < 3; fd++)
			if (fd != console)
				sys3(24, console, fd, 0);
		if (console > 2)
			sys3(57, console, 0, 0);
	}
	put("ARM64_LIVENESS_BEGIN\n");
	if (ret != 0 && ret != -16)
		fail("mount_devtmpfs", ret);
	equal("mount_proc", sys6(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0), 0);
	metric("TEST_ROUNDS", LIVENESS_ROUNDS);
	metric("TEST_CPU", LIVENESS_CPU);
	pin_test_cpu();
	setup_signals();
	long before = fd_count();
	long kvm = -1;
	for (int attempt = 0; attempt < 200; attempt++) {
		kvm = open_file("/dev/kvm", O_RDWR);
		if (kvm >= 0)
			break;
		struct timespec64 delay = { .sec = 0, .nsec = 50000000 };
		sys3(101, (long)&delay, 0, 0);
	}
	nonnegative("open_kvm", kvm);
	equal("api_version", ioctl(kvm, KVM_GET_API_VERSION, 0), 12);
	long size = nonnegative("vcpu_mmap_size", ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0));
	if (size < (long)PAGE_SIZE || size % PAGE_SIZE)
		fail("invalid_run_size", size);
	for (long round = 1; round <= LIVENESS_ROUNDS; round++)
		run_round(kvm, size, round);
	close_file(kvm);
	equal("final_fd_balance", fd_count(), before);
	put("FD_BALANCE=PASS\n");
	finish(1);
}
