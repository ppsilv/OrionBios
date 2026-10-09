/*
 * Code Generator for UINSP200 CPU core
 * Copyright (C) 2022 Adrien Destugues <pulkomandy@pulkomandy.tk>
 *
 * Distributed under terms of the MIT license.
 */

#include "dt.h"

struct AddressingModes {
	int never_used;
};

typedef zllong zmax;
typedef zullong zumax;

#define MAXR 8 // Number of registers. Maybe SP and PC should not be part of this, and maybe MR should
#define MAXGF 1 // Number of command line flags
#define USEQ2ASZ 0 // Operand 2 cannot also be the target of operations

#define MINADDI2P CHAR // smallest type that can be added to a pointer

#define BIGENDIAN 1
#define LITTLEENDIAN 0

#define SWITCHSUBS 0 // Do not use SUB for switch/case

#define INLINEMEMCPY 0 // No inline memcpy for now

/* size of buffer for asm-output */
#define EMIT_BUF_LEN 1024 /* should be enough */
/* number of asm-output lines buffered */
#define EMIT_BUF_DEPTH 4

/*  We have some target-specific variable attributes. */
#define HAVE_TARGET_ATTRIBUTES 1

/* We use builtin libcalls for some operations */
#define HAVE_LIBCALLS 1

/* size_t is an unsigned int instead of unsigned long */
#define HAVE_INT_SIZET 1

/* Adding long or long long to a pointer is not allowed */
#define MAXADDI2P INT
