/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AXVISOR_LINUX_FFI_H
#define _AXVISOR_LINUX_FFI_H

#include <linux/types.h>

int axvisor_linux_vhe_prepare(void);
bool axvisor_linux_arm64_vhe_enabled(void);
void axvisor_linux_core_boot(void) __noreturn;
#ifdef CONFIG_AXVISOR_ARM64_VHE_SELFTEST
int axvisor_linux_vhe_selftest(void);
#endif
#ifdef CONFIG_AXVISOR_LINUX_CONFORMANCE
bool axvisor_linux_conformance_enabled(void);
int axvisor_linux_conformance_run(void);
bool axvisor_linux_conformance_trigger_irq(unsigned long vector);
#endif

void axvisor_linux_log_message(const u8 *message, size_t length);

size_t axvisor_linux_host_get_cpu_num(void);
size_t axvisor_linux_host_current_cpu(void);
int axvisor_linux_percpu_prepare(void);
void *_percpu_base_ptr(unsigned long cpu);
unsigned long ax_percpu_current_base(void);
void axvisor_linux_host_init_percpu(void);
void axvisor_linux_console_write_bytes(const u8 *bytes, size_t length);
void axvisor_linux_console_enqueue_bytes(const u8 *bytes, size_t length);
size_t axvisor_linux_console_read_bytes(u8 *bytes, size_t length);
int axvisor_linux_console_register_endpoint(void);
void axvisor_linux_host_exit(int code);
#ifdef CONFIG_AXVISOR_LINUX_CONTROL
int axvisor_linux_control_register_endpoint(void);
u64 axvisor_linux_control_open(void);
int axvisor_linux_control_close(u64 control_file);
long axvisor_linux_control_ioctl(u64 control_file, u32 cmd, unsigned long arg);
int axvisor_linux_control_create_fd(u64 control_file, u64 mmap_area);
int axvisor_linux_control_create_fd_at(u64 control_file, int __user *result);
int axvisor_linux_control_copy_from_user(void *dst, const void __user *src,
						 size_t length);
int axvisor_linux_control_copy_to_user(void __user *dst, const void *src,
						 size_t length);
u64 axvisor_linux_control_create_mmap_area(size_t length);
int axvisor_linux_control_read_mmap_area(u64 area, size_t offset, void *buf,
						 size_t length);
int axvisor_linux_control_write_mmap_area(u64 area, size_t offset,
						  const void *buf, size_t length);
int axvisor_linux_control_release_mmap_area(u64 area);
u64 axvisor_linux_control_get_fd_ref(int fd);
ssize_t axvisor_linux_control_write_fd_ref(u64 id, const void *buf,
						   size_t length);
ssize_t axvisor_linux_control_read_fd_ref(u64 id, void *buf, size_t length);
int axvisor_linux_control_release_fd_ref(u64 id);
u64 axvisor_linux_control_retain_user_address_space(void);
int axvisor_linux_control_release_user_address_space(u64 id);
u64 axvisor_linux_control_pin_user_pages(u64 mm_id, unsigned long addr,
						  size_t length, bool writable,
						  u64 *phys_pages, size_t max_pages,
						  size_t *page_count);
int axvisor_linux_control_release_pinned_pages(u64 id);
int axvisor_linux_control_pending_signal(const u8 *blocked_bytes, size_t length);
#endif
u64 axvisor_linux_time_current_time_nanos(void);
void axvisor_linux_time_set_oneshot_timer(u64 deadline_nanos);
unsigned long axvisor_linux_arch_host_fdt_paddr(void);
void axvisor_linux_arch_capture_host_fdt(void);
#ifdef CONFIG_ARM64
unsigned long axvisor_linux_arm64_gic_info(unsigned int which);
long axvisor_linux_vhe_enter(void);
void axvisor_linux_vhe_exit(unsigned long token);
#endif
void axvisor_linux_arch_remote_hfence_vvma_all(void);
unsigned int axvisor_linux_arch_host_tsc_frequency_mhz(void);
void *axvisor_linux_alloc(size_t size, size_t align);
void axvisor_linux_dealloc(void *ptr);
void axvisor_linux_task_yield(void);
unsigned long axvisor_linux_wait_queue_create(void);
void axvisor_linux_wait_queue_destroy(unsigned long queue);
void axvisor_linux_wait_queue_wait(unsigned long queue);
u64 axvisor_linux_wait_queue_generation(unsigned long queue);
void axvisor_linux_wait_queue_wait_since(unsigned long queue, u64 generation);
void axvisor_linux_wait_queue_wake_one(unsigned long queue);
void axvisor_linux_wait_queue_wake_all(unsigned long queue);
unsigned long axvisor_linux_spawn_task(int (*entry)(void *), void *data,
					       const u8 *name, unsigned long cpu_set);
void axvisor_linux_join_task(unsigned long handle);
unsigned long axvisor_linux_current_task(void);
bool axvisor_linux_handle_irq(unsigned long vector);
bool axvisor_linux_handle_registered_irq(unsigned long vector);
bool axvisor_linux_prepare_irq_vector(unsigned long vector);
#ifdef CONFIG_X86
bool axvisor_linux_dispatch_host_irq(unsigned long vector);
bool axvisor_linux_dispatch_host_system_irq(unsigned long vector);
#endif
#ifdef CONFIG_RISCV
void axvisor_linux_handle_pending_external_irqs(void);
void axvisor_linux_handle_pending_software_irq(void);
#endif
#ifdef CONFIG_AXVISOR_LINUX_CONTROL
int axvisor_linux_core_control_boot(void);
#endif
unsigned long axvisor_linux_irq_local_save(void);
void axvisor_linux_irq_local_restore(unsigned long flags);
unsigned long axvisor_linux_memory_alloc_frame(void);
unsigned long axvisor_linux_memory_alloc_contiguous(size_t num_frames, size_t align);
void axvisor_linux_memory_dealloc_frame(unsigned long addr);
void axvisor_linux_memory_dealloc_contiguous(unsigned long addr, size_t num_frames);
unsigned long axvisor_linux_memory_phys_to_virt(unsigned long addr);
void axvisor_linux_memory_prepare_io_maps(void);
unsigned long axvisor_linux_memory_virt_to_phys(unsigned long addr);

#endif
