// SPDX-License-Identifier: GPL-2.0
/* Freestanding AArch64 host/guest init. All constants are Linux ARM64 UAPI. */
#if !defined(__aarch64__)
#error "AArch64 only"
#endif

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
struct timespec64 { long sec, nsec; };

#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define EINTR 4
#ifndef TEST_ROUNDS
#define TEST_ROUNDS 3
#endif
#ifndef GUEST_REBOOT_COMMAND
#define GUEST_REBOOT_COMMAND 0x01234567
#endif
#ifndef FC_CPU_MASK
#define FC_CPU_MASK 0UL
#endif
#ifndef GUEST_VCPUS
#define GUEST_VCPUS 1
#endif

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
	u64 length = 0;
	while (s[length])
		length++;
	while (length) {
		long done = sys3(64, 1, (long)s, length);
		if (done == -EINTR)
			continue;
		if (done <= 0)
			return;
		s += done;
		length -= done;
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
#ifdef GUEST_INIT
	put(success ? "FH_LINUX_GUEST_RESULT=PASS\n" : "FH_LINUX_GUEST_RESULT=FAIL\n");
	sys3(162, 0, 0, 0); /* sync */
	long ret = sys6(142, 0xfee1dead, 672274793, GUEST_REBOOT_COMMAND, 0, 0, 0);
#else
	put(success ? "FH_LINUX_HOST_RESULT=PASS\n" : "FH_LINUX_HOST_RESULT=FAIL\n");
	long ret = sys6(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
#endif
	metric("REBOOT_ERROR", ret);
	sys3(94, 1, 0, 0);
	for (;;) {}
}

__attribute__((noreturn)) static void fail(const char *step, long ret)
{
#ifdef GUEST_INIT
	put("FH_LINUX_GUEST_FAIL step=");
#else
	put("FH_LINUX_HOST_FAIL step=");
#endif
	put(step);
	put(" value=");
	number(ret);
	put("\n");
	finish(0);
}

static long need(const char *step, long ret)
{
	if (ret < 0)
		fail(step, ret);
	return ret;
}

static void zero(const char *step, long ret)
{
	if (ret)
		fail(step, ret);
}

static void close_fd(long fd)
{
	zero("close", sys3(57, fd, 0, 0));
}

static void mount_fs(const char *name, const char *path)
{
	long ret = sys6(40, (long)name, (long)path, (long)name, 0, 0, 0);
	if (ret != 0 && ret != -16)
		fail(path, ret);
}

static void setup_console(void)
{
	/* Linux supplies PID 1 with /dev/console fds; reopen after devtmpfs. */
	long fd = need("console", sys3(56, AT_FDCWD, (long)"/dev/console", O_RDWR));
	for (long target = 0; target < 3; target++)
		if (target != fd && sys3(24, fd, target, 0) != target)
			fail("dup3", target);
	if (fd > 2)
		close_fd(fd);
}

static u64 now_ns(void)
{
	struct timespec64 ts;
	zero("clock_gettime", sys3(113, 1, (long)&ts, 0));
	return ts.sec * 1000000000UL + ts.nsec;
}

#ifdef GUEST_INIT
/* Keep the RW PT_LOAD file-backed for older Linux ELF loaders. */
static u8 data[65536] = { 0x5a };

static void topology_test(void)
{
	volatile u64 allowed[16];
	for (unsigned int i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
		allowed[i] = 0;
	need("guest_getaffinity", sys3(123, 0, sizeof(allowed), (long)allowed));
	if (allowed[0] != (1UL << GUEST_VCPUS) - 1)
		fail("guest_online_cpus", allowed[0]);
	for (unsigned int i = 1; i < sizeof(allowed) / sizeof(allowed[0]); i++)
		if (allowed[i])
			fail("guest_extra_cpu", i);
	/* Run on every guest CPU, including PSCI-started secondary vCPUs. */
	for (unsigned int cpu = 0; cpu < GUEST_VCPUS; cpu++) {
		volatile u64 target[16];
		for (unsigned int i = 0; i < sizeof(target) / sizeof(target[0]); i++)
			target[i] = 0;
		u32 actual = ~0U;
		target[0] = 1UL << cpu;
		zero("guest_setaffinity", sys3(122, 0, sizeof(target), (long)target));
		zero("guest_getcpu", sys3(168, (long)&actual, 0, 0));
		if (actual != cpu)
			fail("guest_cpu_placement", actual);
		struct timespec64 delay = { .sec = 0, .nsec = 1000000 };
		zero("guest_cpu_timer", sys6(115, 1, 0, (long)&delay, 0, 0, 0));
	}
	zero("guest_restore_affinity", sys3(122, 0, sizeof(allowed), (long)allowed));
	metric("FH_LINUX_GUEST_VCPUS", GUEST_VCPUS);
	put("FH_LINUX_TOPOLOGY=PASS\n");
}

static u8 pattern(u64 offset)
{
	return (u8)((offset * 17 + (offset >> 8) + 91) & 255);
}

static void io_test(void)
{
	const u64 size = 4UL * 1024 * 1024;
	long fd = need("create_io_file", sys6(56, AT_FDCWD, (long)"/io-test.dat",
					O_RDWR | O_CREAT | O_TRUNC, 0600, 0, 0));
	for (u64 offset = 0; offset < size; offset += sizeof(data)) {
		for (u64 i = 0; i < sizeof(data); i++)
			data[i] = pattern(offset + i);
		for (u64 done = 0; done < sizeof(data);) {
			long count = sys3(64, fd, (long)(data + done), sizeof(data) - done);
			if (count == -EINTR)
				continue;
			if (count <= 0)
				fail("write_io_file", count);
			done += count;
		}
	}
	zero("fsync_io_file", sys3(82, fd, 0, 0));
	close_fd(fd);
	fd = need("reopen_io_file", sys3(56, AT_FDCWD, (long)"/io-test.dat", 0));
	u64 checksum = 0;
	for (u64 offset = 0; offset < size; offset += sizeof(data)) {
		for (u64 done = 0; done < sizeof(data);) {
			long count = sys3(63, fd, (long)(data + done), sizeof(data) - done);
			if (count == -EINTR)
				continue;
			if (count <= 0)
				fail("read_io_file", count);
			done += count;
		}
		for (u64 i = 0; i < sizeof(data); i++) {
			if (data[i] != pattern(offset + i))
				fail("io_data_mismatch", offset + i);
			checksum += data[i];
		}
	}
	if (sys3(63, fd, (long)data, 1) != 0)
		fail("io_file_size", size);
	close_fd(fd);
	metric("FH_LINUX_IO_BYTES", size);
	metric("FH_LINUX_IO_CHECKSUM", checksum);
	put("FH_LINUX_IO=PASS\n");
}

static void guest_main(void)
{
	put("FH_LINUX_GUEST_BEGIN\n");
	topology_test();
	volatile u32 state = 0x12345678;
	for (u32 i = 0; i < 500000; i++)
		state = state * 1664525U + 1013904223U;
	if (state != 298169368U)
		fail("cpu_checksum", state);
	put("FH_LINUX_CPU=PASS\n");
	u64 start = now_ns();
	for (int i = 0; i < 50; i++) {
		struct timespec64 delay = { .sec = 0, .nsec = 1000000 };
		struct timespec64 remain;
		long ret;
		do {
			ret = sys6(115, 1, 0, (long)&delay, (long)&remain, 0, 0);
			if (ret == -EINTR)
				delay = remain;
		} while (ret == -EINTR);
		zero("clock_nanosleep", ret);
	}
	u64 elapsed = now_ns() - start;
	metric("FH_LINUX_TIMER_ELAPSED_NS", elapsed);
	if (elapsed < 50000000UL)
		fail("timer_elapsed_too_short", elapsed);
	put("FH_LINUX_TIMER=PASS\n");
	io_test();
	finish(1);
}
#else
static void bind_firecracker(void)
{
	/* Pin before exec; all Firecracker threads inherit this allowed mask. */
	volatile u64 requested[16];
	volatile u64 actual[16];
	for (unsigned int i = 0; i < sizeof(actual) / sizeof(actual[0]); i++) {
		requested[i] = 0;
		actual[i] = 0;
	}
	requested[0] = FC_CPU_MASK;
	if (FC_CPU_MASK)
		zero("fc_sched_setaffinity", sys3(122, 0, sizeof(requested), (long)requested));
	long size = need("fc_sched_getaffinity", sys3(123, 0, sizeof(actual), (long)actual));
	if (!size || size > (long)sizeof(actual) || !actual[0])
		fail("fc_affinity_size", size);
	for (unsigned int i = 1; i < sizeof(actual) / sizeof(actual[0]); i++)
		if (actual[i])
			fail("fc_affinity_out_of_range", i);
	if (FC_CPU_MASK && actual[0] != FC_CPU_MASK)
		fail("fc_affinity_mismatch", actual[0]);
	metric("FH_FC_CPU_MASK", actual[0]);
	put("FH_FC_AFFINITY=PASS\n");
}

static void check_kvm(void)
{
	long kvm = -1;
	for (int attempt = 0; attempt < 200; attempt++) {
		kvm = sys3(56, AT_FDCWD, (long)"/dev/kvm", O_RDWR);
		if (kvm >= 0)
			break;
		struct timespec64 delay = { .sec = 0, .nsec = 50000000 };
		sys3(101, (long)&delay, 0, 0);
	}
	need("open_kvm", kvm);
	if (sys3(29, kvm, 0xae00, 0) != 12)
		fail("api_version", -1);
	close_fd(kvm);
}

static void host_main(void)
{
	put("FH_LINUX_HOST_BEGIN\n");
	for (int round = 1; round <= TEST_ROUNDS; round++) {
		metric("FH_LINUX_ROUND_BEGIN", round);
		u64 start = now_ns();
		long child = need("clone", sys6(220, 17, 0, 0, 0, 0, 0));
		if (!child) {
			bind_firecracker();
			check_kvm();
			char *argv[] = { "/firecracker", "--no-api", "--no-seccomp",
				"--config-file", "/firecracker.json", 0 };
			char *envp[] = { "PATH=/", "RUST_BACKTRACE=1", 0 };
			long ret = sys3(221, (long)argv[0], (long)argv, (long)envp);
			metric("FIRECRACKER_EXEC_ERROR", ret);
			sys3(94, 127, 0, 0);
			for (;;) {}
		}
		int status = 0;
		long ret;
		do {
			ret = sys6(260, child, (long)&status, 0, 0, 0, 0);
		} while (ret == -EINTR);
		if (ret != child || status)
			fail("firecracker_wait_status", status ? status : ret);
		metric("FH_LINUX_ROUND_NS", now_ns() - start);
		metric("FH_LINUX_ROUND_PASS", round);
	}
	finish(1);
}
#endif

void _start(void)
{
	mount_fs("devtmpfs", "/dev");
	setup_console();
	mount_fs("proc", "/proc");
	mount_fs("sysfs", "/sys");
#ifdef GUEST_INIT
	guest_main();
#else
	host_main();
#endif
}
