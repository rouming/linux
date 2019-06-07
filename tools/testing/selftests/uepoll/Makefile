# SPDX-License-Identifier: GPL-2.0

CC := $(CROSS_COMPILE)gcc
CFLAGS += -O2 -g -I../../../../usr/include/ -lnuma -lpthread

BUILTIN_SUPPORT := $(shell $(CC) -o /dev/null ./atomic-builtins-support.c >/dev/null 2>&1; echo $$?)

ifeq "$(BUILTIN_SUPPORT)" "0"
    TEST_GEN_PROGS := uepoll-test
else
    $(warning WARNING:)
    $(warning WARNING: uepoll compilation is skipped, gcc atomic builtins are not supported!)
    $(warning WARNING:)
endif

include ../lib.mk
