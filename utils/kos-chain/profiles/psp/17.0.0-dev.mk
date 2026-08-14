# KallistiOS Toolchain Builder (kos-chain)

target=mipsel-psp-elf

# The PSP's Allegrex CPU is a MIPS32-derived core with a single-precision
# FPU. MIPS II is the safe baseline ISA, and --with-fpu=single makes
# -msingle-float the compiler default so newlib, libgcc, and libstdc++
# never contain double-precision FPU instructions.
cpu_configure_args=--with-arch=mips2 --with-float=hard --with-fpu=single --disable-multilib

# Toolchain versions for PSP
binutils_ver=2.46.1
gcc_ver=17.0.0

# Override toolchain download type
gcc_download_type=git
gcc_git_repo=git://gcc.gnu.org/git/gcc.git
gcc_git_branch=master
newlib_ver=4.6.0.20260123

# GCC dependencies
gmp_ver=6.2.1
mpfr_ver=4.1.0
mpc_ver=1.2.1
isl_ver=0.24

gcc_pass1_configure_args = \
  --disable-libgcc \
  --disable-shared \
  --disable-threads \
  --disable-multilib \
  --disable-libatomic \
  --disable-nls \
  --disable-tls \
  --disable-libgomp \
  --disable-libquadmath

# Native TLS is enabled. The Allegrex has no rdhwr instruction, so the
# KOS GCC patch makes pre-MIPS32r2 targets read the thread pointer via a
# call to __mips_get_tp, which the KOS PSP port must provide (the same
# arrangement as the PS2 EE's __r5900_get_tp).
gcc_pass2_configure_args = \
  --enable-cxx-flags=-G0 \
  --disable-multilib \
  --disable-libatomic \
  --disable-nls \
  --enable-tls
