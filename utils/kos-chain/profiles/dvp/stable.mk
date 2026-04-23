# KallistiOS Toolchain Builder (kos-chain)

target=dvp-elf

# DVP is binutils-only (assembler and linker for VU microcode)
binutils_only=1

binutils_extra_configure_args += --disable-nls --disable-build-warnings

# Toolchain versions for PS2 DVP
binutils_ver=2.45.1
