// SPDX-License-Identifier: GPL-2.0
/* Run actual Firecracker; exit 1 is expected for the deliberately unknown exit. */
#if !defined(__aarch64__)
#error "AArch64 only"
#endif
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
#ifndef FC_CPU_MASK
#define FC_CPU_MASK 12UL
#endif
struct timespec64 { long sec, nsec; };
struct linux_dirent64 { u64 ino; long off; u16 reclen; u8 type; char name[]; };
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
static long sys3(long n, long a, long b, long c) { return sys6(n, a, b, c, 0, 0, 0); }
static void put(const char *s)
{
	u64 n = 0; while (s[n]) n++;
	while (n) {
		long r = sys3(64, 1, (long)s, n);
		if (r == -4) continue;
		if (r <= 0) return;
		s += r; n -= r;
	}
}
static void metric(const char *name, long value)
{
	char s[32]; u64 pos = sizeof(s) - 1, n = value < 0 ? -(u64)value : (u64)value;
	s[pos] = 0;
	do { s[--pos] = '0' + n % 10; n /= 10; } while (n);
	if (value < 0) s[--pos] = '-';
	put(name); put("="); put(s + pos); put("\n");
}
__attribute__((noreturn)) static void finish(int good)
{
	put(good ? "FC_STOP_HOST_RESULT=PASS\n" : "FC_STOP_HOST_RESULT=FAIL\n");
	metric("HOST_POWEROFF_ERROR", sys6(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0));
	for (;;) {}
}
__attribute__((noreturn)) static void fail(const char *step, long r)
{
	put("FC_STOP_HOST_FAIL "); metric(step, r); finish(0);
}
static long need(const char *s, long r) { if (r < 0) fail(s, r); return r; }
static void eq(const char *s, long r, long wanted)
{
	if (r != wanted) { metric("EXPECTED", wanted); fail(s, r); }
}
static void close_fd(long fd) { eq("close", sys3(57, fd, 0, 0), 0); }
static u64 now(void)
{
	struct timespec64 ts; eq("clock", sys3(113, 1, (long)&ts, 0), 0);
	return ts.sec * 1000000000UL + ts.nsec;
}
static void delay(u64 ns)
{
	struct timespec64 ts = { .sec = 0, .nsec = ns };
	struct timespec64 remain; long ret;
	do { ret = sys3(101, (long)&ts, (long)&remain, 0); if (ret == -4) ts = remain; } while (ret == -4);
	eq("nanosleep", ret, 0);
}
static long fd_count(void)
{
	char data[4096] __attribute__((aligned(8))); long count = 0;
	long fd = need("open_fds", sys3(56, -100, (long)"/proc/self/fd", 040000));
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
static void bind_firecracker(void)
{
	volatile u64 actual[16], requested[16];
	for (unsigned int i = 0; i < sizeof(actual) / sizeof(actual[0]); i++) { actual[i] = 0; requested[i] = 0; }
	requested[0] = FC_CPU_MASK;
	eq("fc_setaffinity", sys3(122, 0, sizeof(requested), (long)requested), 0);
	long size = need("fc_getaffinity", sys3(123, 0, sizeof(actual), (long)actual));
	if (size <= 0 || size > (long)sizeof(actual)) fail("fc_affinity_size", size);
	eq("fc_affinity_mask", actual[0], FC_CPU_MASK);
	for (unsigned int i = 1; i < sizeof(actual) / sizeof(actual[0]); i++) eq("fc_extra_cpus", actual[i], 0);
	metric("FC_STOP_CPU_MASK", actual[0]); put("FC_STOP_AFFINITY=PASS\n");
}
static void check_kvm(void)
{
	long fd = -1;
	for (int i = 0; i < 200 && fd < 0; i++) { fd = sys3(56, -100, (long)"/dev/kvm", 2); if (fd < 0) delay(10000000); }
	need("open_kvm", fd); eq("kvm_api", sys3(29, fd, 0xae00, 0), 12); close_fd(fd);
}
void _start(void)
{
	long mounted = sys6(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	long console = need("console", sys3(56, -100, (long)"/dev/console", 2));
	for (long fd = 0; fd < 3; fd++) if (fd != console) eq("dup3", sys3(24, console, fd, 0), fd);
	if (console > 2) close_fd(console);
	put("FC_STOP_HOST_BEGIN\n"); if (mounted != 0 && mounted != -16) fail("devtmpfs", mounted);
	eq("mount_proc", sys6(40, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0), 0);
	bind_firecracker(); check_kvm();
	long baseline = fd_count(); u64 began = now();
	long child = need("clone", sys6(220, 17, 0, 0, 0, 0, 0));
	if (!child) {
		char *argv[] = { "/firecracker", "--no-api", "--no-seccomp", "--config-file", "/firecracker.json", 0 };
		char *envp[] = { "PATH=/", "RUST_BACKTRACE=1", 0 };
		metric("FC_STOP_EXEC_ERROR", sys3(221, (long)argv[0], (long)argv, (long)envp));
		sys3(94, 127, 0, 0); for (;;) {}
	}
	int status = -1; long r;
	do { r = sys6(260, child, (long)&status, 0, 0, 0, 0); } while (r == -4);
	eq("wait_child", r, child); eq("intentional_exit_one", status, 1 << 8);
	metric("FC_STOP_LIFECYCLE_NS", now() - began);
	put("FC_STOP_EXPECTED_EXIT_ONE=PASS\n");
	/* wait4 returned naturally after all FC threads exited. Verify host progress. */
	u64 resumed = now(); delay(10000000);
	if (now() <= resumed) fail("host_clock_progress", 0);
	check_kvm(); eq("post_exit_fd_balance", fd_count(), baseline);
	put("FC_STOP_FD_BALANCE=PASS\nFC_STOP_HOST_AFTER_EXIT=PASS\n");
	finish(1);
}
