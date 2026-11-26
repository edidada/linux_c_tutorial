obj-m += hello_module.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 在 QEMU guest 里编译时，KDIR 指向 guest 里准备的内核源码树：
#   make KDIR=/lib/modules/$(uname -r)/build
