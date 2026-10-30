#!/usr/bin/env python3
"""Build the freestanding Rust domain and its ARM64 Linux host, out of tree."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--config', required=True, type=Path)
    p.add_argument('--features', default='arm-vhe,control')
    p.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    p.add_argument('--jobs', type=int, default=6)
    a = p.parse_args()
    linux = Path(__file__).resolve().parents[2]
    rust = linux / 'drivers/virt/axvisor/rust'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['CARGO_TARGET_DIR'] = str(out / 'rust')
    env['RUSTFLAGS'] = '-C opt-level=2 -C panic=abort -C relocation-model=static -C force-unwind-tables=no'
    cargo = ['cargo']
    common = ['--locked', '-Z', 'json-target-spec', '-Z', 'build-std=core,alloc,compiler_builtins',
              '--target', 'targets/aarch64-linux-kernel.json', '--release']
    objects = out / 'drivers/virt/axvisor'
    objects.mkdir(parents=True, exist_ok=True)
    host_features = ','.join(f for f in a.features.split(',') if f in ('control', 'shell', 'arm-vhe'))
    host = objects / 'axvisor_linux_host.o'
    run(cargo + ['rustc', '-p', 'axvisor-linux-host'] + common
        + (['--features', host_features] if host_features else [])
        + ['--', f'--emit=obj={host}'], cwd=rust, env=env)
    run(cargo + ['build', '-p', 'axvisor-linux-core'] + common
        + (['--features', a.features] if a.features else []), cwd=rust, env=env)
    archive = out / 'rust/aarch64-linux-kernel/release/libaxvisor_linux_core.a'
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        run([a.cross_compile + 'ar', 'x', archive], cwd=tmp)
        run([a.cross_compile + 'ld', '-r', '-o', objects / 'axvisor_linux_core.o']
            + sorted(Path(tmp).glob('*.o')))
    # Composite dependencies must see a freshly emitted Rust object.
    (objects / 'axvisor_linux.o').unlink(missing_ok=True)
    make = ['make', '-C', linux, f'O={out}', 'ARCH=arm64', f'CROSS_COMPILE={a.cross_compile}']
    run(make + [f'KCONFIG_ALLCONFIG={a.config.resolve()}', 'allnoconfig'], env=env)
    run([linux / 'scripts/config', '--file', out / '.config', '-e', 'AXVISOR_LINUX_BRIDGE',
         '-e' if ('arm-vhe' in host_features) else '-d', 'AXVISOR_ARM64_VHE',
         '-e' if 'conformance-test' in a.features.split(',') else '-d', 'AXVISOR_LINUX_CONFORMANCE',
         '-e' if 'control' in a.features.split(',') else '-d', 'AXVISOR_LINUX_CONTROL'])
    run(make + ['olddefconfig'], env=env)
    config = (out / '.config').read_text()
    for forbidden in ('CONFIG_KVM=y', 'CONFIG_ARM64_64K_PAGES=y', 'CONFIG_ARM64_16K_PAGES=y'):
        if forbidden in config:
            raise SystemExit(f'Unsupported host profile: {forbidden}')
    if 'CONFIG_AXVISOR_LINUX_BRIDGE=y' not in config:
        raise SystemExit('bridge was not enabled')
    run(make + [f'-j{a.jobs}', 'Image'], env=env)


if __name__ == '__main__':
    main()
