# Standard Unix with m68k-elf-gcc

TARGET = vlink
DIR = objects
MD = mkdir
RM = rm -f

CC = m68k-elf-gcc
CCOUT = -o $(DUMMYVARIABLE)	# produces the string "-o "
CFLAGS = -std=c99 -pedantic -O2 -fomit-frame-pointer -c -I../../../include -L../../../lib
CONFIG =

LD = $(CC)
LDOUT = -o $(DUMMYVARIABLE)	# produces the string "-o "
LDOPTS =
LIBS = c68k


include make.rules
