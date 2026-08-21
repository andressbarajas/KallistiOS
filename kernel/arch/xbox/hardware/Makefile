# KallistiOS ##version##
#
# arch/xbox/hardware/Makefile
# Copyright (C) 2026 Cypress
#

# MCPX legacy block
OBJS = rtc.o

# MCPX SMBus host and the Xbox System Management Controller
OBJS += smbus.o smc.o

# Native kos-load syscall transport
OBJS += kosload_syscalls.o

SUBDIRS = usb

include $(KOS_BASE)/Makefile.prefab
