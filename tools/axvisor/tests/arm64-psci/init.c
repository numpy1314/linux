// SPDX-License-Identifier: GPL-2.0
/* Real ARM64 KVM ioctls and guest instructions. No libc or host-only mocks. */
#if !defined(__aarch64__)
#error "AArch64 only"
#endif
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

#define PAGE_SIZE 4096UL
#define RAM_SIZE (2 * PAGE_SIZE)
#define RAM 0x40000000UL
#define MMIO 0x20000000UL
#define O_DIRECTORY 040000
#define KVM_CREATE_VM 0xae01UL
#define KVM_CREATE_VCPU 0xae41UL
#define KVM_GET_VCPU_MMAP_SIZE 0xae04UL
#define KVM_SET_USER_MEMORY_REGION 0x4020ae46UL
#define KVM_RUN 0xae80UL
#define KVM_GET_ONE_REG 0x4010aeabUL
#define KVM_SET_ONE_REG 0x4010aeacUL
#define KVM_ARM_VCPU_INIT 0x4020aeaeUL
#define KVM_ARM_PREFERRED_TARGET 0x8020aeafUL
#define KVM_GET_MP_STATE 0x8004ae98UL
#define KVM_SET_MP_STATE 0x4004ae99UL
#define KVM_CREATE_DEVICE 0xc00caee0UL
#define KVM_SET_DEVICE_ATTR 0x4018aee1UL
#define KVM_REG_CORE 0x6030000000100000UL
#define SYSTEM_EVENT 24
#define MMIO_EXIT 6
#define UNKNOWN_EXIT 0
#define STOPPED 5
#define RUNNABLE 0
#define SIGUSR1 10
#define SIGCHLD 17
#define EINTR 4

struct timespec64 { long sec, nsec; };
struct memory_region { u32 slot, flags; u64 gpa, size, hva; };
struct create_device { u32 type, fd, flags; };
struct device_attr { u32 flags, group; u64 attr, addr; };
struct vcpu_init { u32 target, features[7]; };
struct one_reg { u64 id, addr; };
struct linux_dirent64 { u64 ino; long off; u16 reclen; u8 type; char name[]; };
struct signal_action { void (*handler)(int); u64 flags; void (*restorer)(void); u64 mask; };
struct worker_state { long fd, ret; u32 started, returned, signal_count; };
struct fixture { long vm, device, vcpu[2], run_size; u8 *run[2]; u8 *ram; u64 call_pc; };

_Static_assert(sizeof(struct memory_region) == 32, "memory UAPI");
_Static_assert(sizeof(struct device_attr) == 24, "device UAPI");
_Static_assert(sizeof(struct one_reg) == 16, "register UAPI");

void *memset(void *p, int c, u64 size)
{
	volatile u8 *q = p;
	for (u64 i = 0; i < size; i++) q[i] = (u8)c;
	return p;
}

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
		: "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
	return x0;
}
static long sys3(long nr, long a, long b, long c) { return sys6(nr, a, b, c, 0, 0, 0); }
static long ioctl(long fd, long cmd, const void *arg) { return sys3(29, fd, cmd, (long)arg); }
static void put(const char *s)
{
	u64 n = 0;
	while (s[n]) n++;
	while (n) {
		long done = sys3(64, 1, (long)s, n);
		if (done == -EINTR) continue;
		if (done <= 0) return;
		s += done; n -= done;
	}
}
static void metric(const char *name, long value)
{
	char text[32]; u64 p = sizeof(text) - 1;
	u64 n = value < 0 ? -(u64)value : (u64)value;
	text[p] = 0;
	do { text[--p] = '0' + n % 10; n /= 10; } while (n);
	if (value < 0) text[--p] = '-';
	put(name); put("="); put(text + p); put("\n");
}
__attribute__((noreturn)) static void finish(int success)
{
	put(success ? "ARM64_PSCI_RESULT=PASS\n" : "ARM64_PSCI_RESULT=FAIL\n");
	metric("HOST_POWEROFF_ERROR", sys6(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0));
	for (;;) {}
}
__attribute__((noreturn)) static void fail(const char *name, long got)
{
	put("ARM64_PSCI_FAIL "); metric(name, got); finish(0);
}
static long need(const char *name, long got) { if (got < 0) fail(name, got); return got; }
static void eq(const char *name, long got, long expected)
{
	if (got != expected) { metric("EXPECTED", expected); fail(name, got); }
}
static void *map(long size, long flags, long fd)
{
	return (void *)need("mmap", sys6(222, 0, size, 3, flags, fd, 0));
}
static void unmap(void *p, long n) { eq("munmap", sys3(215, (long)p, n, 0), 0); }
static void close_fd(long fd) { eq("close", sys3(57, fd, 0, 0), 0); }
static u64 now(void)
{
	struct timespec64 ts;
	eq("clock", sys3(113, 1, (long)&ts, 0), 0);
	return ts.sec * 1000000000UL + ts.nsec;
}
static void delay(u64 ns)
{
	struct timespec64 ts = { .sec = ns / 1000000000UL, .nsec = ns % 1000000000UL };
	struct timespec64 remain;
	long ret;
	do {
		ret = sys3(101, (long)&ts, (long)&remain, 0);
		if (ret == -EINTR) ts = remain;
	} while (ret == -EINTR);
	eq("nanosleep", ret, 0);
}
static void pin(u32 cpu)
{
	u64 mask = 1UL << cpu; u32 actual = ~0U;
	eq("setaffinity", sys3(122, 0, sizeof(mask), (long)&mask), 0);
	eq("getcpu", sys3(168, (long)&actual, 0, 0), 0);
	eq("actual_cpu", actual, cpu);
}
static long fd_count(void)
{
	char data[4096] __attribute__((aligned(8))); long count = 0;
	long fd = need("open_fd", sys3(56, -100, (long)"/proc/self/fd", O_DIRECTORY));
	for (;;) {
		long n = need("getdents", sys3(61, fd, (long)data, sizeof(data)));
		if (!n) break;
		for (long p = 0; p < n;) {
			struct linux_dirent64 *d = (void *)(data + p);
			if (n - p < 20 || d->reclen < 20 || d->reclen > n - p) fail("dirent", p);
			if (d->name[0] != '.') count++;
			p += d->reclen;
		}
	}
	close_fd(fd); return count - 1;
}
static void set_reg(long fd, u64 offset, u64 value)
{
	struct one_reg reg = { .id = KVM_REG_CORE | offset, .addr = (u64)&value };
	eq("set_reg", ioctl(fd, KVM_SET_ONE_REG, &reg), 0);
}
static u64 get_reg(long fd, u64 offset)
{
	u64 value = 0;
	struct one_reg reg = { .id = KVM_REG_CORE | offset, .addr = (u64)&value };
	eq("get_reg", ioctl(fd, KVM_GET_ONE_REG, &reg), 0); return value;
}
static void set_mp(long fd, u32 state) { eq("set_mp", ioctl(fd, KVM_SET_MP_STATE, &state), 0); }
static u32 get_mp(long fd)
{
	u32 state = ~0U; eq("get_mp", ioctl(fd, KVM_GET_MP_STATE, &state), 0); return state;
}
static void attr(long fd, u32 group, u64 id, const void *value)
{
	struct device_attr a = { .flags = 0, .group = group, .attr = id, .addr = (u64)value };
	eq("set_attr", ioctl(fd, KVM_SET_DEVICE_ATTR, &a), 0);
}

extern long spawn_vm_task(void *stack, void (*entry)(void *), void *arg);
extern void signal_restorer(void);
__asm__(".text\n.align 2\n.type spawn_vm_task,%function\n"
	"spawn_vm_task:\nsub x0,x0,#16\nstp x1,x2,[x0]\nmov x1,x0\n"
	"mov x0,#273\nmov x2,xzr\nmov x3,xzr\nmov x4,xzr\nmov x8,#220\nsvc #0\n"
	"cbnz x0,1f\nldp x1,x0,[sp],#16\nblr x1\nmov x0,#99\nmov x8,#93\nsvc #0\n"
	"1: ret\n.size spawn_vm_task,.-spawn_vm_task\n"
	".type signal_restorer,%function\nsignal_restorer:\nmov x8,#139\nsvc #0\n"
	".size signal_restorer,.-signal_restorer\n");
static volatile u32 signal_count;
static void handler(int signal) { if (signal == SIGUSR1) __atomic_add_fetch(&signal_count, 1, __ATOMIC_RELAXED); }
static void setup_signals(void)
{
	struct signal_action a = { .handler = handler, .flags = 0x04000000, .restorer = signal_restorer, .mask = 0 };
	eq("sigaction", sys6(134, SIGUSR1, (long)&a, 0, 8, 0, 0), 0);
	u64 mask = 1UL << (SIGCHLD - 1);
	eq("block_sigchld", sys6(135, 0, (long)&mask, 0, 8, 0, 0), 0);
}
static void worker(void *arg)
{
	struct worker_state *s = arg;
	pin(3);
	__atomic_store_n(&s->started, 1, __ATOMIC_RELEASE);
	s->ret = ioctl(s->fd, KVM_RUN, 0);
	s->signal_count = __atomic_load_n(&signal_count, __ATOMIC_RELAXED);
	__atomic_store_n(&s->returned, 1, __ATOMIC_RELEASE);
	sys3(93, 0, 0, 0);
	for (;;) {}
}
struct worker_handle { struct worker_state *s; void *stack; long pid; };
static struct worker_handle start_worker(long fd)
{
	struct worker_handle h = { .s = map(PAGE_SIZE, 0x21, -1), .stack = map(65536, 0x22, -1) };
	h.s->fd = fd; signal_count = 0;
	h.pid = need("clone_vm", spawn_vm_task((u8 *)h.stack + 65536, worker, h.s));
	u64 limit = now() + 1000000000UL;
	while (!__atomic_load_n(&h.s->started, __ATOMIC_ACQUIRE)) {
		if (now() > limit) fail("worker_not_started", h.pid);
		delay(1000000);
	}
	return h;
}
static void stop_worker(struct worker_handle *h)
{
	eq("worker_still_in_run", __atomic_load_n(&h->s->returned, __ATOMIC_ACQUIRE), 0);
	eq("kill_worker", sys3(129, h->pid, SIGUSR1, 0), 0);
	int status = -1;
	eq("wait_worker", sys6(260, h->pid, (long)&status, 0, 0, 0, 0), h->pid);
	eq("worker_exit", status, 0); eq("worker_eintr", h->s->ret, -EINTR);
	eq("worker_signal", h->s->signal_count, 1);
	unmap(h->s, PAGE_SIZE); unmap(h->stack, 65536);
}

static struct fixture create_fixture(long kvm, long run_size)
{
	struct fixture f;
	f.run_size = run_size;
	f.vm = need("create_vm", ioctl(kvm, KVM_CREATE_VM, 0));
	struct create_device d = { .type = 7, .fd = ~0U, .flags = 0 };
	eq("create_vgic", ioctl(f.vm, KVM_CREATE_DEVICE, &d), 0); f.device = d.fd;
	u64 dist = 0x3fff0000, redist = dist - 0x40000; u32 nr_irqs = 64;
	attr(f.device, 0, 2, &dist); attr(f.device, 0, 3, &redist); attr(f.device, 3, 0, &nr_irqs);
	f.ram = map(RAM_SIZE, 0x21, -1);
	/* Fixed snippets avoid modifying guest instructions after their first use. */
	const u32 calls[] = { 0xd4000002, 0xd4000003, 0xd4000023 };
	for (unsigned int i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
		u32 *code = (u32 *)(f.ram + i * 0x20);
		code[0] = calls[i]; code[1] = 0xf9000280; /* str x0,[x20] */
		code[2] = 0x14000000;
	}
	/* ldr/add/str/b forms a non-cooperating guest loop with guest DAIF masked. */
	u32 *busy = (u32 *)(f.ram + 0x200);
	busy[0] = 0xf9400262; busy[1] = 0x91000442; busy[2] = 0xf9000262; busy[3] = 0x17fffffd;
	struct memory_region m = { .slot = 0, .flags = 0, .gpa = RAM, .size = RAM_SIZE, .hva = (u64)f.ram };
	eq("memory", ioctl(f.vm, KVM_SET_USER_MEMORY_REGION, &m), 0);
	struct vcpu_init init;
	eq("preferred_target", ioctl(f.vm, KVM_ARM_PREFERRED_TARGET, &init), 0);
	for (unsigned int i = 0; i < 7; i++) init.features[i] = 0;
	init.features[0] = 1U << 2; /* KVM_ARM_VCPU_PSCI_0_2 */
	for (int i = 0; i < 2; i++) {
		f.vcpu[i] = need("create_vcpu", ioctl(f.vm, KVM_CREATE_VCPU, (void *)(u64)i));
		eq("vcpu_init", ioctl(f.vcpu[i], KVM_ARM_VCPU_INIT, &init), 0);
		f.run[i] = map(run_size, 1, f.vcpu[i]);
		set_reg(f.vcpu[i], 66, 0x3c5); set_reg(f.vcpu[i], 40, MMIO);
	}
	attr(f.device, 4, 0, 0);
	return f;
}
static void prepare_call(struct fixture *f, u32 instruction, u64 function)
{
	u64 offset;
	if (instruction == 0xd4000002) offset = 0;
	else if (instruction == 0xd4000003) offset = 0x20;
	else if (instruction == 0xd4000023) offset = 0x40;
	else fail("invalid_call_instruction", instruction);
	f->call_pc = RAM + offset;
	set_mp(f->vcpu[0], RUNNABLE);
	set_reg(f->vcpu[0], 0, function); set_reg(f->vcpu[0], 64, f->call_pc);
	set_reg(f->vcpu[0], 66, 0x3c5); set_reg(f->vcpu[0], 40, MMIO);
}
static void expect_mmio(struct fixture *f, u64 value)
{
	eq("run_mmio", ioctl(f->vcpu[0], KVM_RUN, 0), 0);
	eq("mmio_reason", *(u32 *)(f->run[0] + 8), MMIO_EXIT);
	eq("mmio_address", *(u64 *)(f->run[0] + 32), MMIO);
	eq("mmio_length", *(u32 *)(f->run[0] + 48), 8);
	eq("mmio_write", f->run[0][52], 1);
	eq("mmio_value", *(u64 *)(f->run[0] + 40), value);
	eq("guest_x0", get_reg(f->vcpu[0], 0), value);
	eq("pc_advanced", get_reg(f->vcpu[0], 64), f->call_pc + 8);
}
static void rejected_calls(struct fixture *f)
{
	prepare_call(f, 0xd4000002, 0x8400001e); expect_mmio(f, ~0UL);
	put("UNKNOWN_PSCI_HVC=PASS\n");
	prepare_call(f, 0xd4000003, 0x8400001e); expect_mmio(f, ~0UL);
	put("UNKNOWN_PSCI_SMC=PASS\n");
	prepare_call(f, 0xd4000003, 0x8200abcd); expect_mmio(f, ~0UL);
	put("UNKNOWN_SMC=PASS\n");
	prepare_call(f, 0xd4000023, 0x84000009); expect_mmio(f, ~0UL); /* smc #1 */
	put("NONZERO_SMC_IMMEDIATE=PASS\n");
	prepare_call(f, 0xd4000002, 0x8200abcd);
	eq("private_hvc_run", ioctl(f->vcpu[0], KVM_RUN, 0), 0);
	eq("private_hvc_reason", *(u32 *)(f->run[0] + 8), UNKNOWN_EXIT);
	eq("private_hvc_not_smc", get_reg(f->vcpu[0], 0), 0x8200abcd);
	eq("private_hvc_pc", get_reg(f->vcpu[0], 64), f->call_pc + 4);
	put("PRIVATE_HVC_SEPARATE=PASS\n");
}
static void already_on(struct fixture *f)
{
	set_mp(f->vcpu[1], RUNNABLE);
	set_reg(f->vcpu[1], 64, RAM + 0x200); set_reg(f->vcpu[1], 0, 0x12345678);
	prepare_call(f, 0xd4000002, 0x84000003);
	set_reg(f->vcpu[0], 2, 1); set_reg(f->vcpu[0], 4, RAM + 0x100); set_reg(f->vcpu[0], 6, 0xdeadbeef);
	expect_mmio(f, (u64)-4L);
	eq("already_on_target_pc", get_reg(f->vcpu[1], 64), RAM + 0x200);
	eq("already_on_target_x0", get_reg(f->vcpu[1], 0), 0x12345678);
	put("CPU_ON_ALREADY_ON=PASS\n");
}
static void system_event(struct fixture *f, int smc, int reset)
{
	prepare_call(f, smc ? 0xd4000003 : 0xd4000002, reset ? 0x84000009 : 0x84000008);
	set_mp(f->vcpu[1], RUNNABLE);
	set_reg(f->vcpu[1], 64, RAM + 0x200); set_reg(f->vcpu[1], 38, RAM + PAGE_SIZE);
	set_reg(f->vcpu[1], 66, 0x3c5);
	volatile u64 *counter = (volatile u64 *)(f->ram + PAGE_SIZE);
	__atomic_store_n(counter, 0, __ATOMIC_RELEASE);
	struct worker_handle h = start_worker(f->vcpu[1]);
	u64 limit = now() + 2000000000UL;
	while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < 1000) {
		if (__atomic_load_n(&h.s->returned, __ATOMIC_ACQUIRE) || now() > limit)
			fail("secondary_no_progress", *counter);
		delay(1000000);
	}
	eq("system_event_run", ioctl(f->vcpu[0], KVM_RUN, 0), 0);
	u8 *run = f->run[0];
	eq("system_event_reason", *(u32 *)(run + 8), SYSTEM_EVENT);
	eq("system_event_type", *(u32 *)(run + 32), reset ? 2 : 1);
	eq("system_event_ndata", *(u32 *)(run + 36), 1);
	eq("system_event_data0", *(u64 *)(run + 40), 0);
	eq("primary_stopped", get_mp(f->vcpu[0]), STOPPED);
	eq("secondary_stopped", get_mp(f->vcpu[1]), STOPPED);
	u64 stopped = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
	delay(20000000);
	eq("secondary_synchronously_stopped", __atomic_load_n(counter, __ATOMIC_ACQUIRE), stopped);
	stop_worker(&h);
	put("SYNCHRONOUS_STOP=PASS\n");
	/* A later KVM_RUN must wait until an explicit state change, not re-enter. */
	h = start_worker(f->vcpu[0]); delay(10000000); stop_worker(&h);
	put("STOPPED_RUN_EINTR=PASS\n");
	/* Explicit userspace resume is allowed, with PSCI INTERNAL_FAILURE left in x0. */
	set_mp(f->vcpu[0], RUNNABLE); expect_mmio(f, (u64)-6L);
	put("EXPLICIT_RESUME_INTERNAL_FAILURE=PASS\n");
	if (smc) put(reset ? "SMC_RESET=PASS\n" : "SMC_OFF=PASS\n");
	else put(reset ? "HVC_RESET=PASS\n" : "HVC_OFF=PASS\n");
}
void _start(void)
{
	long mounted = sys6(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	long console = need("console", sys3(56, -100, (long)"/dev/console", 2));
	for (long fd = 0; fd < 3; fd++) if (fd != console) eq("dup3", sys3(24, console, fd, 0), fd);
	if (console > 2) close_fd(console);
	put("ARM64_PSCI_BEGIN\n");
	if (mounted != 0 && mounted != -16) fail("devtmpfs", mounted);
	eq("proc", sys6(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0), 0);
	pin(2); setup_signals();
	long baseline = fd_count(), kvm = -1;
	for (int i = 0; i < 100 && kvm < 0; i++) {
		kvm = sys3(56, -100, (long)"/dev/kvm", 2);
		if (kvm < 0) delay(10000000);
	}
	need("open_kvm", kvm);
	long run_size = need("run_size", ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0));
	struct fixture f = create_fixture(kvm, run_size);
	rejected_calls(&f); already_on(&f);
	system_event(&f, 0, 0); system_event(&f, 0, 1);
	system_event(&f, 1, 0); system_event(&f, 1, 1);
	for (int i = 0; i < 2; i++) { unmap(f.run[i], run_size); close_fd(f.vcpu[i]); }
	close_fd(f.device); close_fd(f.vm); unmap(f.ram, RAM_SIZE); close_fd(kvm);
	eq("fd_balance", fd_count(), baseline);
	put("FD_BALANCE=PASS\n"); finish(1);
}
