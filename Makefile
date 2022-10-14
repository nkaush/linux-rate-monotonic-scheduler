EXTRA_CFLAGS +=
APP_EXTRA_FLAGS:= -O2 -ansi -pedantic
KERNEL_SRC:= /lib/modules/$(shell uname -r)/build
SUBDIR= $(PWD)
GCC:=gcc
RM:=rm

.PHONY : clean

all: clean modules app

obj-m:= mp2.o

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(SUBDIR) modules

app: userapp.c userapp.h
	$(GCC) -o userapp userapp.c

clean:
	$(RM) -f userapp *~ *.ko *.o *.mod.c Module.symvers modules.order

wipe: clean
	find . -name "*.cmd" -delete
	find . -name "*.mod" -delete

ul: unload load 

logs:
	sudo dmesg | grep "MP2"

slab_logs: 
	sudo cat /proc/slabinfo | head -n 10

load:
	sudo insmod mp2.ko

unload: 
	sudo rmmod mp2.ko