// SPDX-License-Identifier: Apache-2.0

fn main() {
    println!("cargo:rerun-if-changed=../check-core-baseline.rs");
    if std::env::var_os("CARGO_FEATURE_ARM_VHE").is_some() {
        assert_eq!(
            std::env::var("CARGO_CFG_TARGET_ARCH").as_deref(),
            Ok("aarch64"),
            "arm-vhe requires the AArch64 kernel target"
        );
        assert_eq!(
            std::env::var("CARGO_CFG_TARGET_OS").as_deref(),
            Ok("none"),
            "Core's IRQ guard requires the freestanding kernel target"
        );
        assert!(
            std::env::var_os("CARGO_FEATURE_CONTROL").is_some(),
            "ARM VHE currently supports the KVM control endpoint; static VM mode is not migrated"
        );
    }
}
