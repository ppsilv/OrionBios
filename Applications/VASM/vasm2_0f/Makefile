# Unix, using gcc

CC = m68k-elf-gcc
TARGET =
TARGETEXTENSION =

CCOUT = -o $(DUMMY)
CFLAGS = -c -m68000 -std=c90 -ffreestanding -nostdlib -pedantic -Wno-long-long -DUNIX -DNO_FLOAT $(OUTFMTS) -I../../../include

LD = $(CC)
LDOUT = $(CCOUT)
LDFLAGS = -nostdlib -L../../../lib -lc -lgcc -lnosys

RM = rm -f

include make.rules
