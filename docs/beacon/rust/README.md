# Rust on Beacon riscv64

Rust std works on riscv64 Haiku with no changes to Rust: nightly Rust
(1.102, 2026-11-25) builds core/alloc/std (libc 0.2.191 already handles
64-bit Haiku) for the target spec `riscv64gc-unknown-haiku.json`, linked by
the Haiku cross gcc against a Haiku sysroot:

    RUSTFLAGS="-C link-arg=--sysroot=<haiku riscv64 sysroot>" \
    cargo +nightly build --release -Z build-std=std,panic_unwind \
        --target riscv64gc-unknown-haiku.json -Z json-target-spec

`stdtest.rs` passed on the Orange Pi RV (rv69): threads/channels/mutex,
thread-locals, files, Instant/SystemTime, env, HashMap, f64 formatting, DNS +
TCP (HTTP HEAD to example.com), and a caught thread panic (unwinding).

The binary carries PT_INTERP /lib/ld.so.1 (gcc's default for -pie); Haiku
ignores it and uses runtime_loader. Next: a built-in riscv64gc-unknown-haiku
target in rustc and a rust-std build for it, so ordinary builds (Firefox's)
can use `--target riscv64gc-unknown-haiku` without build-std.

## Built-in target (Rust 1.100.0)

`rust-1.100.0-riscv64-haiku.patch` adds `riscv64gc-unknown-haiku` to rustc
(new file targets/riscv64gc_unknown_haiku.rs, registered in spec/mod.rs, and
listed in bootstrap's STAGE0_MISSING_TARGETS). Build with `bootstrap.toml`
(`x.py dist rustc rust-std`; ~2.5 h on the laptop with 3 jobs). The C
compiler and linker wrappers add `--sysroot` (a Haiku riscv64 sysroot), or
compiler-rt fails on `#include_next <limits.h>`. Cargo's dist build needs host
OpenSSL headers; the official cargo 1.100.0 binary works instead.
Confirmed on the board: stdtest all PASS with a normal
`cargo build --target riscv64gc-unknown-haiku`.
