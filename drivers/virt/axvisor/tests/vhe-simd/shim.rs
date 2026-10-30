// SPDX-License-Identifier: Apache-2.0
#![no_std]

#[path = "host-call.rs"]
mod host_call;

unsafe extern "C" {
    fn fh_simd_linux_callback(mode: u32) -> i32;
}

#[unsafe(no_mangle)]
pub extern "C" fn fh_simd_rust_call(mode: u32) -> i32 {
    let result: Result<i32, i32> = host_call::call(|| {
        let result = unsafe { fh_simd_linux_callback(mode) };
        if result < 0 { Err(result) } else { Ok(result) }
    });
    match result {
        Ok(value) => value,
        Err(error) => error,
    }
}
