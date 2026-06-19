SHELL = /bin/bash

CFLAGS += -O3 -ggdb
LIBS = -lnuma -lpthread -lrt
MCMODEL = -mcmodel=large

#NPROC=$(shell nproc)
#N_SOCK=$(shell ./num_sock.sh)
#$(eval clients=$(shell echo $$((($(NPROC)/$(N_SOCK))-1))))

# Core ordering:
# STC : socket-filling
# CTS (default case) : core-filling

all: libffwd.a

platform_defs.h: platform_defs.h.in
	cat $< | sed -e "s/@num_numa_nodes@/$$(awk -F- '{print $$2-$$1+1}' /sys/devices/system/node/online)/g" > $@

# -S -fverbose-asm
ffwd.o: ffwd.c ffwd.h platform_defs.h ffwd_internals.h
	gcc $(CFLAGS) -Wall $(MCMODEL) -c -o ffwd.o ffwd.c

ffwd_asm.o: ffwd_asm.S ffwd.h platform_defs.h ffwd_internals.h
	gcc $(CFLAGS) -Wall $(MCMODEL) -c -o ffwd_asm.o ffwd_asm.S

libffwd.a: ffwd.o ffwd_asm.o
	ar rcs $@ $^

ffwd_sample: libffwd.a ffwd.h ffwd_sample.c
	gcc $(CFLAGS) -o $@ ffwd_sample.c libffwd.a $(LIBS)

clean:
	rm -f platform_defs.h libffwd.a *.o ffwd_sample
