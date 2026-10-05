#![no_std]

use core::{
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

use alloc::{boxed::Box, collections::BTreeMap, sync::Arc};
#[cfg(feature = "control")]
use ax_errno::{AxError, AxResult};
#[cfg(feature = "control")]
use axvisor_api::control::{self, ControlOps, PinnedUserPages};
use axvisor_api::{
    api_impl,
    arch::ArchIf,
    console::ConsoleIf,
    host::HostIf,
    irq::{IrqHandler, IrqIf},
    memory::{MemoryIf, PhysAddr, VirtAddr},
    sync::SyncIf,
    task::{TaskHandle, TaskIf, TaskOptions},
    time::TimeIf,
};

extern crate alloc;

mod wait;

unsafe extern "C" {
    fn axvisor_linux_host_get_cpu_num() -> usize;
    fn axvisor_linux_host_current_cpu() -> usize;
    fn axvisor_linux_core_init_percpu_reg(cpu_id: usize);
    fn axvisor_linux_host_init_percpu();
    #[cfg(feature = "shell")]
    fn axvisor_linux_host_exit(code: i32) -> !;
    fn axvisor_linux_console_write_bytes(bytes: *const u8, length: usize);
    fn axvisor_linux_console_read_bytes(bytes: *mut u8, length: usize) -> usize;
    fn axvisor_linux_time_current_time_nanos() -> u64;
    fn axvisor_linux_time_set_oneshot_timer(deadline_nanos: u64);
    #[cfg(any(
        target_arch = "aarch64",
        target_arch = "loongarch64",
        target_arch = "riscv64"
    ))]
    fn axvisor_linux_arch_host_fdt_paddr() -> usize;
    #[cfg(target_arch = "x86_64")]
    fn axvisor_linux_arch_host_tsc_frequency_mhz() -> u32;
    #[cfg(target_arch = "riscv64")]
    fn axvisor_linux_arch_remote_hfence_vvma_all();
    fn axvisor_linux_memory_alloc_frame() -> usize;
    fn axvisor_linux_memory_alloc_contiguous(num_frames: usize, align: usize) -> usize;
    fn axvisor_linux_memory_dealloc_frame(addr: usize);
    fn axvisor_linux_memory_dealloc_contiguous(addr: usize, num_frames: usize);
    fn axvisor_linux_memory_phys_to_virt(addr: usize) -> usize;
    fn axvisor_linux_memory_virt_to_phys(addr: usize) -> usize;
    fn axvisor_linux_task_yield();
    fn axvisor_linux_irq_local_save() -> usize;
    fn axvisor_linux_irq_local_restore(flags: usize);
    #[cfg(target_arch = "riscv64")]
    fn axvisor_linux_handle_pending_external_irqs();
    #[cfg(target_arch = "riscv64")]
    fn axvisor_linux_handle_pending_software_irq();
    fn axvisor_linux_wait_queue_create() -> usize;
    fn axvisor_linux_wait_queue_destroy(queue: usize);
    fn axvisor_linux_wait_queue_wait(queue: usize);
    fn axvisor_linux_wait_queue_generation(queue: usize) -> u64;
    fn axvisor_linux_wait_queue_wait_since(queue: usize, generation: u64);
    fn axvisor_linux_wait_queue_wake_one(queue: usize);
    fn axvisor_linux_wait_queue_wake_all(queue: usize);
    fn axvisor_linux_spawn_task(
        entry: extern "C" fn(*mut u8) -> i32,
        data: *mut u8,
        name: *const u8,
        cpu_set: usize,
    ) -> usize;
    fn axvisor_linux_join_task(handle: usize);
    fn axvisor_linux_current_task() -> usize;
    #[cfg(target_arch = "x86_64")]
    fn axvisor_linux_dispatch_host_irq(vector: usize) -> bool;
    #[cfg(target_arch = "x86_64")]
    fn axvisor_linux_dispatch_host_system_irq(vector: usize) -> bool;
    #[cfg(target_arch = "x86_64")]
    fn axvisor_linux_prepare_irq_vector(vector: usize) -> bool;
    #[cfg(target_arch = "aarch64")]
    fn axvisor_linux_arm64_gic_info(which: u32) -> usize;
    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn axvisor_linux_vhe_enter() -> isize;
    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn axvisor_linux_vhe_exit(token: usize);
    #[cfg(feature = "control")]
    fn axvisor_linux_control_register_endpoint() -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_copy_from_user(dst: *mut u8, src: usize, length: usize) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_copy_to_user(dst: usize, src: *const u8, length: usize) -> i32;
    #[cfg(feature = "control")]
    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn axvisor_linux_control_create_fd_at(control_file: u64, result: usize) -> i32;
    fn axvisor_linux_control_create_fd(control_file: u64, mmap_area: u64) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_create_mmap_area(length: usize) -> u64;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_read_mmap_area(
        area: u64,
        offset: usize,
        buf: *mut u8,
        length: usize,
    ) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_write_mmap_area(
        area: u64,
        offset: usize,
        buf: *const u8,
        length: usize,
    ) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_release_mmap_area(area: u64) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_get_fd_ref(fd: i32) -> u64;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_write_fd_ref(id: u64, buf: *const u8, length: usize) -> isize;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_read_fd_ref(id: u64, buf: *mut u8, length: usize) -> isize;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_release_fd_ref(id: u64) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_retain_user_address_space() -> u64;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_release_user_address_space(id: u64) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_pin_user_pages(
        mm_id: u64,
        addr: usize,
        length: usize,
        writable: bool,
        phys_pages: *mut u64,
        max_pages: usize,
        page_count: *mut usize,
    ) -> u64;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_release_pinned_pages(id: u64) -> i32;
    #[cfg(feature = "control")]
    fn axvisor_linux_control_pending_signal(blocked_bytes: *const u8, length: usize) -> i32;
}

pub struct LinuxHost;

static IRQ_HANDLERS: spin::Mutex<BTreeMap<usize, IrqHandler>> = spin::Mutex::new(BTreeMap::new());
#[cfg(feature = "control")]
static CONTROL_OPS: spin::Mutex<Option<ControlOps>> = spin::Mutex::new(None);

#[api_impl]
impl HostIf for LinuxHost {
    fn get_host_cpu_num() -> usize {
        unsafe { axvisor_linux_host_get_cpu_num() }
    }

    fn init_percpu() {
        // Select this host CPU's writable custom-base area before accessing
        // any AxVisor per-CPU static.
        let cpu_id = unsafe { axvisor_linux_host_current_cpu() };
        // The per-CPU crate is linked into the core static library (the host
        // object is emitted standalone), so use its exported C shim here.
        unsafe { axvisor_linux_core_init_percpu_reg(cpu_id) };
        unsafe { axvisor_linux_host_init_percpu() }
    }

    #[cfg(feature = "shell")]
    fn exit(code: i32) -> ! {
        unsafe { axvisor_linux_host_exit(code) }
    }
}

#[api_impl]
impl ConsoleIf for LinuxHost {
    fn write_bytes(bytes: &[u8]) {
        unsafe { axvisor_linux_console_write_bytes(bytes.as_ptr(), bytes.len()) }
    }

    fn read_bytes(bytes: &mut [u8]) -> usize {
        unsafe { axvisor_linux_console_read_bytes(bytes.as_mut_ptr(), bytes.len()) }
    }
}

#[api_impl]
impl TimeIf for LinuxHost {
    fn current_time_nanos() -> u64 {
        unsafe { axvisor_linux_time_current_time_nanos() }
    }

    fn set_oneshot_timer(deadline: Duration) {
        unsafe { axvisor_linux_time_set_oneshot_timer(deadline.as_nanos() as u64) }
    }
}

#[api_impl]
impl ArchIf for LinuxHost {
    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn vhe_host_enter() -> ax_errno::AxResult<usize> {
        let token = unsafe { axvisor_linux_vhe_enter() };
        if token < 0 {
            Err(ax_errno::AxError::Unsupported)
        } else {
            Ok(token as usize)
        }
    }

    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn vhe_host_exit(token: usize) {
        unsafe { axvisor_linux_vhe_exit(token) }
    }

    #[cfg(target_arch = "aarch64")]
    fn hardware_inject_virtual_interrupt(_vector: u8) {
        panic!("Linux requires the vCPU-owned VHE interrupt queue");
    }

    #[cfg(target_arch = "aarch64")]
    fn read_vgicd_typer() -> u32 {
        unsafe { axvisor_linux_arm64_gic_info(2) as u32 }
    }

    #[cfg(target_arch = "aarch64")]
    fn read_vgicd_iidr() -> u32 {
        unsafe { axvisor_linux_arm64_gic_info(3) as u32 }
    }

    #[cfg(target_arch = "aarch64")]
    fn get_host_gicd_base() -> PhysAddr {
        PhysAddr::from_usize(unsafe { axvisor_linux_arm64_gic_info(0) })
    }

    #[cfg(target_arch = "aarch64")]
    fn get_host_gicr_base() -> PhysAddr {
        PhysAddr::from_usize(unsafe { axvisor_linux_arm64_gic_info(1) })
    }

    #[cfg(target_arch = "aarch64")]
    fn fetch_irq() -> u64 {
        // Physical IRQ acknowledgement remains exclusively in Linux's GIC driver.
        1023
    }

    #[cfg(any(
        target_arch = "aarch64",
        target_arch = "loongarch64",
        target_arch = "riscv64"
    ))]
    fn host_fdt_paddr() -> Option<axvisor_api::memory::PhysAddr> {
        let addr = unsafe { axvisor_linux_arch_host_fdt_paddr() };
        (addr != 0).then(|| axvisor_api::memory::PhysAddr::from_usize(addr))
    }

    #[cfg(target_arch = "riscv64")]
    fn remote_hfence_vvma_all() {
        unsafe { axvisor_linux_arch_remote_hfence_vvma_all() }
    }

    #[cfg(target_arch = "x86_64")]
    fn host_tsc_frequency_mhz() -> Option<u32> {
        let frequency = unsafe { axvisor_linux_arch_host_tsc_frequency_mhz() };
        (frequency != 0).then_some(frequency)
    }
}

#[api_impl]
impl SyncIf for LinuxHost {
    fn create_wait_queue() -> usize {
        unsafe { axvisor_linux_wait_queue_create() }
    }
    fn destroy_wait_queue(queue: usize) {
        unsafe { axvisor_linux_wait_queue_destroy(queue) }
    }
    fn wait_queue_wait(queue: usize) {
        unsafe { axvisor_linux_wait_queue_wait(queue) }
    }
    fn wait_queue_wait_until(queue: usize, condition: Box<dyn Fn() -> bool + Send + 'static>) {
        wait::wait_until(
            condition,
            || unsafe { axvisor_linux_wait_queue_generation(queue) },
            |generation| unsafe { axvisor_linux_wait_queue_wait_since(queue, generation) },
        );
    }
    fn wait_queue_wake_one(queue: usize) {
        unsafe { axvisor_linux_wait_queue_wake_one(queue) }
    }
    fn wait_queue_wake_all(queue: usize) {
        unsafe { axvisor_linux_wait_queue_wake_all(queue) }
    }
}

#[api_impl]
impl TaskIf for LinuxHost {
    fn spawn_task_raw(
        options: TaskOptions,
        entry: Box<dyn FnOnce() + Send + 'static>,
    ) -> TaskHandle {
        let mut name = options.name.into_bytes();
        // Linux's kthread API takes a NUL-terminated format argument while
        // axvisor_api carries an owned Rust String.  Strip embedded NULs so
        // the complete task name is passed safely to the C adapter.
        name.retain(|byte| *byte != 0);
        name.push(0);
        // A Linux kthread may run immediately after wake_up_process().  Keep
        // it behind an explicit registration barrier until its handle has
        // been returned by C.
        let registered = Arc::new(AtomicBool::new(false));
        let registered_for_task = registered.clone();
        let holder: Box<Box<dyn FnOnce() + Send>> = Box::new(Box::new(move || {
            while !registered_for_task.load(Ordering::Acquire) {
                unsafe { axvisor_linux_task_yield() };
            }
            entry();
        }));
        let raw = Box::into_raw(holder) as *mut u8;
        let id = unsafe {
            axvisor_linux_spawn_task(
                axvisor_linux_task_trampoline,
                raw,
                name.as_ptr(),
                options.cpu_set.unwrap_or(0),
            )
        };
        if id == 0 {
            unsafe {
                drop(Box::from_raw(raw as *mut Box<dyn FnOnce() + Send>));
            }
        } else {
            registered.store(true, Ordering::Release);
        }
        unsafe { core::mem::transmute(id) }
    }
    fn join_task(task: TaskHandle) {
        let raw: usize = unsafe { core::mem::transmute(task) };
        unsafe { axvisor_linux_join_task(raw) }
    }
    fn current_task() -> Option<TaskHandle> {
        let raw = unsafe { axvisor_linux_current_task() };
        (raw != 0).then(|| unsafe { core::mem::transmute(raw) })
    }
    fn yield_now() {
        unsafe { axvisor_linux_task_yield() }
    }
}

extern "C" fn axvisor_linux_task_trampoline(raw: *mut u8) -> i32 {
    // The C kthread owns a Box<Box<dyn FnOnce()>> until this callback runs.
    let entry = unsafe { Box::from_raw(raw as *mut Box<dyn FnOnce() + Send>) };
    (*entry)();
    0
}

#[api_impl]
impl IrqIf for LinuxHost {
    fn handle_irq(vector: usize) -> bool {
        #[cfg(target_arch = "riscv64")]
        if vector == (1usize << (usize::BITS - 1)) + 1 {
            let flags = unsafe { axvisor_linux_irq_local_save() };
            unsafe { axvisor_linux_handle_pending_software_irq() };
            unsafe { axvisor_linux_irq_local_restore(flags) };
            return true;
        }
        #[cfg(target_arch = "riscv64")]
        if vector == (1usize << (usize::BITS - 1)) + 9 {
            // A RISC-V external interrupt can arrive as a VM exit while the
            // host Linux PLIC chained handler is bypassed.  Claim and
            // dispatch all pending host sources here, exactly as the normal
            // Linux PLIC path does, so a level-triggered source is completed
            // instead of causing an exit storm.
            // The VM-exit path is not entered through Linux's chained PLIC
            // handler, so establish the same IRQ-disabled section around the
            // claim/dispatch loop.  In particular, generic IRQ handlers
            // (8250, virtio, ...) require interrupts to be masked while they
            // run; restore the caller's state before KVM_RUN yields back to
            // userspace.
            let flags = unsafe { axvisor_linux_irq_local_save() };
            unsafe { axvisor_linux_handle_pending_external_irqs() };
            unsafe { axvisor_linux_irq_local_restore(flags) };
            return true;
        }
        #[cfg(target_arch = "x86_64")]
        // A VMX external-interrupt exit carries a host APIC vector.  Dispatch
        // it through Linux before consulting AxVisor's guest-vector registry;
        // the two vector spaces overlap numerically.
        if unsafe {
            axvisor_linux_dispatch_host_system_irq(vector)
                || axvisor_linux_dispatch_host_irq(vector)
        } {
            return true;
        }
        handle_registered_irq(vector)
    }
    fn register_irq_handler(vector: usize, handler: IrqHandler) -> bool {
        #[cfg(target_arch = "x86_64")]
        if (vector == 0x2a || vector == 0x2b)
            && !unsafe { axvisor_linux_prepare_irq_vector(vector) }
        {
            return false;
        }
        let flags = unsafe { axvisor_linux_irq_local_save() };
        let mut handlers = IRQ_HANDLERS.lock();
        if handlers.contains_key(&vector) {
            drop(handlers);
            unsafe { axvisor_linux_irq_local_restore(flags) };
            return false;
        }
        handlers.insert(vector, handler);
        drop(handlers);
        unsafe { axvisor_linux_irq_local_restore(flags) };
        true
    }
}

fn handle_registered_irq(vector: usize) -> bool {
    let flags = unsafe { axvisor_linux_irq_local_save() };
    let handler = IRQ_HANDLERS.lock().get(&vector).copied();
    unsafe { axvisor_linux_irq_local_restore(flags) };
    if let Some(handler) = handler {
        handler(vector);
        true
    } else {
        false
    }
}

/// Dispatch a vector already translated from a Linux IRQ descriptor into the
/// guest-vector namespace.
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_handle_registered_irq(vector: usize) -> bool {
    handle_registered_irq(vector)
}

#[api_impl]
impl MemoryIf for LinuxHost {
    fn alloc_frame() -> Option<PhysAddr> {
        let addr = unsafe { axvisor_linux_memory_alloc_frame() };
        (addr != 0).then(|| PhysAddr::from_usize(addr))
    }
    fn alloc_contiguous_frames(num_frames: usize, frame_align: usize) -> Option<PhysAddr> {
        let addr = unsafe { axvisor_linux_memory_alloc_contiguous(num_frames, frame_align) };
        (addr != 0).then(|| PhysAddr::from_usize(addr))
    }
    fn dealloc_frame(addr: PhysAddr) {
        unsafe { axvisor_linux_memory_dealloc_frame(addr.as_usize()) }
    }
    fn dealloc_contiguous_frames(addr: PhysAddr, num_frames: usize) {
        unsafe { axvisor_linux_memory_dealloc_contiguous(addr.as_usize(), num_frames) }
    }
    fn phys_to_virt(addr: PhysAddr) -> VirtAddr {
        VirtAddr::from_usize(unsafe { axvisor_linux_memory_phys_to_virt(addr.as_usize()) })
    }
    fn virt_to_phys(addr: VirtAddr) -> PhysAddr {
        PhysAddr::from_usize(unsafe { axvisor_linux_memory_virt_to_phys(addr.as_usize()) })
    }
}

#[cfg(feature = "control")]
#[api_impl]
impl control::ControlIf for LinuxHost {
    fn register_endpoint(ops: ControlOps) -> AxResult {
        let mut registered = CONTROL_OPS.lock();
        if registered.is_some() {
            return Err(AxError::AlreadyExists);
        }
        *registered = Some(ops);
        if unsafe { axvisor_linux_control_register_endpoint() } != 0 {
            *registered = None;
            return Err(AxError::Io);
        }
        Ok(())
    }

    fn create_user_fd(
        control_file: control::ControlFileId,
        _ops: ControlOps,
        mmap_area: Option<control::MmapAreaId>,
    ) -> AxResult<control::Fd> {
        let fd = unsafe { axvisor_linux_control_create_fd(control_file, mmap_area.unwrap_or(0)) };
        (fd >= 0).then_some(fd).ok_or(AxError::Io)
    }

    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    fn create_user_fd_at(control_file: u64, _ops: ControlOps, user_address: usize) -> AxResult {
        match unsafe { axvisor_linux_control_create_fd_at(control_file, user_address) } {
            0 => Ok(()),
            -14 => Err(AxError::BadAddress),
            -12 => Err(AxError::NoMemory),
            _ => Err(AxError::Io),
        }
    }

    fn get_user_fd_ref(fd: control::Fd) -> AxResult<control::UserFdRefId> {
        let id = unsafe { axvisor_linux_control_get_fd_ref(fd) };
        (id != 0).then_some(id).ok_or(AxError::BadFileDescriptor)
    }

    fn write_user_fd_ref(user_fd_ref: control::UserFdRefId, buf: &[u8]) -> AxResult<usize> {
        let ret =
            unsafe { axvisor_linux_control_write_fd_ref(user_fd_ref, buf.as_ptr(), buf.len()) };
        if ret >= 0 {
            Ok(ret as usize)
        } else if ret == -(11isize) {
            Err(AxError::WouldBlock)
        } else if ret == -(4isize) {
            Err(AxError::Interrupted)
        } else {
            Err(AxError::Io)
        }
    }

    fn read_user_fd_ref(user_fd_ref: control::UserFdRefId, buf: &mut [u8]) -> AxResult<usize> {
        let ret =
            unsafe { axvisor_linux_control_read_fd_ref(user_fd_ref, buf.as_mut_ptr(), buf.len()) };
        if ret >= 0 {
            Ok(ret as usize)
        } else if ret == -(11isize) {
            Err(AxError::WouldBlock)
        } else if ret == -(4isize) {
            Err(AxError::Interrupted)
        } else {
            Err(AxError::Io)
        }
    }

    fn release_user_fd_ref(user_fd_ref: control::UserFdRefId) -> AxResult {
        let ret = unsafe { axvisor_linux_control_release_fd_ref(user_fd_ref) };
        (ret == 0).then_some(()).ok_or(AxError::BadFileDescriptor)
    }

    fn create_mmap_area(len: usize) -> AxResult<control::MmapAreaId> {
        let area = unsafe { axvisor_linux_control_create_mmap_area(len) };
        (area != 0).then_some(area).ok_or(AxError::NoMemory)
    }

    fn read_mmap_area(area: control::MmapAreaId, offset: usize, buf: &mut [u8]) -> AxResult {
        let ret = unsafe {
            axvisor_linux_control_read_mmap_area(area, offset, buf.as_mut_ptr(), buf.len())
        };
        (ret == 0).then_some(()).ok_or(AxError::BadAddress)
    }

    fn write_mmap_area(area: control::MmapAreaId, offset: usize, buf: &[u8]) -> AxResult {
        let ret =
            unsafe { axvisor_linux_control_write_mmap_area(area, offset, buf.as_ptr(), buf.len()) };
        (ret == 0).then_some(()).ok_or(AxError::BadAddress)
    }

    fn release_mmap_area(area: control::MmapAreaId) -> AxResult {
        let ret = unsafe { axvisor_linux_control_release_mmap_area(area) };
        (ret == 0).then_some(()).ok_or(AxError::BadAddress)
    }

    fn copy_from_user(addr: usize, buf: &mut [u8]) -> AxResult {
        let ret =
            unsafe { axvisor_linux_control_copy_from_user(buf.as_mut_ptr(), addr, buf.len()) };
        (ret == 0).then_some(()).ok_or(AxError::BadAddress)
    }

    fn copy_to_user(addr: usize, buf: &[u8]) -> AxResult {
        let ret = unsafe { axvisor_linux_control_copy_to_user(addr, buf.as_ptr(), buf.len()) };
        (ret == 0).then_some(()).ok_or(AxError::BadAddress)
    }

    fn current_thread_has_pending_signal(blocked_signals: &[u8]) -> AxResult<bool> {
        let ret = unsafe {
            axvisor_linux_control_pending_signal(blocked_signals.as_ptr(), blocked_signals.len())
        };
        match ret {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(AxError::InvalidInput),
        }
    }

    fn retain_current_user_address_space() -> AxResult<control::UserAddressSpaceId> {
        let id = unsafe { axvisor_linux_control_retain_user_address_space() };
        (id != 0).then_some(id).ok_or(AxError::BadState)
    }

    fn release_user_address_space(id: control::UserAddressSpaceId) -> AxResult {
        let ret = unsafe { axvisor_linux_control_release_user_address_space(id) };
        (ret == 0).then_some(()).ok_or(AxError::BadState)
    }

    fn pin_user_pages(
        user_address_space: control::UserAddressSpaceId,
        addr: usize,
        len: usize,
        writable: bool,
    ) -> AxResult<PinnedUserPages> {
        let max_pages = len
            .checked_add(addr & (4096 - 1))
            .and_then(|size| size.checked_add(4095))
            .map(|size| size / 4096)
            .ok_or(AxError::InvalidInput)?;
        let mut pages = alloc::vec![0u64; max_pages];
        let mut count = 0usize;
        let id = unsafe {
            axvisor_linux_control_pin_user_pages(
                user_address_space,
                addr,
                len,
                writable,
                pages.as_mut_ptr(),
                pages.len(),
                &mut count,
            )
        };
        if id == 0 {
            return Err(AxError::BadAddress);
        }
        pages.truncate(count);
        Ok(PinnedUserPages {
            id: id as u64,
            pages: pages
                .into_iter()
                .map(|page| PhysAddr::from_usize(page as usize))
                .collect(),
        })
    }

    fn release_pinned_user_pages(id: control::PinnedUserPagesId) -> AxResult {
        let ret = unsafe { axvisor_linux_control_release_pinned_pages(id) };
        (ret == 0).then_some(()).ok_or(AxError::BadState)
    }
}

#[cfg(feature = "control")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_control_open() -> u64 {
    let ops = *CONTROL_OPS.lock();
    ops.and_then(|ops| (ops.open)().ok()).unwrap_or(0)
}

#[cfg(feature = "control")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_control_close(control_file: u64) -> i32 {
    let ops = *CONTROL_OPS.lock();
    ops.map(|ops| (ops.close)(control_file).is_ok())
        .unwrap_or(false) as i32
}

#[cfg(feature = "control")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_control_ioctl(control_file: u64, cmd: u32, arg: usize) -> isize {
    let ops = *CONTROL_OPS.lock();
    match ops {
        Some(ops) => match (ops.ioctl)(control_file, cmd, arg) {
            Ok(value) => value,
            Err(error) => {
                // KVM's ioctl ABI uses ENOTTY for commands unsupported by a
                // particular fd type (for example a system ioctl on a VM fd),
                // whereas AxError's generic Unsupported maps to EOPNOTSUPP.
                // Preserve the Linux KVM-visible errno here.
                let linux_error = match ax_errno::AxErrorKind::try_from(error) {
                    Ok(ax_errno::AxErrorKind::Unsupported) => ax_errno::LinuxError::ENOTTY,
                    _ => error.into(),
                };
                -(linux_error.code() as isize)
            }
        },
        None => -(ax_errno::LinuxError::ENODEV.code() as isize),
    }
}
