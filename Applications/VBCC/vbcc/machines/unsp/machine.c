/*
 * Code generator for UNSP CPU core
 * Copyright (C) 2022 Adrien Destugues <pulkomandy@pulkomandy.tk>
 *
 * Distributed under terms of the MIT license.
 */

#include "supp.h"

#include <stdbool.h>

static char FILE_[] = __FILE__;

char cg_copyright[]="vbcc code-generator for unsp V0.2 (c) 2022-2024 by Adrien Destugues";

int g_flags[MAXGF] = { VALFLAG };
char* g_flags_name[MAXGF] = { "abi" }; // TODO support ABIs 1.0, 1.1 and 1.2.
union ppi g_flags_val[MAXGF];

#define USE_COMMONS (g_flags[19]&USEDFLAG)

struct Typ ityp={INT};

// Values in target endianness, initialized by init_cg

/*  Alignment-requirements for all types in bytes.              */
zmax align[MAX_TYPE+1];

/*  Alignment that is sufficient for every object.              */
zmax maxalign;

/*  CHAR_BIT of the target machine.                             */
zmax char_bit;
zmax sizetab[MAX_TYPE+1];
/*  Minimum and Maximum values each type can have.              */
/*  Must be initialized in init_cg().                           */
zmax t_min[MAX_TYPE+1];
zumax t_max[MAX_TYPE+1];
zumax tu_max[MAX_TYPE+1];

/*  Alignment-requirements for all types in bytes.              */
static long malign[MAX_TYPE+1]   = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

/*  Sizes of all elementary types in bytes.                     */
static long msizetab[MAX_TYPE+1] = { 0, 1, 1, 1, 2, 2, 2, 4, 4, 0, 1, 0, 0, 0, 1, 0 };

char* regnames[MAXR+1] = {"noreg", "SP", "R1", "R2", "R3", "R4", "BP", "SR", "PC" };

/*  The Size of each register in bytes.                         */
zmax regsize[MAXR+1];

/*  Type which can store each register. */
struct Typ *regtype[MAXR+1];

/*  regsa[reg]!=0 if a certain register is allocated and should */
/*  not be used by the compiler pass.                           */
int regsa[MAXR+1];

/*  Specifies which registers may be scratched by functions.    */
int regscratch[MAXR+1] = {0, 0, 1, 1, 1, 1, 0, 0, 0};

/* Names of target-specific variable attributes.                */
char *g_attr_name[]={"__interrupt",0};
#define INTERRUPT 1

#define DATA 0
#define BSS 1
#define CODE 2
#define RODATA 3
#define SPECIAL 4

static char *codename="\t.text\n\t.align\t2\n",
  *dataname="\t.data\n\t.align\t2\n",
  *bssname="",
  *rodataname="\t.section\t.rodata\n\t.align\t2\n",
  *tocname="\t.tocd\n\t.align\t2\n",
  *sdataname="\t.section\t\".sdata\",\"aw\"\n\t.align\t2\n",
  *sdata2name="\t.section\t\".sdata2\",\"a\"\n\t.align\t2\n",
  *sbssname="\t.section\t\".sbss\",\"auw\"\n\t.align\t2\n";
static char *labprefix="l",*idprefix="_",*tocprefix="@_";
static int newobj;
int current_section;

static void emit_obj(FILE *,obj *,int);

static int special_section(FILE *f,struct Var *v)
{
  char *sec;
  if(!v->vattr) return 0;
  sec=strstr(v->vattr,"section(");
  if(!sec) return 0;
  sec+=strlen("section(");
  emit(f,"\t.section\t");
  while(*sec&&*sec!=')') emit_char(f,*sec++);
  emit(f,"\n");
  if(f) current_section=SPECIAL;
  return 1;
}

int init_cg(void)
{
	maxalign=l2zm(1L);
	char_bit = l2zm(16L);

	current_section = 0;

	for (int i = 0; i <= MAX_TYPE; i++) {
		sizetab[i] = l2zm(msizetab[i]);
		align[i] = l2zm(malign[i]);
	}

	for (int i = 1; i <= MAXR; i++) {
		regsize[i] = l2zm(1);
		regtype[i]=&ityp;
	}

  /*  Initialize the min/max-settings. */
  t_min[CHAR]=l2zm(-32767);
  t_min[SHORT]= t_min[CHAR];
  t_min[INT]=t_min[CHAR];
  t_min[LONG]=l2zm(-2147483647);
  t_min[LLONG]=t_min[LONG];
  t_min[MAXINT]=t_min[LONG];

  t_max[CHAR]=ul2zum(32768);
  t_max[SHORT]=t_max[CHAR];
  t_max[INT]=t_max[CHAR];
  t_max[LONG]=l2zm(2147483647);
  t_max[LLONG]=t_max[LONG];
  t_max[MAXINT]=t_max[LONG];

  tu_max[CHAR]=ul2zum(65535);
  tu_max[SHORT]=tu_max[CHAR];
  tu_max[INT]=tu_max[CHAR];
  tu_max[LONG]=ul2zum(4294967295);
  tu_max[LLONG]=tu_max[LONG];
  tu_max[MAXINT]=tu_max[LONG];

  declare_builtin("__div", INT, INT, 0, INT, 0, 0, 0);
  declare_builtin("__mod", INT, INT, 0, INT, 0, 0, 0);

  regsa[0+1] = 1; // Reserve SP for the code generator
  regsa[5+1] = 1; // Reserve BP for the code generator
  regsa[6+1] = 1; // Reserve SR for the code generator
  regsa[7+1] = 1; // Reserve PC for the code generator

	return 1;
}

int freturn(struct Typ *t)
/*  Returns the register in which variables of type t are returned. */
/*  If the value cannot be returned in a register returns 0.        */
{
	return 1; // return values in R1, why not?
}

int regok(int r,int t,int mode)
/*  Returns 0 if register r cannot store variables of   */
/*  type t. If t==POINTER and mode!=0 then it returns   */
/*  non-zero only if the register can store a pointer   */
/*  and dereference a pointer to mode.                  */
{
	// Only 16 bit values can be stored in registers
	return msizetab[t & NQ] <= 1;
}

int reg_pair(int r, struct rpair* p)
/* Returns 0 if the register is no register pair. If r  */
/* is a register pair non-zero will be returned and the */
/* structure pointed to p will be filled with the two   */
/* elements.                                            */
{
	// FIXME handle MR (R3 pair R4)
	return 0;
}

int shortcut(int c, int t)
{
	// All implicit conversions are allowed
	return 1;
}

void cleanup_cg(FILE *f)
{
}

int dangerous_IC(struct IC *p)
/*  Returns zero if the IC p can be safely executed     */
/*  without danger of exceptions or similar things.     */
/*  vbcc may generate code in which non-dangerous ICs   */
/*  are sometimes executed although control-flow may    */
/*  never reach them (mainly when moving computations   */
/*  out of loops).                                      */
/*  Typical ICs that generate exceptions on some        */
/*  machines are:                                       */
/*      - accesses via pointers                         */
/*      - division/modulo                               */
/*      - overflow on signed integer/floats             */
{
	return 0;
}

static long real_offset(struct obj *o)
{
  long off;
  if(zm2l(o->v->offset)>=0){
     return zm2l(o->v->offset)+zm2l(o->val.vmax);
  }else{
    return zm2l(o->v->offset)-zm2l(maxalign)+zm2l(o->val.vmax);
  }
}
static void emit_obj(FILE *f,struct obj *p,int t)
/*  Prints an object.                               */
{
#if 0
  if(p->am){
    if(p->am->flags&REG_IND) emit(f,"%s,%s",mregnames[p->am->offset],mregnames[p->am->base]);
    if(p->am->flags&IMM_IND) emit(f,"%ld(%s)",p->am->offset,mregnames[p->am->base]);
    return;
  }
#endif
  /*  if(p->flags&DREFOBJ) emit(f,"(");*/
  if(p->flags&VAR) {
    if(isauto(p->v->storage_class)){
      if(p->flags&REG){
        emit(f,"%s",regnames[p->reg]);
      }else{
        emit(f,"%ld(%s)",real_offset(p),"BP");
      }
    }else{
      if(!zmeqto(l2zm(0L),p->val.vmax)){emitval(f,&p->val,MAXINT);emit(f,"+");}
      if(isstatic(p->v->storage_class)){
        if (ISFUNC(p->v->vtyp->flags)) {
            emit(f,"_%s",p->v->identifier);
        } else {
            emit(f,"%s%ld ; %d",labprefix,zm2l(p->v->offset), p->v->vtyp->flags);
        }
      }else{
        emit(f,"%s%s",idprefix,p->v->identifier);
      }
    }
  }
  if((p->flags&REG)&&!(p->flags&VAR)) emit(f,"%s",regnames[p->reg]);
  if(p->flags&KONST){
    emitval(f,&p->val,t&NU);
  }
  /*  if(p->flags&DREFOBJ) emit(f,")");*/
}
static int exists_freereg(struct IC *p,int reg)
/*  Test if there is a sequence of FREEREGs containing FREEREG reg. */
{
  while(p&&(p->code==FREEREG||p->code==ALLOCREG)){
    if(p->code==FREEREG&&p->q1.reg==reg) return 1;
    p=p->next;
  }
  return 0;
}

int must_convert(int o,int t,int const_expr)
/*  Returns zero if code for converting np to type t    */
/*  can be omitted.                                     */
{
	int op=o&NQ,tp=t&NQ;

	if(ISFLOAT(op) != ISFLOAT(tp)) return 1;
	return 0; // all types are the same
}

static void title(FILE *f)
/* set file symbol with input file name */
{
  extern char *inname;
  static int done;

  if (!done && f) {
    done = 1;
    emit(f,"\t.file\t\"%s\"\n",inname);
  }
}


static long pof2(zumax x)
/*  Yields log2(x)+1 oder 0. */
{
  zumax p;int ln=1;
  p=ul2zum(1L);
  while(ln<=32&&zumleq(p,x)){
    if(zumeqto(x,p)) return ln;
    ln++;p=zumadd(p,p);
  }
  return 0;
}

void gen_ds(FILE *f,zmax size,struct Typ *t)
/*  This function has to create <size> bytes of storage */
/*  initialized with zero.                              */
{
  title(f);

  if (newobj && current_section!=SPECIAL)
    emit(f,"%ld\n",zm2l(size));
  else
    emit(f,"\t.space\t%ld\n",zm2l(size));

  newobj = 0;
}

void gen_align(FILE *f,zmax align)
/*  This function has to make sure the next data is     */
/*  aligned to multiples of <align> bytes.              */
{
  title(f);
  if(!optsize&&(zm2l(align)<4))
    emit(f,"\t.align\t2\n");
  else
    if(zm2l(align)>1)
      emit(f,"\t.align\t%ld\n",pof2(align)-1);
}

void gen_dc(FILE *f,int t,struct const_list *p)
/*  This function has to create static storage          */
/*  initialized with const-list p.                      */
{
  title(f);

  if ((t&NQ) == POINTER)
    t = UNSIGNED|LONG;
  emit(f,"\t.2byte\t");

  if (!p->tree) {
    if (ISFLOAT(t)) {
      unsigned char *ip = (unsigned char *)&p->val.vdouble;

      emit(f,"0x%02x%02x%02x%02x",ip[0],ip[1],ip[2],ip[3]);
      if ((t&NQ) != FLOAT)
        emit(f,",0x%02x%02x%02x%02x",ip[4],ip[5],ip[6],ip[7]);
    }
    else {
      emitval(f,&p->val,t&NU);
    }
  }
  else {
    emit_obj(f,&p->tree->o,t&NU);
  }

  emit(f, "\n");
  newobj = 0;
}

void init_db(FILE *f)
{
}

void cleanup_db(FILE *f)
{
}

char* use_libcall(int c, int t, int t2)
{
	// TODO separate signed/unsigned
	// TODO check if the divider is a power or two. Then convert div into shifts, and mod into AND
	if (c == MOD)
		return "__mod";
	if (c == DIV)
		return "__div";

	return 0;
}

const char* currentseg = "codeseg";

const char* const codeseg = "codeseg";
const char* const dataseg = "dataseg";

void gen_var_head(FILE *f,struct Var *v)
/*  This function has to create the head of a variable  */
/*  definition, i.e. the label and information for      */
/*  linkage etc.                                        */
{
  int constflag;
  char *sec;

  title(f);

  if(v->clist)
    constflag = is_const(v->vtyp);

  if (isstatic(v->storage_class)) {
    if (ISFUNC(v->vtyp->flags))
      return;
    if (!special_section(f,v)) {
      if (v->clist && (!constflag) && current_section!=DATA) {
        emit(f,dataname);
        if (f)
          current_section = DATA;
      }
      if (v->clist && constflag && current_section!=RODATA) {
        emit(f,rodataname);
        if (f)
          current_section = RODATA;
      }
      if (!v->clist && current_section!=BSS) {
        emit(f,bssname);
        if (f)
          current_section = BSS;
      }
    }
    if (v->clist || current_section==SPECIAL) {
      gen_align(f,falign(v->vtyp));
      emit(f,"%s%ld:\n",labprefix,zm2l(v->offset));
    }
    else
      emit(f,"\t.lcomm\t%s%ld,",labprefix,zm2l(v->offset));
    newobj = 1;
  }

  if (isextern(v->storage_class)) {
    emit(f,"\t.globl\t%s%s\n",idprefix,v->identifier);
    if (v->flags & (DEFINED|TENTATIVE)) {
      if (!special_section(f,v)) {
        if (v->clist && (!constflag) && current_section!=DATA) {
          emit(f,dataname);
          if(f)
            current_section = DATA;
        }
        if (v->clist && constflag && current_section!=RODATA) {
          emit(f,rodataname);
          if (f)
            current_section = RODATA;
        }
        if (!v->clist && current_section!=BSS) {
          emit(f,bssname);
          if (f)
            current_section = BSS;
        }
      }
      if (v->clist || current_section==SPECIAL) {
        gen_align(f,falign(v->vtyp));
        emit(f,"%s%s:\n",idprefix,v->identifier);
      }
      else {
        emit(f,"\t.global\t%s%s\n\t.%scomm\t%s%s,",idprefix,
             v->identifier,(USE_COMMONS?"":"l"),idprefix,v->identifier);
      }
      newobj = 1;
    }
  }
}

static int localslot(int localslots, int vo)
{
	if (vo < 0) {
		return localslots + 2 - vo;
	} else {
		return vo;
	}
}

/** Emit appropriate code to access a variable.
 *
 * f: output assembler file
 * p: instruction being generated (TODO: remove, used for printic in case of errors only)
 * op: operand containing the variable being generated
 * localslots: size of local variables area for computing BP relative offsets
 * store: determine if this is a store to the variable or a read (FIXME this may be wrong and we should rely on DREFOBJ instead?)
 * word: word offset (1 to access the high word of long values, otherwise should be 0)
 */
static void emitvar(FILE* f, IC* p, struct obj* op, int localslots, bool store, int word)
{
	// FIXME handle DREFOBJ and VARADR correctly in all cases
	// DREFOBJ: the variable is a pointer and we need to access the pointed data
	// VARADR: only for static and extern variables, get the address of the variable
	if (op->flags & DREFOBJ) {
		if (op->flags & REG) {
			emit(f, "(%s)", regnames[op->reg]);
		} else if (isextern(op->v->storage_class)) {
			emit(f, "(_%s)", op->v->identifier);
		} else if (isauto(op->v->storage_class))
			emit(f, "(BP+%d)", localslot(localslots, op->v->offset) + word);
		else if (isstatic(op->v->storage_class))
			emit(f, "(%s%d + %d)", labprefix, op->v->offset, op->val.vlong);
		else {
			emit(f, "(%s%d)", labprefix, op->v->offset);
		}
	} else if (op->flags & VARADR) {
		if (isauto(op->v->storage_class))
			emit(f, "BP, %d", op->v->offset);
		else if (isstatic(op->v->storage_class))
			emit(f, "%s%d + %d", labprefix, op->v->offset, op->val.vlong);
		else if (isextern(op->v->storage_class)) {
			emit(f, "_%s + %d", op->v->identifier, word);
		} else {
			printic(stderr, p);
			ierror(0);
		}
	} else {
		if (op->flags & REG) {
			emit(f, "%s", regnames[op->reg]);
		} else if (isauto(op->v->storage_class))
			emit(f, "(BP+%d)", localslot(localslots, op->v->offset) + word);
		else if (isstatic(op->v->storage_class)) {
			emit(f, "(%s%d + %d)", labprefix, op->v->offset, op->val.vlong);
		} else if (isextern(op->v->storage_class))
			if (store)
				emit(f, "(_%s)", op->v->identifier);
			else
				emit(f, "%c_%s", word ? '>' : '<', op->v->identifier);
		else {
			printic(stderr, p);
			ierror(0);
		}
	}
}

static int cg_getreg(FILE* f, struct IC* ic, int excludedreg)
{
	for (int i = 2; i <= MAXR; i++)
	{
		if (regs[i] == 0) {
			regs[i] = 2;
			return i;
		}
	}

	// TODO if the next IC is a freereg, we may be able to use that register, in some cases (where
	// we don't need to use it as a source register anymore in the current IC)

	// no register is available, we need to push/pop one, making sure it is not used by the IC
	// For PUSH IC this is not possible, because the IC needs to use the stack, so the push/pop
	// sequence won't be correct. Maybe we can use R1 since it's also used for the return value
	// and so it is possibly corrupt by the called function?
	for (int reg = 2; reg <= 5; reg++) {
		if ((ic->q1.flags & REG) && (ic->q1.reg == reg))
			continue;
		if ((ic->q2.flags & REG) && (ic->q2.reg == reg))
			continue;
		if ((ic->z.flags & REG) && (ic->z.reg == reg))
			continue;
		if ((excludedreg > 0) && (excludedreg == reg))
			continue;

		if (regs[reg] < 3)
			regs[reg] = 3;
		else
			regs[reg]++;

		if ((ic->code == PUSH) || (ic->code == CALL)) {
			// No register is available, and this is a PUSH IC so we can't use the stack to
			// temporarily save one. Instead, use a preset memory address (at top of stack) which
			// is reserved for this usage.
			emit(f, "\tST %s, (0x27FF)\n", regnames[reg]);
		} else {
			emit(f, "\tPUSH %s, %s, (SP)\n", regnames[reg], regnames[reg]);
		}
		return reg;
	}
	fprintf(stderr, "No more registers!\n");
	return 0;
}

static void cg_freereg(FILE* f, int r, struct IC* ic)
{
	if (regs[r] < 2) {
		fprintf(stderr, "oops\n");
		ierror(0);
		return;
	}

	if (regs[r] >= 3) {
		regs[r] --;
		if (ic->code == PUSH) {
				emit(f, "\tLD %s, (0x27FF)\n", regnames[r]);
		} else {
			emit(f, "\tPOP %s, %s, (SP)\n", regnames[r], regnames[r]);
		}
	} else {
		regs[r] = 0;
	}
}

static int emitoperand(FILE* f, IC* p, struct obj* op, int offset, bool store, int word)
{
	if (op->flags & VAR) {
		emitvar(f, p, op, offset, store, word);
	} else if (op->flags & KONST) {
		if (op->flags & DREFOBJ) {
			emit(f, "(");
			emitval(f, &op->val, store ? ztyp(p) : q1typ(p));
			emit(f, ")");

		} else {
			emitval(f, &op->val, store ? ztyp(p) : q1typ(p));
		}
	} else {
		ierror(0);
	}
}

static int cg_aluop(const char* op, FILE* f, IC* p, zmax offset)
{
	emit(f, "; Q1 flags %d storage %d\n", p->q1.flags, p->q1.v ? p->q1.v->storage_class : 0);
	emit(f, "; Q2 flags %d\n", p->q2.flags);
	emit(f, "; Z  flags %d\n", p->z.flags);
	if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ) && (p->q1.flags & VAR) && (p->q2.flags & VAR|KONST)) {
		// Target is  register
		if (p->q1.flags & REG) {
			if (p->q1.flags & DREFOBJ)
				emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->q1.reg]);
			else if (p->z.reg != p->q1.reg)
				emit(f, "\tLD\t%s, %s\n", regnames[p->z.reg], regnames[p->q1.reg]);
		} else {
			emit(f, "\tLD\t%s, ", regnames[p->z.reg]);
			emitvar(f, p, &p->q1, offset, false, 0);
			emit(f, "\n");


			if ((p->q1.flags & DREFOBJ) && isauto(p->q1.v->storage_class))
				emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->z.reg]);
		}

		emit(f, "\t%s\t%s, ", op, regnames[p->z.reg]);

		if (p->q2.flags & REG) {
			emit(f, "%s\n", regnames[p->q2.reg]);
		} else if (p->q2.flags & KONST) {
			emitval(f, &p->q2.val, q2typ(p));
			emit(f, "\n");
		} else {
			emitvar(f, p, &p->q2, offset, true, 0);
			emit(f, "\n");
		}
		return 1;
	}

	// Target is not a register or q1 is const or VARADR or DREFOBJ are involved?
	if ((p->z.flags & VAR)) {
		if ((p->q1.flags & REG) && !(p->q1.flags & DREFOBJ)
				&& (p->z.flags & REG) && !(p->z.flags & DREFOBJ) && (p->q2.flags & KONST)) {
			emit(f, "\t%s\t%s, %s, ", op, regnames[p->z.reg], regnames[p->q1.reg]);
			emitval(f, &p->q2.val, q2typ(p));
			emit(f, "\n");
		} else {
			// TODO if z is not REG but q1 is REG and there is a freereg for it just after this IC,
			// we should use q1 register here instead of allocating a temporary
			int tmpreg;
			if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ)) {
				// We can use the destination register directly
				tmpreg = p->z.reg;
			} else {
				tmpreg = cg_getreg(f, p, -1);
			}

			// Load the source if we need to
			if (!(p->q1.flags & REG) || (p->q1.flags & DREFOBJ) || (p->q1.reg != tmpreg)) {
				emit(f, "\tLD\t%s, ", regnames[tmpreg]);
				emitoperand(f, p, &p->q1, offset, false, 0);
				emit(f, "\n");
			}

			if ((p->q2.flags & DREFOBJ) && !(p->q2.flags & REG)) {
				// Extra step needed to dereference q2
				int tmpreg2;
				tmpreg2 = cg_getreg(f, p, tmpreg);

				emit(f, "\tLD\t%s, ", regnames[tmpreg2]);
				emitoperand(f, p, &p->q2, offset, false, 0);
				emit(f, "\n");

				// Do the operation
				emit(f, "\t%s\t%s, (%s)\n", op, regnames[tmpreg], regnames[tmpreg2]);

				cg_freereg(f, tmpreg2, p);
			} else {
				// Do the operation
				emit(f, "\t%s\t%s, ", op, regnames[tmpreg]);
				if (p->q2.flags & VAR) {
					emitvar(f, p, &p->q2, offset, false, 0);
				} else if (p->q2.flags & KONST) {
					emitval(f, &p->q2.val, q2typ(p));
				} else {
					ierror(0);
				}
			}
			emit(f, "\n");

			// Store the result
			if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ)) {
				// Operation was done directly in target register, no need to store
			} else {
				emit(f, "\tST\t%s, ", regnames[tmpreg]);
				emitvar(f, p, &p->z, offset, true, 0);
				emit(f, "\n");
				cg_freereg(f, tmpreg, p);
			}
		}
		return 1;
	}

	if ((p->q2.flags & KONST) && (p->z.flags & (KONST | DREFOBJ)))
	{
		int tmpreg = cg_getreg(f, p, -1);
		if (p->q1.flags & (KONST | DREFOBJ)) {
			emit(f, "\tLD\t%s, (", regnames[tmpreg]);
			emitval(f, &p->q1.val, q1typ(p));
			emit(f, ")\n");
		} else if (p->q1.flags & VAR) {
			emit(f, "\tLD\t%s, ", regnames[tmpreg]);
			emitvar(f, p, &p->q1, offset, false, 0);
			emit(f, "\n");
		}

		emit(f, "\t%s\t%s, ", op, regnames[tmpreg]);
		emitval(f, &p->q2.val, q2typ(p));
		emit(f, "\n");

		emit(f, "\tST\t%s, (", regnames[tmpreg]);
		emitval(f, &p->z.val, ztyp(p));
		emit(f, ")\n");

		cg_freereg(f, tmpreg, p);
		
		return 1;
	}

	printf("ALUOP OPS %d %d %d\n", p->q1.flags, p->q2.flags, p->z.flags);
	return 0;
}

static int cg_ptrop(const char* op, FILE* f, IC* p, zmax offset)
{
	emit(f, ";ptrop %d %d %d\n", p->q1.flags, p->q2.flags, p->z.flags);
	if ((p->q1.flags & REG) && (p->q2.flags & REG) && (p->z.flags & REG) && (p->q1.reg == p->z.reg)) {
		if (p->q1.flags & DREFOBJ) {
			emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->q1.reg]);
		}
		if (p->q2.flags & DREFOBJ) {
			emit(f, "\t%s\t%s, (%s)\n", op, regnames[p->z.reg], regnames[p->q2.reg]);
		} else {
			emit(f, "\t%s\t%s, %s\n", op, regnames[p->z.reg], regnames[p->q2.reg]);
		}
		return 1;
	}
	if ((p->q1.flags & VAR) && (p->q2.flags & VAR) && isauto(p->q2.v->storage_class) && (p->z.flags & REG)) {
		emit(f, "\tLD\t%s, ", regnames[p->z.reg]);
		emitvar(f, p, &p->q1, offset, false, 0);
		emit(f, "\n");

		if ((p->q1.flags & DREFOBJ) && isauto(p->q1.v->storage_class)) {
			emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->z.reg]);
		}

		if (p->q2.flags & REG) {
			if (p->q2.flags & DREFOBJ) {
				emit(f, "\t%s\t%s, (%s)\n", op, regnames[p->z.reg], regnames[p->q2.reg]);
			} else {
				emit(f, "\t%s\t%s, %s\n", op, regnames[p->z.reg], regnames[p->q2.reg]);
			}
		} else {
			emit(f, "\t%s\t%s, (BP+%d)\n", op, regnames[p->z.reg], localslot(offset, p->q2.v->offset));
		}
		return 1;
	}
	if ((p->q1.flags & REG) && (p->z.flags & REG) && (p->q2.flags & KONST)) {
		if (p->q1.flags & DREFOBJ) {
			emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->q1.reg]);
		}
		emit(f, "\t%s\t%s, %s, ", op, regnames[p->z.reg], regnames[p->q1.reg]);
		emitval(f, &p->q2.val, q2typ(p));
		emit(f, "\n");
		return 1;
	}
	if (!strcmp(op, "ADD") && (p->q1.flags & VAR) && !(p->q1.flags & DREFOBJ)
			&& (p->q2.flags & KONST) && (p->z.flags & VAR)) {
		int tmpreg;
		if (p->z.flags & REG) {
			tmpreg = p->z.reg;
		} else {
			tmpreg = cg_getreg(f, p, -1);
		}

		emit(f, "\tLD\t%s, ", regnames[tmpreg]);
		emitval(f, &p->q2.val, q2typ(p));
		emit(f, "\n");

		emit(f, "\t%s\t%s, ", op, regnames[tmpreg]);
		// FIXME the "store" argument to emitvar makes no sense, but I don't understand yet how to
		// differenciate the two cases
		emitvar(f, p, &p->q1, offset, true, 0);
		emit(f, "; PTROP %d\n", p->q1.flags);

		if (!(p->z.flags & REG)) {
			emit(f, "# PTR1\n");
			emit(f, "\tST\t%s, ", regnames[tmpreg]);
			emitvar(f, p, &p->z, offset, true, 0);
			emit(f, "\n");

			cg_freereg(f, tmpreg, p);
		}
		return 1;
	}
	if ((p->q1.flags & VAR)) {
		int tmpreg;
		if (p->z.flags & REG) {
			tmpreg = p->z.reg;
		} else {
			tmpreg = cg_getreg(f, p, -1);
		}

		emit(f, "\tLD\t%s, ", regnames[tmpreg]);
		emitvar(f, p, &p->q1, offset, false, 0);
		emit(f, "\n");

		if ((p->q1.flags & DREFOBJ) && isauto(p->q1.v->storage_class)) {
			emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
		}

		emit(f, "\t%s\t%s, ", op, regnames[tmpreg]);
		if (p->q2.flags & REG) {
			emit(f, "%s", regnames[p->q2.reg]);
		} else {
			emitoperand(f, p, &p->q2, offset, false, 0);
		}
		emit(f, "\n");

		if (!(p->z.flags & REG)) {
			emit(f, "# PTR2\n");
			emit(f, "\tST\t%s, ", regnames[tmpreg]);
			emitvar(f, p, &p->z, offset, true, 0);
			emit(f, "\n");

			cg_freereg(f, tmpreg, p);
		}
		return 1;
	}
	return 0;
}

/*  The main code-generation routine.                   */
/*  f is the stream the code should be written to.      */
/*  p is a pointer to a doubly linked list of ICs       */
/*  containing the function body to generate code for.  */
/*  v is a pointer to the function.                     */
/*  offset is the size of the stackframe the function   */
/*  needs for local variables.                          */
void gen_code(FILE *f,struct IC *p,struct Var *v,zmax offset)
{
	emit(f, "\t.type\t%s%s,@function\n%s%s:\n", idprefix, v->identifier, idprefix, v->identifier);
	if(v->storage_class==EXTERN){
		if((v->flags&(INLINEFUNC|INLINEEXT))!=INLINEFUNC)
			emit(f,"\t.globl\t%s%s\n", idprefix, v->identifier);
	}
	// different prologue and epilogue for interrupts: save all registers, use RETI instead RETF
	if (v->tattr & INTERRUPT) {
		// FIXME we can be smarter about which registers to save, by scanning all the ICs to see
		// what's really used.
		emit(f, "\tPUSH\tR1, BP, (SP)\n"); // Save previous SP
		if (offset > 0) {
			emit(f, "\tSUB\tSP, %d\n", offset); // Make space for local variables
			// There are no parameters, so we need to set BP only if there are local variables
			emit(f, "\tADD\tBP, SP, 1\n\n"); // BP points on first local variable
		 }
	} else {
		// TODO we can avoid saving and restoring BP if: offset is 0 AND there are no parameters
		// There does't seem to be an easy way to know if the function has parameters so we may
		// need to scan ICs for any access to auto variables?
		emit(f, "\tPUSH\tBP, BP, (SP)\n"); // Save previous SP
		if (offset > 0) {
			emit(f, "\tSUB\tSP, %d\n", offset); // Make space for local variables
		}
		emit(f, "\tADD\tBP, SP, 1\n\n"); // BP points on first local variable
	}

	// Start with the default register set
	for (int i = 1; i <= MAXR; i++)
		regs[i] = regsa[i];

	for(; p != NULL; p = p->next) {
		switch (p->code) {
			case ASSIGN:
			{
				// Copy Q1 to Z.
				if ((p->q1.flags & REG) && (p->z.flags & REG)) {
					emit(f, "; ASSIGN %d -> %d\n", p->q1.flags, p->z.flags);
					if (p->z.flags & DREFOBJ) {
						if (p->q1.flags & DREFOBJ) {
							int tmpreg = cg_getreg(f, p, -1);
							emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
							emit(f, "\tST\t%s, (%s)\t; ASSIGN ptr to ptr\n", regnames[tmpreg], regnames[p->z.reg]);
							cg_freereg(f, tmpreg, p);
						} else {
							emit(f, "\tST\t%s, (%s)\t; ASSIGN to ptr\n", regnames[p->q1.reg], regnames[p->z.reg]);
						}
					} else if (p->q1.flags & DREFOBJ) {
						emit(f, "\tLD\t%s, (%s)\t; ASSIGN from ptr\n", regnames[p->z.reg], regnames[p->q1.reg]);
					} else {
						emit(f, "\tLD\t%s, %s\t; ASSIGN T1\n", regnames[p->z.reg], regnames[p->q1.reg]);
					}
					continue;
				}
				if (!(p->q1.flags & KONST)) {
					// Q1 is not constant
					if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ) && (p->q1.flags & VAR) && !(p->q1.flags & REG)) {
						emit(f, "\tLD\t%s, ", regnames[p->z.reg]);
						emitvar(f, p, &p->q1, offset, true, 0);
						emit(f, "\n");
						if ((p->q1.flags & DREFOBJ) && isauto(p->q1.v->storage_class))
							emit(f, "\tLD\t%s, (%s)\n", regnames[p->z.reg], regnames[p->z.reg]);
						continue;
					}

					// REG to whatever: can be done in a simple store instruction
					if ((p->q1.flags & REG) && ((p->z.flags & VAR) || (p->z.flags == (KONST | DREFOBJ)))) {
						if (p->q1.flags & DREFOBJ) {
							int tmpreg = cg_getreg(f, p, -1);
							emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
							emit(f, "\tST\t%s, ", regnames[tmpreg]);
							emitoperand(f, p, &p->z, offset, true, 0);
							emit(f, "\n");
							cg_freereg(f, tmpreg, p);
						} else {
							if (p->z.flags & DREFOBJ) {
								int tmpreg = cg_getreg(f, p, -1);
								emit(f, "\tLD\t%s, ", regnames[tmpreg]);
								emitoperand(f, p, &p->z, offset, true, 0);
								emit(f, "\n");
								emit(f, "\tST\t%s, (%s)\n", regnames[p->q1.reg], regnames[tmpreg]);
								cg_freereg(f, tmpreg, p);
							} else {
								emit(f, "\tST\t%s, ", regnames[p->q1.reg]);
								emitoperand(f, p, &p->z, offset, true, 0);
								emit(f, "\n");
							}
						}
						continue;
					}

					if ((p->q1.flags & REG) || ((p->z.flags & REG) && !(p->z.flags & DREFOBJ))) {
						fprintf(stderr, "error3 q1 %d z %d\n", p->q1.flags, p->z.flags);
						printic(stderr, p);
						break;
					}

					int tmpreg = cg_getreg(f, p, -1);

					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q1, offset, true, 0);
					emit(f, " ; ASSIGN MISC %d %d\n", p->q1.flags, p->z.flags);
					
					if (p->q1.flags & DREFOBJ) {
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
					}

					if (p->z.flags & DREFOBJ) {
						if (p->z.flags & REG) {
							emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[p->z.reg]);
						} else {
							int tmpreg2 = cg_getreg(f, p, tmpreg);
							emit(f, "\tLD\t%s, ", regnames[tmpreg2]);
							emitvar(f, p, &p->z, offset, true, 0);
							emit(f, " ; ASSIGN DREFOBJ\n");
							emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg2]);
							cg_freereg(f, tmpreg2, p);
						}
					} else {
						emit(f, "# MOVE2 %d\n", p->z.flags);
						emit(f, "\tST\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->z, offset, true, 0);
						emit(f, "\n");
					}

					cg_freereg(f, tmpreg, p);

					continue;
				}
				// For all cases below Q1 is a constant
				if ((p->z.flags & ~SCRATCH) == VAR) {
					// Memory variables
					int tmpreg = cg_getreg(f, p, -1);
					if (p->q1.flags & DREFOBJ) {
						emit(f, "\tLD\t%s, (", regnames[tmpreg]);
						emitval(f, &p->q1.val, q1typ(p));
						emit(f, ") ; ASSIGN CONST1\n");
					} else {
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitval(f, &p->q1.val, q1typ(p));
						emit(f, " ; ASSIGN CONST2\n");
					}
					emit(f, "\tST\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->z, offset, true, 0);
					emit(f, "\n");
					cg_freereg(f, tmpreg, p);
					continue;
				}
				if (p->z.flags == (REG|DREFOBJ)) {
					// Constant to memory pointed by register
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]); emitval(f, &p->q1.val, q1typ(p)); emit(f, " ; ASSIGNT6\n");
					emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[p->z.reg]);
					cg_freereg(f, tmpreg, p);
					continue;
				}
				if (p->z.flags & REG) {
					// Constant to register or to variable
					if ((p->z.flags & ~SCRATCH) == (VAR|REG|DREFOBJ)) {
						// FIXME isn't this the same as above?
						// TODO look in the next ICs if there is an ADDI2P on the same pointer,
						// if so, we can use postincrement here and skip the ADDI2P
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, ", regnames[tmpreg]); emitval(f, &p->q1.val, q1typ(p)); emit(f, "\t; ASSIGN T7\n");
						emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[p->z.reg]);
						cg_freereg(f, tmpreg, p);
					} else {
						if (p->q1.flags & DREFOBJ) {
							emit(f, "\tLD\t%s, (", regnames[p->z.reg]);
							emitval(f, &p->q1.val, q1typ(p));
							emit(f, ") ; ASSIGN T7.5\n");
						} else {
							emit(f, "\tLD\t%s, ", regnames[p->z.reg]);
							emitval(f, &p->q1.val, q1typ(p));
							emit(f, " ; ASSIGN TOREG %d\n", p->z.flags);
						}
					}
					continue;
				}
				if (p->z.flags == (VAR|DREFOBJ)) {
					// Constant to external variable
					int tmpreg = cg_getreg(f, p, -1);
					int tmpreg2 = cg_getreg(f, p, tmpreg);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]); emitval(f, &p->q1.val, q1typ(p)); emit(f, " ; ASSIGNT8\n");
					emit(f, "\tLD\t%s, ", regnames[tmpreg2]);
					emitvar(f, p, &p->z, offset, true, 0);
					emit(f, "\n");
					emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg2]);
					cg_freereg(f, tmpreg, p);
					cg_freereg(f, tmpreg2, p);
					continue;
				}
				if ((p->q1.flags == KONST) && (p->z.flags == (KONST | DREFOBJ))) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitval(f, &p->q1.val, q1typ(p));
					emit(f, " ; ASSIGNT9\n");
					emit(f, "\tST\t%s, (", regnames[tmpreg]);
					emitval(f, &p->z.val, ztyp(p));
					emit(f, ")\n");
					cg_freereg(f, tmpreg, p);
					continue;
				}
				printf("ASSIGN q1flags: %d zflags %d\n", p->q1.flags, p->z.flags);
				break;
			}
			case OR:
			{
				if (cg_aluop("OR", f, p, offset))
					continue;
				break;
			}
			case XOR:
			{
				if (cg_aluop("XOR", f, p, offset))
					continue;
				break;
			}
			case AND:
			{
				if (cg_aluop("AND", f, p, offset))
					continue;
				break;
			}
			case LSHIFT:
			{
				if (p->q2.flags & KONST) {
					int shift = p->q2.val.vint;
					// TODO handle all possible shift values
					// TODO check type of Q1 and Z, int and long should be handled differently
					if (shift == 16) {
						if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ)) {
							emit(f, "\tLD\t%s, ", regnames[p->z.reg]);
							if (p->q1.flags & VAR) {
								emitvar(f, p, &p->q1, offset, false, 1);
							} else if (p->q1.flags & KONST) {
								emit(f, "#");
								emitval(f, &p->q1.val, q1typ(p));
							}
							emit(f, "\n");
							continue;
						}

						if (msizetab[ztyp(p) & NQ] > 1) {
							int tmpreg;
							tmpreg = cg_getreg(f, p, -1);
							emit(f, "\tLD\t%s, ", regnames[tmpreg]);
							emitoperand(f, p, &p->q1, offset, false, 0);
							emit(f, "\n");
							emit(f, "\tST\t%s, ", regnames[tmpreg]);
							emitvar(f, p, &p->z, offset, true, 1);
							emit(f, "\n");
							emit(f, "\tLD\t%s, 0\n", regnames[tmpreg]);
							emit(f, "\tST\t%s, ", regnames[tmpreg]);
							emitvar(f, p, &p->z, offset, true, 0);
							emit(f, "\n");
							continue;
						}
					}

					if ((p->q1.flags & REG) && (p->z.flags & REG)) {
						int shift = p->q2.val.vint;
						if ((p->q1.reg == p->z.reg)) {
							while (shift > 4) {
								emit(f, "\tLD\t%s, %s LSL %d\n", regnames[p->z.reg], regnames[p->z.reg], 4);
								shift -= 4;
							}
							emit(f, "\tLD\t%s, %s LSL %d\n", regnames[p->z.reg], regnames[p->z.reg], shift);
						} else {
							emit(f, "\tLD\t%s, %s LSL %d\n", regnames[p->z.reg], regnames[p->q1.reg], shift > 4 ? 4 : shift);
							if (shift > 4) {
								shift -= 4;
								while (shift > 4) {
									emit(f, "\tLD\t%s, %s LSL %d\n", regnames[p->z.reg], regnames[p->z.reg], 4);
									shift -= 4;
								}
								if (shift > 0)
									emit(f, "\tLD\t%s, %s LSL %d\n", regnames[p->z.reg], regnames[p->z.reg], shift);
							}
						}
						continue;
					}

					if (!(p->q1.flags & REG)) {
						int tmpreg;
						if (p->z.flags & REG) {
							tmpreg = p->z.reg;
						} else {
							tmpreg = cg_getreg(f, p, -1);
						}
						int shift = p->q2.val.vint;

						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q1, offset, false, 0);
						emit(f, "\n");

						while (shift > 0) {
							emit(f, "\tLD\t%s, %s LSL %d\n", regnames[tmpreg], regnames[tmpreg], (shift >= 4) ? 4 : shift);
							shift -= 4;
						}

						if (!(p->z.flags & REG)) {
							emit(f, "# LSHIFT\n");
							emit(f, "\tST\t%s, ", regnames[tmpreg]);
							emitvar(f, p, &p->z, offset, true, 0);
							emit(f, "\n");
							cg_freereg(f, tmpreg, p);
						}
						continue;
					}
				}
				break;
			}
			case RSHIFT:
			{
				if ((p->q1.flags & VAR) && (p->q2.flags & KONST) && (p->z.flags & VAR)) {
					if ((p->q1.flags & REG) && (p->z.flags & REG)) {
						int shift = p->q2.val.vint;
						if ((p->q1.reg == p->z.reg)) {
							while (shift > 4) {
								emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->z.reg], regnames[p->z.reg], 4);
								shift -= 4;
							}
							emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->z.reg], regnames[p->z.reg], shift);
						} else {
							emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->q1.reg], regnames[p->z.reg], shift > 4 ? 4 : shift);
							if (shift > 4) {
								shift -= 4;
								while (shift > 4) {
									emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->z.reg], regnames[p->z.reg], 4);
									shift -= 4;
								}
								if (shift > 0)
									emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->z.reg], regnames[p->z.reg], shift);
							}
						}
						continue;
					}

					if ((p->q1.flags & REG) && !(p->z.flags & REG)) {
						// TODO check that there is a FREEREG for Q1 right after, because we are
						// corrupting it. Otherwise, allocate a tmpreg
						int shift = p->q2.val.vint;
						while (shift > 4) {
							emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->q1.reg], regnames[p->q1.reg], 4);
							shift -= 4;
						}
						emit(f, "\tLD\t%s, %s LSR %d\n", regnames[p->q1.reg], regnames[p->q1.reg], shift);
						emit(f, "\tST\t%s, ", regnames[p->q1.reg]);
						emitvar(f, p, &p->z, offset, true, 0);
						emit(f, "\n");
						continue;
					}


					if (!(p->q1.flags & REG)) {
						int tmpreg;
						if (p->z.flags & REG) {
							tmpreg = p->z.reg;
						} else {
							tmpreg = cg_getreg(f, p, -1);
						}
						int shift = p->q2.val.vint;
						
						if ((ztyp(p) & NQ) >= LONG) {
							struct obj* source = &p->q1;
							// 32bit shifts. 32bit values are not allowed to be in registers, which
							// simplifies this a little
							if (shift >= 16) {
								// Special case for the common operation shifting to get the high
								// byte of a long value
								// TODO is this useful also for shifts larger than 16? Then we can
								// finish the shifting with just the low byte (the high word will
								// be all 0)
								emit(f, "\tLD\t%s, ", regnames[tmpreg]);
								emitvar(f, p, source, offset, false, 1);
								emit(f, "\n");
								emit(f, "\tST\t%s, ", regnames[tmpreg]);
								emitvar(f, p, &p->z, offset, true, 0);
								emit(f, "\n");
								emit(f, "\tLD\t%s, 0\n", regnames[tmpreg]);
								emit(f, "\tST\t%s, ", regnames[tmpreg]);
								emitvar(f, p, &p->z, offset, true, 1);
								emit(f, "\n");
								shift -= 16;
								source = &p->z;
							}

							while (shift > 0) {
								// TODO this can be done more efficiently if we allocate two
								// temporary registers for the high and low bytes. There would be
								// a lot less load-store, especially for shifts greater than 4.

								// Shift the high word first
								emit(f, "\tLD\t%s, ", regnames[tmpreg]);
								emitvar(f, p, source, offset, false, 1);
								emit(f, "\n");
								emit(f, "\tLD\t%s, %s LSR %d\n", regnames[tmpreg], regnames[tmpreg], (shift >= 4) ? 4 : shift);
								emit(f, "\tST\t%s, ", regnames[tmpreg]);
								emitvar(f, p, &p->z, offset, true, 1);
								emit(f, "\n");

								// Then do the low word. The ROR addressing mode gets the bits we
								// just removed from the high bit, using the shift buffer
								emit(f, "\tLD\t%s, ", regnames[tmpreg]);
								emitvar(f, p, source, offset, false, 0);
								emit(f, "\n");
								emit(f, "\tLD\t%s, %s ROR %d\n", regnames[tmpreg], regnames[tmpreg], (shift >= 4) ? 4 : shift);
								emit(f, "\tST\t%s, ", regnames[tmpreg]);
								emitvar(f, p, &p->z, offset, true, 0);
								emit(f, "\n");

								shift -= 4;
								source = &p->z;
							}
							cg_freereg(f, tmpreg, p);
							continue;
						} else {
							emit(f, "\tLD\t%s, ", regnames[tmpreg]);
							emitvar(f, p, &p->q1, offset, false, 0);
							emit(f, "\n");

							while (shift > 0) {
								emit(f, "\tLD\t%s, %s LSR %d\n", regnames[tmpreg], regnames[tmpreg], (shift >= 4) ? 4 : shift);
								shift -= 4;
							}

							if (!(p->z.flags & REG)) {
								emit(f, "# RSHIFT\n");
								emit(f, "\tST\t%s, ", regnames[tmpreg]);
								emitvar(f, p, &p->z, offset, true, 0);
								emit(f, "\n");
								cg_freereg(f, tmpreg, p);
							}
							continue;
						}
					}
				}
				break;
			}
			case ADD:
			{
				if (cg_aluop("ADD", f, p, offset))
					continue;
				break;
			}
			case SUB:
			{
				if (cg_aluop("SUB", f, p, offset))
					continue;
				break;
			}
			case MULT:
			{
				// FIXME properly handle long-to-long multiplication (do the 3 crossed multiplications and add them up)
				if ((p->q2.flags & KONST) && (p->z.flags & VAR) && isauto(p->z.v->storage_class)) {
					int mult = p->q2.val.vuint;
					long pof2 = get_pof2(p->q2.val.vuint);
					if (pof2 != 0) {
						pof2--;
						// Special case for power-of-two multiplications, use shift operations
						// FIXME adjust the code to work for 2, 8, 16, 128, ... as well
						// FIXME check instructions timing, at which point is it faster to use a multiplication? (including the register transfers)
						int tmpreg;
						if (p->z.flags & REG)
							tmpreg = p->z.reg;
						else
							tmpreg = cg_getreg(f, p, -1);
						// TODO if p->q1 is REG, we can just push it and reuse the same register
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q1, offset, false, 0);
						emit(f, "\n");
						while (pof2 > 4) {
							emit(f, "\tLD\t%s, %s LSL 4\n", regnames[tmpreg], regnames[tmpreg]);
							pof2 -= 4;
						}
						emit(f, "\tLD\t%s, %s LSL %ld\n", regnames[tmpreg], regnames[tmpreg], pof2);

						if (!(p->z.flags & REG)) {
							emit(f, "# MULT64\n");
							emit(f, "\tST\t%s, (BP+%d)\n", regnames[tmpreg], localslot(offset, p->z.v->offset));
							cg_freereg(f, tmpreg, p);
						}
						continue;
					}

					// FIXME make sure the source or dest isn't already in R3 or R4
					emit(f, "\tPUSH R3,R4,\t(SP)\n"); // FIXME check if R3 and R4 are allocated
					emit(f, "\tLD\tR3, ");
					emitvar(f, p, &p->q1, offset, false, 0);
					emit(f, "\n");
					emit(f, "\tLD\tR4, ");
					emitval(f, &p->q2.val, q2typ(p));
					emit(f, "\n");
					emit(f, "\tMUL\tR3, R4\n");
					if (p->z.flags & REG) {
						emit(f, "\tLD\t%s, R3\n", regnames[p->z.reg]);
					} else {
						break;
					}
					emit(f, "\tPOP R3,R4,\t(SP)\n"); // FIXME check if R3 and R4 are allocated
					continue;
				}

				// (REG|VAR|SCRATCH * VAR|SCRATCH -> REG|VAR|SCRATCH (Q1 and Z samereg)
				if (((p->q1.flags & ~(REG|SCRATCH)) == VAR) && ((p->q2.flags & ~SCRATCH) == VAR))
				{
					// FIXME make sure the source or dest isn't already in R3 or R4
					emit(f, "\tPUSH R3,R4,\t(SP)\n"); // FIXME check if R3 and R4 are allocated
					emit(f, "\tLD\tR3, ");
					emitvar(f, p, &p->q1, offset, false, 0);
					emit(f, "\n");
					emit(f, "\tLD\tR4, ");
					emitvar(f, p, &p->q2, offset, false, 0);
					emit(f, "\n");
					emit(f, "\tMUL\tR3, R4\n");
					if (p->z.flags & REG)
						emit(f, "\tLD\t%s, R3\n", regnames[p->z.reg]);
					else {
						emit(f, "\tST\tR3, ");
						emitvar(f, p, &p->z, offset, true, 0);
						emit(f, "\n");
					}
					emit(f, "\tPOP R3,R4,\t(SP)\n"); // FIXME check if R3 and R4 are allocated
					continue;
				}

				emit(f, "# ??MUL %d %d -> %d\n", p->q1.flags, p->q2.flags, p->z.flags);
				break;
			}
			case MINUS:
			{
				if (p->z.flags & REG) {
					emit(f, "\tNEG %s, ", regnames[p->z.reg]);
					emitvar(f, p, &p->q1, offset, false, 0);
					emit(f, "\n");
					continue;
				}
			}
			case ADDRESS:
			{
				// FIXME check if q1 is a local variable, otherwise we need completely different code
				int tmpreg;
				if (p->z.flags & REG) {
					tmpreg = p->z.reg;
					emit(f, "\tADD\t%s, BP, %d\n", regnames[tmpreg], localslot(offset, p->q1.v->offset));
				} else {
					tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tADD\t%s, BP, %d\n", regnames[tmpreg], localslot(offset, p->q1.v->offset));
					if ((p->z.flags & VAR) && isauto(p->z.v->storage_class)) {
						emit(f, "# ADDRESS\n");
						emit(f, "\tST\t%s, (BP+%d)\n", regnames[tmpreg], localslot(offset, p->z.v->offset));
					} else {
						printf("flags: %x class %x\n", p->z.flags, p->z.v->storage_class);
						ierror(0);
					}
					cg_freereg(f, tmpreg, p);
				}
				continue;
			}
			case CALL:
			{
				if ((p->q1.flags & (VAR|DREFOBJ)) == VAR && p->q1.v->fi && p->q1.v->fi->inline_asm) {
					emit_inline_asm(f, p->q1.v->fi->inline_asm);
				} else {
					if (p->q1.flags & REG) {
						emit(f, "\tCALL\t__indirect_%s\n", regnames[p->q1.reg]);
					} else if (p->q1.flags == (VAR|DREFOBJ|SCRATCH)) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q1, offset, false, 0);
						emit(f, "\n");
						emit(f, "\tCALL\t__indirect_%s\n", regnames[tmpreg]);
						cg_freereg(f, tmpreg, p);
					} else {
						emit(f, "\tCALL\t_%s ; Q1: %d\n", p->q1.v->identifier, p->q1.flags);
					}

					// POP parameters if needed
					if (!zmeqto(l2zm(0L), opsize(p))) {
						emit(f, "\tADD\tSP, ");
						emitval(f, &p->q2.val, INT);
						emit(f, "\n");
					}
				}
				continue;
			}
			case CONVERT:
			{
				if ((p->q1.flags & REG) && (p->z.flags & REG) && (p->z.flags & DREFOBJ)) {
					if (p->q1.flags & DREFOBJ) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
						emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[p->z.reg]);
						cg_freereg(f, tmpreg, p);
					} else {
						emit(f, "\tST\t%s, (%s)\n", regnames[p->q1.reg], regnames[p->z.reg]);
					}
					continue;
				}

				bool allocreg = false;
				bool store = true;
				if ((p->q1.flags & VAR)) {
					int tmpreg;
					if ((p->q1.flags & REG) && !(p->q1.flags & DREFOBJ)) {
						tmpreg = p->q1.reg;

						if ((p->z.flags & ~SCRATCH) == (REG | VAR)) {
							store = false;
							emit(f, "\tLD\t%s, %s\n", regnames[p->z.reg], regnames[p->q1.reg]);
						}
					} else {
						if ((p->z.flags & REG) && !(p->z.flags & DREFOBJ)) {
							tmpreg = p->z.reg;
							store = false;
						} else {
							tmpreg = cg_getreg(f, p, -1);
							allocreg = true;
						}
						
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitoperand(f, p, &p->q1, offset, false, 0);
						emit(f, "; CONVERT TD %d\n", p->z.flags);
						
						if ((p->q1.flags & (REG | VAR | DREFOBJ)) == (VAR | DREFOBJ)) {
							emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
						}
					}

					if (store) {
						if ((p->z.flags & (VAR|REG|DREFOBJ)) == (VAR|REG|DREFOBJ)) {
							emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[p->z.reg]);
						} else if ((p->z.flags & (VAR|DREFOBJ)) == (VAR|DREFOBJ)) {
							int tmpreg2 = cg_getreg(f, p, tmpreg);
							emit(f, "\tLD\t%s, ", regnames[tmpreg2]);
							emitoperand(f, p, &p->z, 0, false, 0);
							emit(f, "\n");
							emit(f, "\tST\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg2]);
							cg_freereg(f, tmpreg2, p);
						} else {
							emit(f, "\tST\t%s, ", regnames[tmpreg]);
							// TODO use emitoperand?
							if (p->z.flags & VAR) {
								emitvar(f, p, &p->z, offset, true, 0);
								emit(f, " ; CONVERT T2 %d %d\n", p->q1.flags, p->z.flags);
							} else if (p->z.flags & (KONST | DREFOBJ)) {
								emit(f, "(");
								emitval(f, &p->z.val, ztyp(p));
								emit(f, ") ; CONVERT T3\n");
							}
						}
					}

					if (allocreg) {
						cg_freereg(f, tmpreg, p);
					}
					continue;
				}

				if ((p->q1.flags == KONST|DREFOBJ) && (p->z.flags & ~SCRATCH == REG|VAR)) {
					emit(f, "\tLD\t%s, (", regnames[p->z.reg]);
					emitval(f, &p->q1.val, ztyp(p));
					emit(f, ") # CONVERT T4\n");
					continue;
				}

				fprintf(stderr, "MISSING IMPL CONVERT %d %d\n", p->q1.flags, p->z.flags);
				break;
			}
			case ALLOCREG:
			{
				// TODO sometimes vbcc does an allocreg, some operations that don't use the register
				// then a freereg (I guess as a consequence of optimizations).
				// Since we don't have a lot of registers, it's wise to clean that up...
				int reallyUsed = 0;
				for (struct IC* p2 = p->next; p2 != NULL; p2 = p2->next) {
					if (p2->code == FREEREG) {
						if (p2->q1.reg == p->q1.reg)
							break;
					} else {
						if ((p2->z.flags & REG) && (p2->z.reg == p->q1.reg)) {
							reallyUsed = 1;
							break;
						}
						if ((p2->q1.flags & REG) && (p2->q1.reg == p->q1.reg)) {
							reallyUsed = 1;
							break;
						}
						if ((p2->q2.flags & REG) && (p2->q2.reg == p->q1.reg)) {
							reallyUsed = 1;
							break;
						}
					}
				}

				// remember that the reg is in use
				if (reallyUsed)
					regs[p->q1.reg] = 1;
				else
					emit(f, "\t# Fake allocreg %s\n", regnames[p->q1.reg]);
				continue;
			}
			case FREEREG:
			{
				// remember that the reg is available
				regs[p->q1.reg] = 0;
				continue;
			}
			case COMPARE:
			{
				emit(f, "; CMP %d %d\n", p->q1.flags, p->q2.flags);
				if (p->q2.flags & KONST) {
					if (p->q1.flags & REG) {
						if (p->q1.flags & DREFOBJ) {
							int tmpreg = cg_getreg(f, p, -1);
							emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
							emit(f, "\tCMP\t%s, ", regnames[tmpreg]);
							emitval(f, &p->q2.val, q2typ(p));
							emit(f, "\n");
							cg_freereg(f, tmpreg, p);
						} else {
							emit(f, "\tCMP\t%s, ", regnames[p->q1.reg]);
							emitval(f, &p->q2.val, q2typ(p));
							emit(f, "\n");
						}
					} else if (p->q1.flags & VAR) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q1, offset, false, 0);
						emit(f, "\n");
						emit(f, "\tCMP\t%s, ", regnames[tmpreg]);
						emitval(f, &p->q2.val, q2typ(p));
						emit(f, "\n");
						cg_freereg(f, tmpreg, p);
					} else {
						printic(stderr, p);
						ierror(0);
						break;
					}
					continue;
				}


				if (((p->q1.flags & ~SCRATCH) == VAR) && ((p->q2.flags & ~SCRATCH) == (VAR|REG))) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q1, offset, false, 0);
					emit(f, "\n");
					emit(f, "\tCMP\t%s, %s\n", regnames[tmpreg], regnames[p->q2.reg]);
					cg_freereg(f, tmpreg, p);
					continue;
				}

				if (((p->q1.flags & ~(VARADR|DREFOBJ|SCRATCH)) == VAR) && (p->q2.flags & VAR)) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q1, offset, true, 0);
					emit(f, "\n");
					if (p->q1.flags & DREFOBJ) {
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
					}

					if ((p->q2.flags & DREFOBJ) && !(p->q2.flags & REG) && isauto(p->q1.v->storage_class)) {
						int tmpreg2 = cg_getreg(f, p, tmpreg);

						emit(f, "\tLD\t%s, ", regnames[tmpreg2]);
						emitvar(f, p, &p->q2, offset, false, 0);
						emit(f, "\n");

						emit(f, "\tCMP\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg2]);

						cg_freereg(f, tmpreg2, p);
					} else {
						emit(f, "\tCMP\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q2, offset, true, 0);
						emit(f, "\n");
					}
					cg_freereg(f, tmpreg, p);
					continue;
				}

				if ((p->q1.flags & REG) && (p->q1.flags & DREFOBJ) && (p->q2.flags & VAR)) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
					emit(f, "\tCMP\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q2, offset, false, 0);
					emit(f, "\n");
					cg_freereg(f, tmpreg, p);
					continue;
				}

				if ((p->q1.flags & REG) && (p->q2.flags & REG)) {
					if (p->q2.flags & DREFOBJ) {
						emit(f, "\tCMP\t%s, (%s)\n", regnames[p->q1.reg], regnames[p->q2.reg]);
					} else {
						emit(f, "\tCMP\t%s, %s\n", regnames[p->q1.reg], regnames[p->q2.reg]);
					}
					continue;
				}

				if (p->q1.flags & REG) {
					if (p->q2.flags & DREFOBJ) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, ", regnames[tmpreg]);
						emitvar(f, p, &p->q2, offset, false, 0);
						emit(f, "\n");
						emit(f, "\tCMP\t%s, (%s)\n", regnames[p->q1.reg], regnames[tmpreg]);
						cg_freereg(f, tmpreg, p);

					} else {
						emit(f, "\tCMP\t%s, ", regnames[p->q1.reg]);
						emitvar(f, p, &p->q2, offset, true, 0);
						emit(f, "\n");
					}
					continue;
				}

				printic(stderr, p);
				ierror(0);
				break;
			}
			case TEST:
			{
				// TODO check if a previous IC already set the flags for the same register
				// (for example we just loaded it with LD). In that case, emit no instruction.
				if (p->q1.flags & REG) {
					if (p->q1.flags & DREFOBJ) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
						// LD already sets the Z flag, no need to do an explicit compare here
						//emit(f, "\tCMP\t%s, 0\n", regnames[tmpreg]);
						cg_freereg(f, tmpreg, p);
					} else {
						emit(f, "\tCMP\t%s, 0\n", regnames[p->q1.reg]);
					}
					continue;
				}
				if (p->q1.flags & VAR) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q1, offset, true, 0);
					emit(f, "\n");
					if (p->q1.flags & DREFOBJ) {
						// Dereference the pointer
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
					}
					// LD already sets the Z flag, no need to do an explicit compare here
					//emit(f, "\tCMP\t%s, 0\n", regnames[tmpreg]);
					cg_freereg(f, tmpreg, p);
					continue;
				}
				break;
			}
			case LABEL:
			{
				emit(f, "f%d:\n", p->typf);
				continue;
			}
			case BEQ:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tLJE\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BNE:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tLJNE\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BLT:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tLJB\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BGE:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tLJGE\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BLE:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tJLE\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BGT:
			{
				if (p->q1.flags == 0) {
					emit(f, "\tLJG\tf%d\n", iclabel(p));
					continue;
				}
				break;
			}
			case BRA:
			{
				// TODO decide if the target is close enough that we can use JMP (but really that's
				// a job for the assembler)
				emit(f, "\tGOTO\tf%d\n", iclabel(p));
				continue;
			}
			case PUSH:
			{
				if ((p->q1.flags & REG)) {
					if ((p->q1.flags & DREFOBJ)) {
						int tmpreg = cg_getreg(f, p, -1);
						emit(f, "\tLD\t%s, (%s)\n", regnames[tmpreg], regnames[p->q1.reg]);
						emit(f, "\tPUSH\t%s, %s, (SP)\n", regnames[tmpreg], regnames[tmpreg]);
						cg_freereg(f, tmpreg, p);
					} else
						emit(f, "\tPUSH\t%s, %s, (SP)\n", regnames[p->q1.reg], regnames[p->q1.reg]);
					continue;
				}

				if ((p->q1.flags & VAR)) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, ", regnames[tmpreg]);
					emitvar(f, p, &p->q1, offset, true, 0);
					emit(f, "\n");
					if ((p->q1.flags & DREFOBJ) && isauto(p->q1.v->storage_class)) {
						emit(f, "LD\t%s, (%s)\n", regnames[tmpreg], regnames[tmpreg]);
					}
					emit(f, "\tPUSH\t%s, %s, (SP)\n", regnames[tmpreg], regnames[tmpreg]);
					cg_freereg(f, tmpreg, p);
					continue;
				}

				if ((p->q1.flags & KONST)) {
					int tmpreg = cg_getreg(f, p, -1);
					emit(f, "\tLD\t%s, %d\n", regnames[tmpreg], p->q1.val.vint & 0xFFFF);
					emit(f, "\tPUSH\t%s, %s, (SP)\n", regnames[tmpreg], regnames[tmpreg]);
					if (zmeqto(l2zm(2L), opsize(p))) {
						emit(f, "\tLD\t%s, %d\n", regnames[tmpreg], p->q1.val.vint >> 16);
						emit(f, "\tPUSH\t%s, %s, (SP)\n", regnames[tmpreg], regnames[tmpreg]);
					}
					cg_freereg(f, tmpreg, p);
					continue;
				}
				break;
			}
			case ADDI2P:
			{
				if (cg_ptrop("ADD", f, p, offset))
					continue;
				break;
			}
			case SUBIFP:
			{
				if (cg_ptrop("SUB", f, p, offset))
					continue;
				break;
			}
			case SETRETURN:
			{
				if (!(p->q1.flags & REG)) {
					emit(f, "\tLD\tR1, ");
					emitoperand(f, p, &p->q1, offset, true, 0);
					emit(f, "\n");
					continue;
				}

				if (p->q1.reg != 2) {
					emit(f, "\tLD\tR1, %s\n", regnames[p->q1.reg]);
					continue;
				}
				// Result is already in R1
				continue;
			}
			case GETRETURN:
			{
				if (p->z.flags & REG) {
					if (p->z.reg != 2) {
						// Transfer return code from R1 into another register
						emit(f, "\tLD\t%s, R1\n", regnames[p->z.reg]);
					} else {
						// Return code is already in R1, nothing to do
					}
					continue;
				} else {
					// Transfer return code from R1 into memory
					if (p->z.flags & VAR) {
						emit(f, "; GETRETURN\n");
						emit(f, "\tST\tR1, ");
						emitvar(f, p, &p->z, offset, true, 0);
						emit(f, "\n");
						continue;
					}
				}
				break;
			}
			case NOP:
			{
				continue;
			}
		}
		emit(f, "*** UNHANDLED IC: %d\n", p->code);
		if (f)
			printic(f, p);
	}

	emit(f, "\n");
	if (v->tattr & INTERRUPT) {
		if (offset > 0) {
			emit(f, "\tADD SP, %d\n", offset); // Free stack space used by local variables
		}
		emit(f, "\tPOP R1, BP, (SP)\n");
		emit(f, "\tRETI\n"); // Exit function
	} else {
		if (offset > 0) {
			emit(f, "\tADD SP, %d\n", offset); // Free stack space used by local variables
		}
		emit(f, "\tPOP BP, BP, (SP)\n"); // Restore caller BP
		emit(f, "\tRETF\n\n"); // Exit function
	}
}
