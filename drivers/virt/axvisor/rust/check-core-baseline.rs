// SPDX-License-Identifier: Apache-2.0

fn main() {
    println!("cargo:rerun-if-changed=../check-core-baseline.rs");
    if std::env::var_os("CARGO_FEATURE_ARM_VHE").is_some() {
        panic!(
            "Linux ARM VHE adapter migration is incomplete for the unmodified \
             x86 Core 4152c3e780feb5e44241fb3fa143fe04bdca0181. \
             This Core has runtime VHE support, but not the adapter's \
             vhe_host_enter/vhe_host_exit/create_user_fd_at interfaces or \
             vhe-selftest entry point. Integrate the Linux ownership boundary \
             using existing Core interfaces before enabling arm-vhe; do not \
             patch Core or substitute the previous ARM-specific Core revision."
        );
    }
}
