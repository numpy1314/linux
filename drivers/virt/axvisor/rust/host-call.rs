// SPDX-License-Identifier: Apache-2.0
//! Release ARM kernel SIMD ownership only around calls back into Linux.
//!
//! Both Rust bridge objects use the softfloat target. Their ordinary Rust
//! code has no live SIMD values; Core's explicit SIMD world switch executes
//! with IRQs disabled, where the C helper deliberately does not release it.
//! This scope must not surround a call into Core or the KVM_RUN dispatch.

#[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
unsafe extern "C" {
    fn axvisor_linux_vhe_pause_simd() -> bool;
    fn axvisor_linux_vhe_resume_simd(paused: bool);
}

struct HostCall {
    #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
    paused: bool,
}

impl HostCall {
    #[inline]
    fn new() -> Self {
        Self {
            #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
            paused: unsafe { axvisor_linux_vhe_pause_simd() },
        }
    }
}

impl Drop for HostCall {
    #[inline]
    fn drop(&mut self) {
        #[cfg(all(target_arch = "aarch64", feature = "arm-vhe"))]
        unsafe {
            axvisor_linux_vhe_resume_simd(self.paused);
        }
    }
}

#[inline]
pub(crate) fn call<R>(operation: impl FnOnce() -> R) -> R {
    let _scope = HostCall::new();
    operation()
}
