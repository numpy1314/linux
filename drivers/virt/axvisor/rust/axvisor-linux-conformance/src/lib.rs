#![no_std]

extern crate alloc;

use core::sync::atomic::{AtomicBool, Ordering};

unsafe extern "C" {
    fn axvisor_linux_conformance_trigger_irq(vector: usize) -> bool;
    fn axvisor_linux_log_message(message: *const u8, length: usize);
}

fn print(message: &[u8]) {
    unsafe { axvisor_linux_log_message(message.as_ptr(), message.len()) }
}

struct LinuxStimulus;

impl axvisor_conformance::Stimulus for LinuxStimulus {
    #[cfg(target_arch = "x86_64")]
    fn test_irq_vector(&self) -> usize {
        // The physical stimulus is a host IPI, not a passthrough PCI route.
        0x60
    }

    fn init_percpu(&self) {
        axvisor_core::vmm::init_timer_percpu();
    }

    fn verify_oneshot_timer(&self) -> Option<bool> {
        static FIRED: AtomicBool = AtomicBool::new(false);
        const DELAY_NANOS: u64 = 5_000_000;
        const TIMEOUT_NANOS: u64 = 500_000_000;

        FIRED.store(false, Ordering::Release);
        let deadline = axvisor_api::time::current_time_nanos().saturating_add(DELAY_NANOS);
        axvisor_core::vmm::timer::register_timer(deadline, |_| {
            FIRED.store(true, Ordering::Release);
        });
        while !FIRED.load(Ordering::Acquire)
            && axvisor_api::time::current_time_nanos() < deadline.saturating_add(TIMEOUT_NANOS)
        {
            axvisor_api::task::yield_now();
        }
        Some(FIRED.load(Ordering::Acquire))
    }

    fn verify_irq_ingress(&self, test_vector: usize) -> Option<bool> {
        Some(unsafe { axvisor_linux_conformance_trigger_irq(test_vector) })
    }
}

static STIMULUS: LinuxStimulus = LinuxStimulus;

#[unsafe(no_mangle)]
pub extern "C" fn axvisor_linux_conformance_run() -> i32 {
    let report = axvisor_conformance::run(&STIMULUS);
    for case in report.cases() {
        let line = alloc::format!(
            "CONFORMANCE family={} check={} status={}\n",
            case.family,
            case.check,
            case.outcome.as_str()
        );
        print(line.as_bytes());
    }
    let summary = alloc::format!(
        "CONFORMANCE summary={} complete={}\n",
        if report.passed() { "PASS" } else { "FAIL" },
        report.complete()
    );
    print(summary.as_bytes());
    if report.passed() { 0 } else { -1 }
}
