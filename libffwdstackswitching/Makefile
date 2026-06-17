SHELL = /bin/bash

CFLAGS += -O3 -g
LIBS = -lnuma -lpthread -lrt
MCMODEL = -mcmodel=large

#NPROC=$(shell nproc)
#N_SOCK=$(shell ./num_sock.sh)
#$(eval clients=$(shell echo $$((($(NPROC)/$(N_SOCK))-1))))

# Core ordering:
# STC : socket-filling
# CTS (default case) : core-filling

all: libffwd.o

platform_defs.h: platform_defs.h.in
	cat $< | sed -e "s/@num_numa_nodes@/$$(awk -F- '{print $$2-$$1+1}' /sys/devices/system/node/online)/g" > $@

# -S -fverbose-asm
libffwd.o: ffwd.c ffwd.h platform_defs.h ffwd_internals.h
	gcc $(CFLAGS) -Wall $(MCMODEL) -c -o libffwd.o ffwd.c $(LIBS);

ffwd_sample: libffwd.o ffwd.h ffwd_sample.c
	gcc $(CFLAGS) -o ffwd_sample libffwd.o ffwd_sample.c $(LIBS);

clean:
	rm -f platform_defs.h libffwd.o ffwd_sample
