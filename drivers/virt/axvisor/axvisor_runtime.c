// SPDX-License-Identifier: GPL-2.0
#include <linux/printk.h>
#include <linux/timekeeping.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memory_hotplug.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/of_fdt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/ioport.h>
#include <linux/reboot.h>
#include <linux/eventfd.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#ifdef CONFIG_AXVISOR_LINUX_CONTROL
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/sched/signal.h>
#endif
#ifdef CONFIG_RISCV
#include <asm/pgtable.h>
#endif
#ifdef CONFIG_X86
#include <asm/tsc.h>
#endif

#include "axvisor_ffi.h"

#ifdef CONFIG_X86
/* Bind the legacy IOAPIC GSI used by a passthrough PCI device to a Linux
 * descriptor.  The outer QEMU host uses IRQ 20/23, while the guest PCI
 * functions are wired to guest GSI 10/11. */
static irqreturn_t axvisor_linux_passthrough_irq(int irq, void *dev_id)
{
	unsigned long vector = *(unsigned long *)dev_id;
	(void)irq;
	axvisor_linux_handle_registered_irq(vector);
	return IRQ_HANDLED;
}

bool axvisor_linux_prepare_irq_vector(unsigned long vector)
{
	static unsigned long dev_ids[256];
	unsigned int irq;
	int ret;

	/* Map the guest's legacy virtio vectors to the real outer-host IRQ lines.
	 * The VMX exit vector is the host vector (0x34/0x37); Linux invokes this
	 * callback on IRQ 20/23 and the callback injects guest vector 0x2a/0x2b. */
	switch (vector) {
	case 0x2a:
		irq = 20;
		break;
	case 0x2b:
		irq = 23;
		break;
	default:
		return false;
	}
	if (dev_ids[irq] != 0)
		return true;
	dev_ids[irq] = vector;
	ret = request_irq(irq, axvisor_linux_passthrough_irq,
			  IRQF_SHARED, "axvisor-ioapic", &dev_ids[irq]);
	if (ret) {
		dev_ids[irq] = 0;
		return false;
	}
	return true;
}
#endif

#ifdef CONFIG_AXVISOR_LINUX_CONTROL
extern u64 axvisor_linux_control_open(void);
extern int axvisor_linux_control_close(u64 control_file);

struct axvisor_linux_control_mmap {
	void *addr;
	size_t len;
};

struct axvisor_linux_control_anon {
	u64 control_file;
	struct axvisor_linux_control_mmap *mmap;
};

struct axvisor_linux_control_fd_ref {
	struct file *file;
#ifdef CONFIG_EVENTFD
	struct eventfd_ctx *eventfd;
#endif
};
extern long axvisor_linux_control_ioctl(u64 control_file, u32 cmd,
						unsigned long arg);

static int axvisor_linux_control_open_file(struct inode *inode,
						  struct file *file)
{
	u64 control_file = axvisor_linux_control_open();

	(void)inode;
	if (!control_file)
		return -ENODEV;
	file->private_data = (void *)(uintptr_t)control_file;
	return 0;
}

static int axvisor_linux_control_release_file(struct inode *inode,
							      struct file *file)
{
	u64 control_file = (u64)(uintptr_t)file->private_data;

	(void)inode;
	return axvisor_linux_control_close(control_file) ? 0 : -EIO;
}

static long axvisor_linux_control_ioctl_file(struct file *file,
						     unsigned int cmd,
						     unsigned long arg)
{
	u64 control_file = (u64)(uintptr_t)file->private_data;

	return axvisor_linux_control_ioctl(control_file, cmd, arg);
}

static long axvisor_linux_control_anon_ioctl_file(struct file *file,
							  unsigned int cmd,
							  unsigned long arg)
{
	struct axvisor_linux_control_anon *anon = file->private_data;

	if (!anon)
		return -EBADF;
	return axvisor_linux_control_ioctl(anon->control_file, cmd, arg);
}

static int axvisor_linux_control_release_anon(struct inode *inode,
							      struct file *file)
{
	struct axvisor_linux_control_anon *anon = file->private_data;

	(void)inode;
	if (!anon)
		return -EINVAL;
	if (axvisor_linux_control_close(anon->control_file)) {
		kfree(anon);
		return 0;
	}
	kfree(anon);
	return -EIO;
}

static int axvisor_linux_control_mmap_file(struct file *file,
						   struct vm_area_struct *vma)
{
	struct axvisor_linux_control_anon *anon = file->private_data;

	if (!anon || !anon->mmap || !anon->mmap->addr || vma->vm_pgoff != 0 ||
		(vma->vm_end - vma->vm_start) > PAGE_ALIGN(anon->mmap->len))
		return -EINVAL;
	return remap_vmalloc_range(vma, anon->mmap->addr, 0);
}

static const struct file_operations axvisor_linux_control_anon_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = axvisor_linux_control_anon_ioctl_file,
	.release = axvisor_linux_control_release_anon,
	.mmap = axvisor_linux_control_mmap_file,
#ifdef CONFIG_COMPAT
	.compat_ioctl = axvisor_linux_control_anon_ioctl_file,
#endif
};

static const struct file_operations axvisor_linux_control_fops = {
	.owner = THIS_MODULE,
	.open = axvisor_linux_control_open_file,
	.release = axvisor_linux_control_release_file,
	.unlocked_ioctl = axvisor_linux_control_ioctl_file,
#ifdef CONFIG_COMPAT
	.compat_ioctl = axvisor_linux_control_ioctl_file,
#endif
};

static struct miscdevice axvisor_linux_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "kvm",
	.fops = &axvisor_linux_control_fops,
};

int axvisor_linux_control_register_endpoint(void)
{
	return misc_register(&axvisor_linux_control_device);
}

int axvisor_linux_control_copy_from_user(void *dst, const void __user *src,
						 size_t length)
{
	/* Some control operations copy a small value while holding an AxVisor
	 * SpinNoIrq lock.  Linux usercopy may fault and sleep, so use its
	 * non-faulting variant while that lock has local IRQs disabled. */
	if (irqs_disabled() || in_atomic())
		return copy_from_user_nofault(dst, src, length) ? -EFAULT : 0;
	return copy_from_user(dst, src, length) ? -EFAULT : 0;
}

int axvisor_linux_control_copy_to_user(void __user *dst, const void *src,
						 size_t length)
{
	if (irqs_disabled() || in_atomic())
		return copy_to_user_nofault(dst, src, length) ? -EFAULT : 0;
	return copy_to_user(dst, src, length) ? -EFAULT : 0;
}

u64 axvisor_linux_control_create_mmap_area(size_t length)
{
	struct axvisor_linux_control_mmap *mmap;

	if (!length || length > SIZE_MAX - PAGE_SIZE)
		return 0;
	mmap = kzalloc(sizeof(*mmap), GFP_KERNEL);
	if (!mmap)
		return 0;
	mmap->len = PAGE_ALIGN(length);
	mmap->addr = vmalloc_user(mmap->len);
	if (!mmap->addr) {
		kfree(mmap);
		return 0;
	}
	return (u64)(uintptr_t)mmap;
}

static int axvisor_linux_control_mmap_bounds(
		struct axvisor_linux_control_mmap *mmap, size_t offset, size_t length)
{
	if (!mmap || offset > mmap->len || length > mmap->len - offset)
		return -EINVAL;
	return 0;
}

int axvisor_linux_control_read_mmap_area(u64 area, size_t offset, void *buf,
						 size_t length)
{
	struct axvisor_linux_control_mmap *mmap =
		(struct axvisor_linux_control_mmap *)(uintptr_t)area;

	if (!buf || axvisor_linux_control_mmap_bounds(mmap, offset, length))
		return -EINVAL;
	memcpy(buf, mmap->addr + offset, length);
	return 0;
}

int axvisor_linux_control_write_mmap_area(u64 area, size_t offset,
						  const void *buf, size_t length)
{
	struct axvisor_linux_control_mmap *mmap =
		(struct axvisor_linux_control_mmap *)(uintptr_t)area;

	if (!buf || axvisor_linux_control_mmap_bounds(mmap, offset, length))
		return -EINVAL;
	memcpy(mmap->addr + offset, buf, length);
	return 0;
}

int axvisor_linux_control_release_mmap_area(u64 area)
{
	struct axvisor_linux_control_mmap *mmap =
		(struct axvisor_linux_control_mmap *)(uintptr_t)area;

	if (!mmap)
		return -EINVAL;
	/* AxVisor can release the final vCPU mmap while holding a SpinNoIrq
	 * control-state lock.  Linux provides vfree_atomic() specifically for
	 * deferred vmalloc teardown from such contexts. */
	if (irqs_disabled() || in_atomic())
		vfree_atomic(mmap->addr);
	else
		vfree(mmap->addr);
	kfree(mmap);
	return 0;
}

u64 axvisor_linux_control_get_fd_ref(int fd)
{
	struct axvisor_linux_control_fd_ref *ref;

	if (fd < 0)
		return 0;
	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return 0;
	ref->file = fget(fd);
	if (!ref->file) {
		kfree(ref);
		return 0;
	}
#ifdef CONFIG_EVENTFD
	ref->eventfd = eventfd_ctx_fileget(ref->file);
	if (IS_ERR(ref->eventfd))
		ref->eventfd = NULL;
#endif
	return (u64)(uintptr_t)ref;
}

ssize_t axvisor_linux_control_write_fd_ref(u64 id, const void *buf,
						   size_t length)
{
	struct axvisor_linux_control_fd_ref *ref =
		(struct axvisor_linux_control_fd_ref *)(uintptr_t)id;
	loff_t pos = 0;

	if (!ref || !ref->file || !buf)
		return -EBADF;
#ifdef CONFIG_EVENTFD
	if (ref->eventfd) {
		u64 value;

		if (length != sizeof(value))
			return -EINVAL;
		memcpy(&value, buf, sizeof(value));
		/* AxVisor uses this operation to signal irqfd/ioeventfd.  The
		 * in-kernel eventfd API signals one event without going through the
		 * userspace-only ->write handler. */
		if (value != 1 || !eventfd_signal_allowed())
			return -EINVAL;
		eventfd_signal(ref->eventfd);
		return sizeof(value);
	}
#endif
	return kernel_write(ref->file, buf, length, &pos);
}

ssize_t axvisor_linux_control_read_fd_ref(u64 id, void *buf, size_t length)
{
	struct axvisor_linux_control_fd_ref *ref =
		(struct axvisor_linux_control_fd_ref *)(uintptr_t)id;
	loff_t pos = 0;

	if (!ref || !ref->file || !buf)
		return -EBADF;
	return kernel_read(ref->file, buf, length, &pos);
}

int axvisor_linux_control_release_fd_ref(u64 id)
{
	struct axvisor_linux_control_fd_ref *ref =
		(struct axvisor_linux_control_fd_ref *)(uintptr_t)id;

	if (!ref)
		return -EINVAL;
#ifdef CONFIG_EVENTFD
	if (ref->eventfd)
		eventfd_ctx_put(ref->eventfd);
#endif
	fput(ref->file);
	kfree(ref);
	return 0;
}

u64 axvisor_linux_control_retain_user_address_space(void)
{
	return (u64)(uintptr_t)get_task_mm(current);
}

int axvisor_linux_control_release_user_address_space(u64 id)
{
	struct mm_struct *mm = (struct mm_struct *)(uintptr_t)id;

	if (!mm)
		return -EINVAL;
	mmput(mm);
	return 0;
}

struct axvisor_linux_control_pinned_pages {
	struct page **pages;
	unsigned long count;
	bool uses_gup_pin;
};

u64 axvisor_linux_control_pin_user_pages(u64 mm_id, unsigned long addr,
						  size_t length, bool writable,
						  u64 *phys_pages, size_t max_pages,
						  size_t *page_count)
{
	struct mm_struct *mm = (struct mm_struct *)(uintptr_t)mm_id;
	struct axvisor_linux_control_pinned_pages *pinned;
	unsigned long first, last, npages, i;
	struct vm_area_struct *vma;
	long ret;
	int locked = 1;
	unsigned int flags = writable ? FOLL_WRITE : 0;

	if (!mm || !length || addr > ULONG_MAX - length)
		return 0;
	first = addr & PAGE_MASK;
	last = PAGE_ALIGN(addr + length);
	if (last < first)
		return 0;
	npages = (last - first) >> PAGE_SHIFT;
	if (!npages || npages > max_pages)
		return 0;
	pinned = kzalloc(sizeof(*pinned), GFP_KERNEL);
	if (!pinned)
		return 0;
	pinned->pages = kcalloc(npages, sizeof(*pinned->pages), GFP_KERNEL);
	if (!pinned->pages) {
		kfree(pinned);
		return 0;
	}
	mmap_read_lock(mm);
	vma = find_vma(mm, first);
	if (npages == 1 && vma && first >= vma->vm_start &&
	    (vma->vm_flags & (VM_IO | VM_PFNMAP))) {
		struct follow_pfnmap_args args = {
			.vma = vma,
			.address = first,
		};
		struct page *page;

		ret = follow_pfnmap_start(&args);
		if (!ret && (!writable || args.writable)) {
			page = pfn_to_online_page(args.pfn);
			if (page) {
				get_page(page);
				pinned->pages[0] = page;
				pinned->count = 1;
				pinned->uses_gup_pin = false;
				phys_pages[0] = PFN_PHYS(args.pfn);
				*page_count = 1;
				follow_pfnmap_end(&args);
				mmap_read_unlock(mm);
				return (u64)(uintptr_t)pinned;
			}
		}
		if (!ret)
			follow_pfnmap_end(&args);
	}
	ret = pin_user_pages_remote(mm, first, npages, flags, pinned->pages, &locked);
	if (locked)
		mmap_read_unlock(mm);
	if (ret != npages) {
		if (ret > 0)
			unpin_user_pages(pinned->pages, ret);
		kfree(pinned->pages);
		kfree(pinned);
		return 0;
	}
	pinned->count = npages;
	pinned->uses_gup_pin = true;
	for (i = 0; i < npages; i++)
		phys_pages[i] = page_to_phys(pinned->pages[i]);
	*page_count = npages;
	return (u64)(uintptr_t)pinned;
}

int axvisor_linux_control_release_pinned_pages(u64 id)
{
	struct axvisor_linux_control_pinned_pages *pinned =
		(struct axvisor_linux_control_pinned_pages *)(uintptr_t)id;

	if (!pinned)
		return -EINVAL;
	if (pinned->uses_gup_pin) {
		unpin_user_pages(pinned->pages, pinned->count);
	} else {
		unsigned long i;

		for (i = 0; i < pinned->count; i++)
			put_page(pinned->pages[i]);
	}
	kfree(pinned->pages);
	kfree(pinned);
	return 0;
}

int axvisor_linux_control_pending_signal(const u8 *blocked_bytes, size_t length)
{
	sigset_t blocked, pending;
	unsigned long flags;

	if (length > sizeof(blocked))
		return -EINVAL;
	if (length) {
		memset(&blocked, 0, sizeof(blocked));
		memcpy(&blocked, blocked_bytes, length);
	} else {
		blocked = current->blocked;
	}
	spin_lock_irqsave(&current->sighand->siglock, flags);
	sigorsets(&pending, &current->pending.signal,
		  &current->signal->shared_pending.signal);
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	sigandnsets(&pending, &pending, &blocked);
	return sigisemptyset(&pending) ? 0 : 1;
}

int axvisor_linux_control_create_fd(u64 control_file, u64 mmap_area)
{
	struct axvisor_linux_control_anon *anon;

	if (!control_file)
		return -EINVAL;
	anon = kzalloc(sizeof(*anon), GFP_KERNEL);
	if (!anon)
		return -ENOMEM;
	anon->control_file = control_file;
	anon->mmap = (struct axvisor_linux_control_mmap *)(uintptr_t)mmap_area;
	{
		int fd = anon_inode_getfd("axvisor-kvm", &axvisor_linux_control_anon_fops,
					 anon, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			kfree(anon);
		return fd;
	}
}
#endif

/* Names expected by zerocopy, mapped to Linux's linker symbols. */
asm(".globl _ex_table_start\n_ex_table_start = __start___ex_table\n"
    ".globl _ex_table_end\n_ex_table_end = __stop___ex_table\n");

/* ax-percpu custom-base integration.  Rust emits one initialized template
 * section; Linux allocates a writable copy for every possible CPU and the
 * ax-percpu backend selects a copy through _percpu_base_ptr(). */
extern char _percpu_load_start[];
extern char _percpu_load_end[];
static void *axvisor_percpu_area;
static size_t axvisor_percpu_stride;

int axvisor_linux_percpu_prepare(void)
{
	size_t template_size = (size_t)(_percpu_load_end - _percpu_load_start);
	size_t total_size;
	unsigned int cpu;

	if (!template_size || !nr_cpu_ids)
		return -EINVAL;
	axvisor_percpu_stride = ALIGN(template_size, 64);
	if (axvisor_percpu_stride > SIZE_MAX / nr_cpu_ids)
		return -EOVERFLOW;
	total_size = axvisor_percpu_stride * nr_cpu_ids;
	axvisor_percpu_area = vzalloc(total_size);
	if (!axvisor_percpu_area)
		return -ENOMEM;
	for_each_possible_cpu(cpu)
		memcpy((char *)axvisor_percpu_area + cpu * axvisor_percpu_stride,
		       _percpu_load_start, template_size);
	return 0;
}

void *_percpu_base_ptr(unsigned long cpu)
{
	if (!axvisor_percpu_area || cpu >= nr_cpu_ids)
		return NULL;
	return (char *)axvisor_percpu_area + cpu * axvisor_percpu_stride;
}

/* ax-percpu's external-base backend resolves the current area through this
 * callback rather than changing the host's thread-pointer register. */
unsigned long ax_percpu_current_base(void)
{
	return (unsigned long)_percpu_base_ptr(smp_processor_id());
}

void axvisor_linux_log_message(const u8 *message, size_t length)
{
	if (!message)
		return;

	printk(KERN_INFO "%.*s", (int)length, (const char *)message);
}

/* Keep the Linux timer adapter per-CPU, matching axvisor_core's per-CPU
 * TIMER_LIST.  A global hrtimer cannot safely service vCPU tasks running on
 * more than one host CPU. */
/* Report every online Linux CPU so VMX is enabled before control threads can
 * be scheduled there. */
size_t axvisor_linux_host_get_cpu_num(void) { return num_online_cpus(); }
size_t axvisor_linux_host_current_cpu(void) { return smp_processor_id(); }
static DEFINE_PER_CPU(struct hrtimer, axvisor_host_timer);
static DEFINE_PER_CPU(bool, axvisor_host_timer_initialized);

extern void axvisor_linux_timer_interrupt(void);

static void axvisor_linux_timer_work(struct work_struct *work)
{
	(void)work;
	axvisor_linux_timer_interrupt();
}

static DECLARE_WORK(axvisor_host_timer_work, axvisor_linux_timer_work);

static enum hrtimer_restart axvisor_linux_timer_callback(struct hrtimer *timer)
{
	(void)timer;
	schedule_work(&axvisor_host_timer_work);
	return HRTIMER_NORESTART;
}

void axvisor_linux_host_init_percpu(void)
{
	struct hrtimer *timer;
	bool *initialized;

	/* The caller may be a preemptible kthread; pin this short operation to the
	 * CPU whose timer will be armed below. */
	preempt_disable();
	timer = this_cpu_ptr(&axvisor_host_timer);
	initialized = this_cpu_ptr(&axvisor_host_timer_initialized);
	if (!*initialized) {
		hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		timer->function = axvisor_linux_timer_callback;
		*initialized = true;
	}
	preempt_enable();
}

void axvisor_linux_console_write_bytes(const u8 *bytes, size_t length)
{
	if (bytes)
		printk(KERN_INFO "%.*s", (int)length, (const char *)bytes);
}

/* HostIf::exit terminates the monitor. In the Linux host this powers off the
 * machine; guest shutdown is a host-wide terminal operation. */
void axvisor_linux_host_exit(int code)
{
	(void)code;
	kernel_power_off();
	for (;;)
		cpu_relax();
}

/* ConsoleIf::read_bytes is deliberately non-blocking. Linux has no generic
 * way for a kernel component to pull bytes from whichever tty is the active
 * console, so keep an adapter-owned queue that input glue can feed through
 * axvisor_linux_console_enqueue_bytes(). */
#define AXVISOR_CONSOLE_INPUT_CAPACITY 4096
static u8 axvisor_console_input[AXVISOR_CONSOLE_INPUT_CAPACITY];
static size_t axvisor_console_input_head;
static size_t axvisor_console_input_tail;
static size_t axvisor_console_input_count;
static DEFINE_SPINLOCK(axvisor_console_input_lock);

void axvisor_linux_console_enqueue_bytes(const u8 *bytes, size_t length)
{
	unsigned long flags;
	size_t i;

	if (!bytes || !length)
		return;

	spin_lock_irqsave(&axvisor_console_input_lock, flags);
	for (i = 0; i < length && axvisor_console_input_count <
			AXVISOR_CONSOLE_INPUT_CAPACITY; i++) {
		axvisor_console_input[axvisor_console_input_tail] = bytes[i];
		axvisor_console_input_tail =
			(axvisor_console_input_tail + 1) % AXVISOR_CONSOLE_INPUT_CAPACITY;
		axvisor_console_input_count++;
	}
	spin_unlock_irqrestore(&axvisor_console_input_lock, flags);
}

size_t axvisor_linux_console_read_bytes(u8 *bytes, size_t length)
{
	unsigned long flags;
	size_t count = 0;

	if (!bytes || !length)
		return 0;

	spin_lock_irqsave(&axvisor_console_input_lock, flags);
	while (count < length && axvisor_console_input_count) {
		bytes[count++] = axvisor_console_input[axvisor_console_input_head];
		axvisor_console_input_head =
			(axvisor_console_input_head + 1) % AXVISOR_CONSOLE_INPUT_CAPACITY;
		axvisor_console_input_count--;
	}
	spin_unlock_irqrestore(&axvisor_console_input_lock, flags);
	return count;
}

static ssize_t axvisor_linux_console_input_write(struct file *file,
					 const char __user *buffer, size_t length,
					 loff_t *offset)
{
	u8 bytes[256];
	size_t written = 0;

	(void)file;
	(void)offset;
	while (written < length) {
		size_t chunk = min(length - written, sizeof(bytes));

		if (copy_from_user(bytes, buffer + written, chunk))
			return written ? written : -EFAULT;
		axvisor_linux_console_enqueue_bytes(bytes, chunk);
		written += chunk;
	}
	return written;
}

static const struct file_operations axvisor_linux_console_input_fops = {
	.owner = THIS_MODULE,
	.write = axvisor_linux_console_input_write,
};

static struct miscdevice axvisor_linux_console_input_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "axvisor-console-input",
	.fops = &axvisor_linux_console_input_fops,
	.mode = 0200,
};

int axvisor_linux_console_register_endpoint(void)
{
	return misc_register(&axvisor_linux_console_input_device);
}

u64 axvisor_linux_time_current_time_nanos(void)
{
	return ktime_get_mono_fast_ns();
}

void axvisor_linux_time_set_oneshot_timer(u64 deadline_nanos)
{
	u64 now = ktime_get_mono_fast_ns();
	u64 delay_nanos = deadline_nanos > now ? deadline_nanos - now : 1;
	ktime_t expires = ktime_add_ns(ktime_get(), delay_nanos);
	struct hrtimer *timer;
	bool *initialized;

	preempt_disable();
	timer = this_cpu_ptr(&axvisor_host_timer);
	initialized = this_cpu_ptr(&axvisor_host_timer_initialized);
	if (!*initialized) {
		hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		timer->function = axvisor_linux_timer_callback;
		*initialized = true;
	}
	/* Guest exits can occur much faster than the vCPU timeslice.  Do not let
	 * each exit postpone an already earlier host deadline indefinitely. */
	if (!hrtimer_active(timer) ||
	    ktime_before(expires, hrtimer_get_expires(timer)))
		hrtimer_start(timer, expires, HRTIMER_MODE_ABS);
	preempt_enable();
}
static unsigned long axvisor_host_fdt_paddr_saved;

void axvisor_linux_arch_capture_host_fdt(void)
{
#ifdef CONFIG_RISCV
	axvisor_host_fdt_paddr_saved = initial_boot_params ? dtb_early_pa : 0;
#else
	axvisor_host_fdt_paddr_saved = 0;
#endif
	pr_info("axvisor-linux: host FDT physical address %#lx\n",
		axvisor_host_fdt_paddr_saved);
}

unsigned long axvisor_linux_arch_host_fdt_paddr(void)
{
	/* `initial_boot_params` is backed by the RISC-V early fixmap, so
	 * virt_to_phys() is not valid here. The architecture keeps the original
	 * physical address in dtb_early_pa. */
	return axvisor_host_fdt_paddr_saved;
}
void axvisor_linux_arch_remote_hfence_vvma_all(void) { }

unsigned int axvisor_linux_arch_host_tsc_frequency_mhz(void)
{
#ifdef CONFIG_X86
	return tsc_khz / 1000;
#else
	return 0;
#endif
}

void *axvisor_linux_alloc(size_t size, size_t align)
{
	gfp_t flags = irqs_disabled() ? GFP_ATOMIC : GFP_KERNEL;
	/* kmalloc alignment is at least ARCH_KMALLOC_MINALIGN; over-aligned
	 * allocations are conservatively rounded up to the requested boundary. */
	if (!size)
		size = 1;
	if (align > 1)
		size = ALIGN(size, align);
	return kmalloc(size, flags);
}

void axvisor_linux_dealloc(void *ptr)
{
	kfree(ptr);
}

struct axvisor_io_map {
	unsigned long phys;
	unsigned long size;
	void __iomem *virt;
};

static struct axvisor_io_map axvisor_io_maps[64];
static unsigned int axvisor_io_map_count;
static DEFINE_SPINLOCK(axvisor_io_maps_lock);

void axvisor_linux_task_yield(void)
{
	yield();
}

/* The IRQ registry is consulted from chained interrupt handlers. Pairing the
 * Rust-side lock with local_irq_save prevents a process-context registration
 * from being interrupted on the same CPU and deadlocking in handle_irq. */
unsigned long axvisor_linux_irq_local_save(void)
{
	unsigned long flags;

	local_irq_save(flags);
	return flags;
}

void axvisor_linux_irq_local_restore(unsigned long flags)
{
	local_irq_restore(flags);
}

struct axvisor_task_start {
	int (*entry)(void *data);
	void *data;
	struct completion done;
};
static atomic_t axvisor_task_affinity_cursor = ATOMIC_INIT(-1);

static int axvisor_linux_task_main(void *arg)
{
	struct axvisor_task_start *start = arg;
	start->entry(start->data);
	complete(&start->done);
	return 0;
}

unsigned long axvisor_linux_current_task(void)
{
	/* task_struct has stable identity across CPU migration and also covers
	 * ordinary userspace callers of the control interface. */
	return (unsigned long)current;
}

unsigned long axvisor_linux_spawn_task(int (*entry)(void *), void *data,
					       const u8 *name, unsigned long cpu_set)
{
	struct axvisor_task_start *start;
	struct task_struct *task;
	if (!entry)
		return 0;
	start = kmalloc(sizeof(*start), GFP_KERNEL);
	if (!start)
		return 0;
	start->entry = entry;
	start->data = data;
	init_completion(&start->done);
	/* The Rust side supplies a NUL-terminated task name.  Use a fixed format
	 * string so task names cannot be interpreted as printf directives. */
	task = kthread_create(axvisor_linux_task_main, start, "%s",
			      name && *name ? (const char *)name : "axvisor");
	if (IS_ERR(task)) {
		kfree(start);
		return 0;
	}
	if (cpu_set) {
		cpumask_t mask;
		unsigned int cpu, selected_cpu = nr_cpu_ids;
		unsigned int selected_index;

		cpumask_clear(&mask);
		for_each_online_cpu(cpu)
			if (cpu < BITS_PER_LONG && (cpu_set & BIT(cpu)))
				cpumask_set_cpu(cpu, &mask);
		if (!cpumask_empty(&mask)) {
			/* Keep each virtualization task on one CPU so its host per-CPU
			 * state remains stable, distributing tasks round-robin within
			 * the requested allowed set. */
			selected_index = (unsigned int)atomic_inc_return(
				&axvisor_task_affinity_cursor) % cpumask_weight(&mask);
			for_each_cpu(cpu, &mask) {
				if (selected_index-- == 0) {
					selected_cpu = cpu;
					break;
				}
			}
			cpumask_clear(&mask);
			cpumask_set_cpu(selected_cpu, &mask);
			set_cpus_allowed_ptr(task, &mask);
		}
	}
	wake_up_process(task);
	return (unsigned long)start;
}

void axvisor_linux_join_task(unsigned long handle)
{
	struct axvisor_task_start *start = (void *)handle;
	if (!start)
		return;
	wait_for_completion(&start->done);
	kfree(start);
}

unsigned long axvisor_linux_memory_alloc_frame(void)
{
	gfp_t flags = (irqs_disabled() || in_atomic()) ? GFP_ATOMIC : GFP_KERNEL;
	void *p = (void *)__get_free_page(flags | __GFP_ZERO);
	return p ? virt_to_phys(p) : 0;
}

unsigned long axvisor_linux_memory_alloc_contiguous(size_t num_frames, size_t align)
{
	void *p;
	gfp_t gfp = (irqs_disabled() || in_atomic()) ? GFP_ATOMIC : GFP_KERNEL;
	if (!num_frames || num_frames > (SIZE_MAX >> PAGE_SHIFT))
		return 0;
	/* The buddy allocator cannot satisfy guest regions larger than
	 * MAX_ORDER_NR_PAGES.  Ask Linux's migration/compaction-backed contiguous
	 * allocator for those ranges so that they remain normal direct-mapped RAM
	 * and can be returned through free_contig_range(). */
	if (num_frames > MAX_ORDER_NR_PAGES) {
		struct page *pages;
		unsigned long phys;

		if (gfp == GFP_ATOMIC)
			return 0;
		pages = alloc_contig_pages(num_frames, GFP_KERNEL,
					 numa_node_id(), NULL);
		if (!pages)
			return 0;
		phys = page_to_phys(pages);
		if (align > 1 && (phys & (align - 1))) {
			free_contig_range(page_to_pfn(pages), num_frames);
			return 0;
		}
		p = page_address(pages);
		memset(p, 0, num_frames << PAGE_SHIFT);
		return phys;
	}
	p = alloc_pages_exact(num_frames << PAGE_SHIFT, gfp | __GFP_ZERO);
	if (!p || (align > 1 && ((unsigned long)p & (align - 1)))) {
		if (p) free_pages_exact(p, num_frames << PAGE_SHIFT);
		return 0;
	}
	return virt_to_phys(p);
}

void axvisor_linux_memory_dealloc_frame(unsigned long addr)
{
	if (addr) free_page((unsigned long)phys_to_virt(addr));
}

void axvisor_linux_memory_dealloc_contiguous(unsigned long addr, size_t num_frames)
{
	if (!addr || !num_frames)
		return;
	if (num_frames > MAX_ORDER_NR_PAGES) {
		free_contig_range(PHYS_PFN(addr), num_frames);
		return;
	}
	free_pages_exact(phys_to_virt(addr), num_frames << PAGE_SHIFT);
}

unsigned long axvisor_linux_memory_phys_to_virt(unsigned long addr)
{
	/* AxVisor uses the MemoryIf conversion for both guest RAM and host
	 * MMIO. Linux's direct map only covers RAM; device addresses (for
	 * example the QEMU PLIC at 0x0c000000) must be ioremapped first. */
	unsigned long page = addr & PAGE_MASK;
	unsigned long offset = addr & ~PAGE_MASK;
	unsigned long flags;
	unsigned int i;
	void __iomem *mapped;

	if (pfn_valid(page >> PAGE_SHIFT))
		return (unsigned long)phys_to_virt(addr);

	spin_lock_irqsave(&axvisor_io_maps_lock, flags);
	for (i = 0; i < axvisor_io_map_count; i++) {
		if (page >= axvisor_io_maps[i].phys &&
		    page - axvisor_io_maps[i].phys < axvisor_io_maps[i].size) {
			unsigned long result = (unsigned long)axvisor_io_maps[i].virt +
				offset + (page - axvisor_io_maps[i].phys);
			spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);
			return result;
		}
	}
	spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);

	/* Device resources are not part of Linux's linear map. Keep mappings
	 * page-granular so this works for PLIC, UART, virtio-mmio, and other host
	 * devices without architecture- or platform-specific address constants. */
	if (irqs_disabled() || in_atomic())
		return 0;
	mapped = ioremap(page, PAGE_SIZE);
	if (!mapped)
		return 0;
	spin_lock_irqsave(&axvisor_io_maps_lock, flags);
	if (axvisor_io_map_count >= ARRAY_SIZE(axvisor_io_maps)) {
		spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);
		iounmap(mapped);
		return 0;
	}
	axvisor_io_maps[axvisor_io_map_count].phys = page;
	axvisor_io_maps[axvisor_io_map_count].size = PAGE_SIZE;
	axvisor_io_maps[axvisor_io_map_count].virt = mapped;
	axvisor_io_map_count++;
	spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);
	return (unsigned long)mapped + offset;
}

/* Populate the MMIO cache before AxVisor enters interrupt-disabled setup
 * sections.  ioremap() may sleep, so doing this lazily from MemoryIf is not
 * valid when the core is initializing a passthrough device. */
void axvisor_linux_memory_prepare_io_maps(void)
{
#ifdef CONFIG_OF
	struct device_node *np;
	unsigned int index;

	for_each_of_allnodes(np) {
		for (index = 0; index < 8; index++) {
			struct resource resource;
			unsigned long page, end, size;
			void __iomem *mapped;
			unsigned long flags;
			unsigned int i;

			if (of_address_to_resource(np, index, &resource))
				break;
			if (!(resource.flags & IORESOURCE_MEM))
				continue;
			page = resource.start & PAGE_MASK;
			end = resource.end | ~PAGE_MASK;
			size = end - page + 1;
			if (pfn_valid(page >> PAGE_SHIFT))
				continue;

			spin_lock_irqsave(&axvisor_io_maps_lock, flags);
			for (i = 0; i < axvisor_io_map_count; i++) {
				if (page >= axvisor_io_maps[i].phys &&
				    page - axvisor_io_maps[i].phys < axvisor_io_maps[i].size)
					break;
			}
			spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);
			if (i < axvisor_io_map_count)
				continue;

			mapped = ioremap(page, size);
			if (!mapped)
				continue;
			spin_lock_irqsave(&axvisor_io_maps_lock, flags);
			if (axvisor_io_map_count < ARRAY_SIZE(axvisor_io_maps)) {
				axvisor_io_maps[axvisor_io_map_count].phys = page;
				axvisor_io_maps[axvisor_io_map_count].size = size;
				axvisor_io_maps[axvisor_io_map_count].virt = mapped;
				axvisor_io_map_count++;
				mapped = NULL;
			}
			spin_unlock_irqrestore(&axvisor_io_maps_lock, flags);
			if (mapped)
				iounmap(mapped);
		}
	}
#endif
}

unsigned long axvisor_linux_memory_virt_to_phys(unsigned long addr)
{
	return virt_to_phys((void *)addr);
}
