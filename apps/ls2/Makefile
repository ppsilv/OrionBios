CC = m68k-elf-gcc
LD = m68k-elf-ld
OBJCOPY = m68k-elf-objcopy

SYS_ROOT=../..
APP = ls2

CFLAGS = -Wall -O0 -Wa,-adhln=$(APP).lst -I$(SYS_ROOT)/include -ffreestanding -nostdlib -D __68000__ -D ALIGN_MEMORY
LDFLAGS = -T $(SYS_ROOT)/lib/linker.ld -L$(SYS_ROOT)/lib -nostdlib
LDLIBS = -lc68k

SRCS = $(APP).c dummy.c 

OBJS = $(SRCS:.c=.o)
ELFS = $(SRCS:.c=.elf)
BINS = $(SRCS:.c=.bin)

.PHONY: all clean

all: $(APP).bin

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Regra específica para cada arquivo para evitar problemas com $<
$(APP).elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)
	@echo "Linked: $@"

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "Binary: $@"

clean:
	rm -f $(OBJS) $(ELFS) $(BINS) *.lst
	@echo "Clean complete"
