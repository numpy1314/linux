#![no_std]

extern crate alloc;

use core::{
    alloc::{GlobalAlloc, Layout},
    sync::atomic::{AtomicBool, Ordering},
};

struct LinuxAllocator;

// CONTROL_MODE_ACTIVE distinguishes host IRQ ownership. In control mode Linux owns
// the host PLIC and the normal Linux IRQ domain must receive external IRQs;
// in static mode AxVisor consumes/forwards registered IRQs itself.
static CONTROL_MODE_ACTIVE: AtomicBool = AtomicBool::new(false);

unsafe extern "C" {
    fn axvisor_linux_alloc(size: usize, align: usize) -> *mut u8;
    fn axvisor_linux_dealloc(ptr: *mut u8);
    fn axvisor_linux_log_message(message: *const u8, length: usize);
    fn axvisor_linux_task_yield();
    #[cfg(target_arch = "riscv64")]
    fn axvisor_linux_handle_pending_external_irqs();
}

fn boot_log(message: &[u8]) {
    unsafe { axvisor_linux_log_message(message.as_ptr(), message.len()) }
}

struct LinuxLogger;
static LOGGER: LinuxLogger = LinuxLogger;

#[cfg(feature = "vhe-selftest")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_vhe_selftest() -> i32 {
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
    for cpu in 0..axvisor_api::host::get_host_cpu_num() {
        let task = axvisor_api::task::spawn_task(
            axvisor_api::task::TaskOptions {
                name: alloc::format!("vhe-test-init-{cpu}"),
                stack_size: 65536,
                cpu_set: Some(1 << cpu),
            },
            || {
                axvisor_api::host::init_percpu();
                axvisor_core::vmm::init_timer_percpu();
            },
        );
        axvisor_api::task::join_task(task);
    }
    match arm_vcpu::vhe_test::run() {
        Ok(()) => {
            boot_log(b"VHE-TEST summary=PASS\n");
            0
        }
        Err(e) => {
            log::error!("VHE-TEST summary=FAIL error={e:?}");
            -1
        }
    }
}

/// Set the architecture per-CPU register for the current host CPU.  This is
/// exported as a C shim because the Linux host adapter is linked as a
/// standalone object while ax-percpu itself is included by this core library.
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_core_init_percpu_reg(cpu_id: usize) {
    ax_percpu::init_percpu_reg(cpu_id);
}

impl log::Log for LinuxLogger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        metadata.level() <= log::Level::Info
    }

    fn log(&self, record: &log::Record<'_>) {
        if !self.enabled(record.metadata()) {
            return;
        }
        struct Writer {
            buffer: [u8; 512],
            length: usize,
        }
        impl core::fmt::Write for Writer {
            fn write_str(&mut self, s: &str) -> core::fmt::Result {
                let count = s.len().min(self.buffer.len() - self.length);
                self.buffer[self.length..self.length + count]
                    .copy_from_slice(&s.as_bytes()[..count]);
                self.length += count;
                Ok(())
            }
        }
        let mut writer = Writer {
            buffer: [0; 512],
            length: 0,
        };
        let _ = core::fmt::Write::write_fmt(
            &mut writer,
            format_args!("axvisor-linux: [{}] {}\n", record.level(), record.args()),
        );
        boot_log(&writer.buffer[..writer.length]);
    }

    fn flush(&self) {}
}

unsafe impl GlobalAlloc for LinuxAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { axvisor_linux_alloc(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { axvisor_linux_dealloc(ptr) }
    }
}

#[global_allocator]
static ALLOCATOR: LinuxAllocator = LinuxAllocator;

#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    struct Writer;
    impl core::fmt::Write for Writer {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            unsafe { axvisor_linux_log_message(s.as_ptr(), s.len()) };
            Ok(())
        }
    }
    let _ = core::fmt::Write::write_fmt(
        &mut Writer,
        format_args!("axvisor-linux: core panic: {info}\n"),
    );
    loop {
        core::hint::spin_loop();
    }
}

/// Enter AxVisor's host-neutral static boot flow from Linux.
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_core_boot() -> ! {
    CONTROL_MODE_ACTIVE.store(false, Ordering::Release);
    boot_log(b"axvisor-linux: entering static boot flow\n");
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
    axvisor_core::boot::run_static_mode();
    loop {
        // `run_static_mode` returns when no VM remains. Keep the host kthread
        // alive without re-entering the provider trait dispatch path.
        unsafe { axvisor_linux_task_yield() };
    }
}

#[cfg(feature = "conformance-test")]
use axvisor_linux_conformance as _;

/// Initialize AxVisor's KVM-compatible control endpoint without starting the
/// static VM configuration. The Linux module selects this mode explicitly via
/// its `control` boot parameter.
#[cfg(feature = "control")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_core_control_boot() -> i32 {
    CONTROL_MODE_ACTIVE.store(true, Ordering::Release);
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
    match axvisor_core::boot::init_control_mode() {
        Ok(()) => 0,
        Err(err) => {
            CONTROL_MODE_ACTIVE.store(false, Ordering::Release);
            log::error!("AxVisor control initialization failed: {err:?}");
            -1
        }
    }
}

/// Called by the Linux hrtimer callback to service AxVisor timer events.
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_timer_interrupt() {
    axvisor_core::vmm::timer::check_events();
}

/// Offer a host IRQ to AxVisor before Linux dispatches the normal IRQ domain.
/// Returning false leaves the interrupt on Linux's regular path.
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_handle_irq(vector: usize) -> bool {
    #[cfg(target_arch = "riscv64")]
    if vector == (1usize << (usize::BITS - 1)) + 9 {
        // A guest VM-exit delivers the supervisor-external vector, not the
        // individual PLIC source. Claim all pending sources through the host adapter.
        unsafe { axvisor_linux_handle_pending_external_irqs() };
        return true;
    }
    if CONTROL_MODE_ACTIVE.load(Ordering::Acquire) {
        // In control mode this is the host PLIC path. Returning false lets
        // Linux's chained PLIC handler dispatch the IRQ through its domain,
        // matching the host PLIC dispatch contract.
        return false;
    }
    #[cfg(target_arch = "riscv64")]
    if !CONTROL_MODE_ACTIVE.load(Ordering::Acquire) {
        // In static mode all claimed host PLIC sources are passthrough guest
        // interrupts, matching the static IRQ forwarding path.
        if axvisor_core::arch::riscv64::inject_current_interrupt(vector) {
            return true;
        }
    }
    axvisor_api::irq::handle_irq(vector)
}

#[cfg(target_arch = "aarch64")]
#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_arm64_vhe_enabled() -> bool {
    cfg!(feature = "arm-vhe")
}
