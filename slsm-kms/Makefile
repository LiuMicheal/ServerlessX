export KDIR ?= /lib/modules/$(shell uname -r)/build

OFA_DIR ?= /usr/src/ofa_kernel
OFA = $(shell test -d $(OFA_DIR) && echo $(OFA_DIR) || echo /nonexistent)

ofa_autoconf_h = $(shell /bin/ls -1 $(OFA)/include/*/autoconf.h 2>/dev/null | head -1)
ifneq ($(ofa_autoconf_h),)
OFA_AUTOCONF_H = -include $(ofa_autoconf_h)
endif

autoconf_h = $(shell /bin/ls -1 $(KDIR)/include/*/autoconf.h 2>/dev/null | head -1)
export ofa_flags ?= -include $(autoconf_h) $(OFA_AUTOCONF_H) -I$(OFA)/include

ifneq ($(wildcard $(OFA)/Module.symvers),)
KBUILD_EXTRA_SYMBOLS = $(OFA)/Module.symvers
endif

CARGO ?= cargo
CC ?= gcc

.PHONY: all clean userspace

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(CC)

userspace:
	$(MAKE) -C ../slsm-user

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(CC) clean
	$(MAKE) -C ../slsm-user clean
