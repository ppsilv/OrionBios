/*  JRISC (Atari Jaguar RISC) for vbcc

*/

#include "supp.h"

static char FILE_[]=__FILE__;

/*  Public data that MUST be there.                             */

/* Name and copyright. */
char cg_copyright[]="vbcc generic code-generator V0.1b (c) in 2001 by Volker Barthelmann";

/*  Commandline-flags the code-generator accepts:
    0: just a flag
    VALFLAG: a value must be specified
    STRINGFLAG: a string can be specified
    FUNCFLAG: a function will be called
    apart from FUNCFLAG, all other versions can only be specified once */
int g_flags[MAXGF]={VALFLAG,VALFLAG,
		    VALFLAG,VALFLAG,0,
		    0,0,
		    VALFLAG,VALFLAG,0,
		    0,VALFLAG,0};

/* the flag-name, do not use names beginning with l, L, I, D or U, because
   they collide with the frontend */
char *g_flags_name[MAXGF]={"sp","fp",
			   "volatile-gprs","volatile-fprs","gpumain",
			   "gpulocal","dspmain",
			   "gpr-args","fpr-args","dsplocal",
			   "no-delayed-popping","workaround","divbug"};

/* the results of parsing the command-line-flags will be stored here */
union ppi g_flags_val[MAXGF];

/*  Alignment-requirements for all types in bytes.              */
zmax align[MAX_TYPE+1];

/*  Alignment that is sufficient for every object.              */
zmax maxalign;

/*  CHAR_BIT for the target machine.                            */
zmax char_bit;

/*  sizes of the basic types (in bytes) */
zmax sizetab[MAX_TYPE+1];

/*  Minimum and Maximum values each type can have.              */
/*  Must be initialized in init_cg().                           */
zmax t_min[MAX_TYPE+1];
zumax t_max[MAX_TYPE+1];
zumax tu_max[MAX_TYPE+1];

/*  Names of all registers. will be initialized in init_cg(),
    register number 0 is invalid, valid registers start at 1 */
char *regnames[MAXR+1]= {"noreg",
  "r0","r1","r2","r3","r4","r5","r6","r7",
  "r8","r9","r10","r11","r12","r13","r14","r15",
  "r16","r17","r18","r19","r20","r21","r22","r23",
  "r24","r25","r26","r27","r28","r29","r30","r31",
  "R0","R1","R2","R3","R4","R5","R6","R7",
  "R8","R9","R10","R11","R12","R13","R14","R15",
  "R16","R17","R18","R19","R20","R21","R22","R23",
  "R24","R25","R26","R27","R28","R29","R30","R31",
  "r0/r1","r2/r3","r4/r5","r6/r7",
  "r8/r9","r10/r11","r12/r13","r14/r15",
  "r16/r17","r18/r19","r20/r21","r22/r23",
  "r24/r25","r26/r27","r28/r29","r30/r31",
  "R0/R1","R2/R3","R4/R5","R6/R7",
  "R8/R9","R10/R11","R12/R13","R14/R15",
  "R16/R17","R18/R19","R20/R21","R22/R23",
  "R24/R25","R26/R27","R28/R29","R30/R31",
};

/*  The Size of each register in bytes.                         */
zmax regsize[MAXR+1];

/*  a type which can store each register. */
struct Typ *regtype[MAXR+1];

/*  regsa[reg]!=0 if a certain register is allocated and should */
/*  not be used by the compiler pass.                           */
int regsa[MAXR+1];

/*  Specifies which registers may be scratched by functions.    */
int regscratch[MAXR+1];

/* specifies the priority for the register-allocator, if the same
   estimated cost-saving can be obtained by several registers, the
   one with the highest priority will be used */
int reg_prio[MAXR+1];

/* an empty reg-handle representing initial state */
struct reg_handle empty_reg_handle={0};

/* Names of target-specific variable attributes.                */
char *g_attr_name[]={"__interrupt","__68k","__gpumain","__gpulocal","__dspmain","__dsplocal",0};


/****************************************/
/*  Private data and functions.         */
/****************************************/

#define INTERRUPT   1
#define M_68K       2
#define M_GPUMAIN   4
#define M_GPULOCAL  8
#define M_DSPMAIN  16
#define M_DSPLOCAL 32


#define REG_SP     ((g_flags[0]&USEDFLAG)?g_flags_val[0].l:27)
#define REG_FP     ((g_flags[1]&USEDFLAG)?g_flags_val[1].l:26)
#define VOL_GPRS   ((g_flags[2]&USEDFLAG)?g_flags_val[2].l:NUM_GPRS/4-1)
#define VOL_FPRS   ((g_flags[3]&USEDFLAG)?g_flags_val[3].l:NUM_FPRS/2)
#define GPUMAIN    (g_flags[4]&USEDFLAG)
#define GPULOCAL   (g_flags[5]&USEDFLAG)
#define DSPMAIN    (g_flags[6]&USEDFLAG)
#define GPR_ARGS   ((g_flags[7]&USEDFLAG)?g_flags_val[7].l:8)
#define FPR_ARGS   ((g_flags[8]&USEDFLAG)?g_flags_val[8].l:0)
#define DSPLOCAL   (g_flags[9]&USEDFLAG)
#define NODELAYEDPOP (g_flags[10]&USEDFLAG)
#define WORKAROUND ((g_flags[11]&USEDFLAG)?g_flags_val[11].l:-1)
#define DIVBUG     (g_flags[12]&USEDFLAG)


#define ISIDX(r) ((r)==15||(r)==16)
#define ISSHADOW(r) (regnames[r][0]=='R')
#define isreg(x) ((p->x.flags&(REG|DREFOBJ))==REG)

#define LALIGNSTR "\t.p2alignw 2,0xe400\n"
#define LALIGN() emit(f,LALIGNSTR)

/* alignment of basic data-types, used to initialize align[] */
static long malign[MAX_TYPE+1]=  {1,1,2,4,4,4,4,8,8,1,4,1,1,1,4,1};
/* sizes of basic data-types, used to initialize sizetab[] */
static long msizetab[MAX_TYPE+1]={1,1,2,4,4,8,4,8,8,0,4,0,0,0,4,0};

/* used to initialize regtyp[] */
static struct Typ ltyp={LONG},lltyp={LLONG};

/* macros defined by the backend */
static char *marray[]={"__section(x)=__vattr(\"section(\"#x\")\")",
		       "__bank(x)=__vattr(\"bank(\"#x\")\")",
		       "__JRISC__",
		       0};

/* special registers */
static int sp,fp,lr;               /*  Stackpointer                        */
static int t1,t2,t3;               /*  temporary gprs */
static int f1,f2,f3;               /*  temporary fprs */
static int fpsave,fpmerk;

static struct rpair rp2;

static int dsp=0;
static int cmode,dmode,cbank,workaround,flabel;

static char *dct[]={"","byte","2byte","4byte","4byte","4byte","4byte","4byte","4byte"};
static char *lst[]={"err","b","w","","","","","","","",""};
static char *lss[]={"err","b","w","err","","err","err","err","w","err"};

/* sections */
#define DATA 0
#define BSS 1
#define CODE 2
#define RODATA 3
#define SPECIAL 4

static long stack;
static int stack_valid;
static int section=-1;
#if 1
static char *codename="\t.text\n",
  *dataname="\t.data\n",
  *bssname="\t.bss\n",
  *rodataname="\t.section\t.rodata\n";
#else
static char *codename="\t.text\n",
  *dataname="\t.data\n",
  *bssname="",
  *rodataname="\t.section\t.rodata\n";
#endif

/* return-instruction */
static char *ret;

/* label at the end of the function (if any) */
static int exit_label;

/* assembly-prefixes for labels and external identifiers */
static char *labprefix="l",*idprefix="_";

#if FIXED_SP
/* variables to calculate the size and partitioning of the stack-frame
   in the case of FIXED_SP */
static long frameoffset,pushed,maxpushed,framesize;
#else
/* variables to keep track of the current stack-offset in the case of
   a moving stack-pointer */
static long notpopped,dontpop,stackoffset,maxpushed;
#endif

static long localsize,rsavesize,argsize;

static void emit_obj(FILE *f,struct obj *p,int t);

/* calculate the actual current offset of an object relativ to the
   stack-pointer; we use a layout like this:
   ------------------------------------------------
   | arguments to this function                   |
   ------------------------------------------------
   | return-address [size=4]                      |
   ------------------------------------------------
   | caller-save registers [size=rsavesize]       |
   ------------------------------------------------
   | local variables [size=localsize]             |
   ------------------------------------------------
   | arguments to called functions [size=argsize] |
   ------------------------------------------------
   All sizes will be aligned as necessary.
   In the case of FIXED_SP, the stack-pointer will be adjusted at
   function-entry to leave enough space for the arguments and have it
   aligned to 16 bytes. Therefore, when calling a function, the
   stack-pointer is always aligned to 16 bytes.
   For a moving stack-pointer, the stack-pointer will usually point
   to the bottom of the area for local variables, but will move while
   arguments are put on the stack.

   This is just an example layout. Other layouts are also possible.
*/

static long real_offset(struct obj *o)
{
  long off=zm2l(o->v->offset);
  if(off<0){
    /* function parameter */
    off=localsize-off-zm2l(maxalign);
  }

  if(fp==fpmerk)
    off-=stackoffset;

  off+=zm2l(o->val.vmax);
  return off;
}

static int direct_stack_var(obj *o,int t)
{
  long of;
  if(o->v->storage_class!=AUTO&&o->v->storage_class!=REGISTER) return 0;
  of=real_offset(o);
  if(of>128) return 0;
  if(of==0) return 1;
  if(!ISIDX(fp)) return 0;
  if(zm2l(sizetab[t&NQ])!=4) return 0;
  return 1;
}


#if !FIXED_SP
static void push(long l)
{
  stackoffset-=l;
  if(stackoffset<maxpushed) maxpushed=stackoffset;
}
static void pop(long l)
{
  stackoffset+=l;
}
#endif

/*  Initializes an addressing-mode structure and returns a pointer to
    that object. Will not survive a second call! */
static struct obj *cam(int flags,int base,long offset)
{
  static struct obj obj;
  static struct AddressingMode am;
  obj.am=&am;
  am.flags=flags;
  am.base=base;
  am.offset=offset;
  return &obj;
}

static int bank(struct Var *v)
{
  char *s=v->vattr;
  if(s&&(s=strstr(s,"bank("))){
    int n;
    if(sscanf(s+5,"%i",&n)==1)
      return n;
  }
  return 0;
}

static int mmode(struct Var *v)
{
  if(v->tattr&M_GPUMAIN) return M_GPUMAIN;
  if(v->tattr&M_GPULOCAL) return M_GPULOCAL;
  if(v->tattr&M_DSPMAIN) return M_DSPMAIN;
  if(v->tattr&M_DSPLOCAL) return M_DSPLOCAL;
  if(ISFUNC(v->vtyp->flags)) return dmode;
  return 0;
}

/* changes to a special section, used for __section() */
static int special_section(FILE *f,struct Var *v)
{
  char *sec;int mode,banknr;
  if(v->vattr){
    sec=strstr(v->vattr,"section(");
    if(sec){
      sec+=strlen("section(");
      emit(f,"\t.section\t");
      while(*sec&&*sec!=')') emit_char(f,*sec++);
      emit(f,"\n");
      if(f) section=SPECIAL;
      return 1;
    }
  }
  mode=mmode(v);
  if(mode==M_GPULOCAL){
    if(banknr=bank(v))
      emit(f,"\t.section\t\".gpubank%d\",\"acrwx\"\n",banknr);
    else
      emit(f,"\t.section\t\".gpu\",\"acrwx\"\n");
    if(f) section=SPECIAL;
    return 1;
  }
  if(mode==M_DSPLOCAL){
    if(banknr=bank(v))
      emit(f,"\t.section\t\".dspbank%d\",\"acrwx\"\n",banknr);
    else
      emit(f,"\t.section\t\".dsp\",\"acrwx\"\n");
    if(f) section=SPECIAL;
    return 1;
  }
  return 0;
}

#define chk_coll(x) do{if((x)==r||(x)==r1||(x)==r2) return 0;   \
    if(reg_pair((x),&rp)&&(rp.r1==r||rp.r2==r)) return 0;}while(0)

static int scratch(IC *p,int r,int isnext)
{
  int r1,r2;
  if(!p) return 1;
  if(reg_pair(r,&rp)){
    r1=rp.r1;
    r2=rp.r2;
  }else{
    r1=0;
    r2=0;
  }
  if(!isnext&&isreg(z)&&p->z.reg==r){
    if(!(p->q2.flags&REG))
      return 1;
    if(p->q2.reg==r||p->q2.reg==r1||p->q2.reg==r2)
      return 0;
    if(reg_pair(p->q2.reg,&rp))
      if(rp.r1==r||rp.r2==r)
        return 0;
    return 1;
  }
  if(r==t1||r==t2) return 1;
  while(p){
    if(p->code==LABEL||p->code==CALL)
      return 0;
    if(p->code>=BEQ&&p->code<=BRA)
      return 0;
    if(p->code==FREEREG||p->code==ALLOCREG){
      if(p->q1.reg==r)
        return 1;
      if(reg_pair(p->q1.reg,&rp)&&(rp.r1==r||rp.r2==r))
        return 1;
    }
    if(p->q1.am){
      chk_coll(p->q1.am->base);
      if(p->q1.am->flags!=IMM_IND) chk_coll(p->q1.am->idx);
    }
    if(p->q2.am){
      chk_coll(p->q2.am->base);
      if(p->q2.am->flags!=IMM_IND) chk_coll(p->q2.am->idx);
    }
    if(p->z.am){
      chk_coll(p->z.am->base);
      if(p->z.am->flags!=IMM_IND) chk_coll(p->z.am->idx);
    }
    if(p->q1.flags&REG) chk_coll(p->q1.reg);
    if(p->q2.flags&REG) chk_coll(p->q2.reg);
    if(p->z.flags&REG){
      if(p->z.flags&DREFOBJ)
        chk_coll(p->z.reg);
      else{
        if(p->z.reg==r)
          return 1;
        if(reg_pair(p->z.reg,&rp)&&(rp.r1==r||rp.r2==r))
          return 1;
      }
    }

    p=p->next;
  }
  return 1;
}

int get_reg(FILE *f,IC *p,int t)
{
  int r;
  static IC *last;
  if(t!=INT) ierror(0);
  if(p!=last){
    last=p;
    for(r=1;r<=32;r++) regs[r]&=~8;
  }
  for(r=1;r<=32;r++){
    if(!regs[r]&&regscratch[r]){
      regs[r]|=8;
      return r;
    }
  }
  for(r=1;r<=32;r++){
    if(regsa[r]) continue;
    if(regs[r]&8) continue;
    if((p->q1.flags&REG)&&p->q1.reg==r) continue;
    if((p->q2.flags&REG)&&p->q2.reg==r) continue;
    if((p->z.flags&REG)&&p->z.reg==r) continue;
    regs[r]|=16;
    emit(f,"\tsubq\t4,%s\n",regnames[sp]);
    emit(f,"\tstore\t%s,(%s)\n",regnames[r],regnames[sp]);
    return r;
  }
  ierror(0);
}

static void restore_regs(FILE *f)
{
  int r;
  for(r=32;r>=1;r--){
    if(regs[r]&16){
      emit(f,"\tload\t(%s),%s\n",regnames[sp],regnames[r]);
      emit(f,"\taddq\t4,%s\n",regnames[sp]);
      regs[r]&=~16;
    }
  }
}

static int qconst(union atyps *p,int t)
{
  unsigned long val;
  t&=NU;
  if((t&NQ)==LLONG||(t&NQ)>=FLOAT) return 0;
  eval_const(p,t);
  val=zum2ul(vumax)&0xffffffff;
  if(val>=1&&val<=32) return 1;
  return 0;
}

static void qop(FILE *f,char *s,unsigned long val,int r,int tmp)
{
  val&=0xffffffff;
  if(val==0) return;
  if(val>=1&&(val<=64||(optsize&&val<=96))){
    if(val>64)
      emit(f,"\t%sqt\t32,%s\n",s,regnames[r]);
    if(val>32)
      emit(f,"\t%sqt\t32,%s\n",s,regnames[r]);
    emit(f,"\t%sq\t%ld,%s\n",s,val<=32?val:(val<=64?val-32:val-64),regnames[r]);
  }else{
    if(r==tmp)
      ierror(0);
    emit(f,"\tmovei\t%ld,%s\n",val,regnames[tmp]);
    emit(f,"\t%s\t%s,%s\n",s,regnames[tmp],regnames[r]);
  }
}

static void qmov(FILE *f,unsigned long val,int r)
{
  val&=0xffffffff;
  emit(f,"\tmove%s\t%ld,%s\n",(val<32)?"q":"i",val,regnames[r]);
}

/* generate code to load the address of a variable into register r */
static void load_address(FILE *f,int r,struct obj *o,int type)
/*  Generates code to load the address of a variable into register r.   */
{
  if(!(o->flags&VAR))
    ierror(0);
  if((o->flags&(REG|DREFOBJ))==(REG|DREFOBJ)){
    emit(f,"\tmove\t%s,%s\n",regnames[o->reg],regnames[r]);
  }else if(o->v->storage_class==AUTO||o->v->storage_class==REGISTER){
    long off=real_offset(o);
#if 0
    emit(f,"\tmove\t%s,%s\n",regnames[fp],regnames[r]);
    if(off)
      qop(f,"add",off,r,r==t1?t2:t1);
#else
    if(off!=0){
      emit(f,"\tmove%s\t%ld,%s\n",off>=0&&off<=31?"q":"i",off,regnames[r]);
      emit(f,"\tadd\t%s,%s\n",regnames[fp],regnames[r]);
    }else
      emit(f,"\tmove\t%s,%s\n",regnames[fp],regnames[r]);
#endif
  }else{
    emit(f,"\tmovei\t");
    emit_obj(f,o,type);
    emit(f,",%s\n",regnames[r]);
  }
}
/* Generates code to load a memory object into register r. tmp is a
   general purpose register which may be used. tmp can be r. */
static void load_reg(FILE *f,int r,struct obj *o,int type)
{
  type&=NU;
  if(o->flags&VARADR){
    load_address(f,r,o,POINTER);
  }else if((o->flags&(REG|DREFOBJ))==REG){
    if(o->reg!=r){
      if(reg_pair(r,&rp)){
	if(!reg_pair(o->reg,&rp2)) ierror(0);
	emit(f,"\tmove\t%s,%s\n",regnames[rp2.r1],regnames[rp.r1]);
	emit(f,"\tmove\t%s,%s\n",regnames[rp2.r2],regnames[rp.r2]);
      }else
	emit(f,"\tmove\t%s,%s\n",regnames[o->reg],regnames[r]);
    }
  }else if((o->flags&(KONST|DREFOBJ))==KONST){
    unsigned long l;
    eval_const(&o->val,type);
    if(type==FLOAT){
      ierror(0);
    }else if(type==DOUBLE||type==LDOUBLE){
      ierror(0);
    }else if(type==LLONG){
      if(!reg_pair(r,&rp)) ierror(0);
      l=zum2zul(vumax);
      qmov(f,l,rp.r1);
      l=zum2zul(zumrshift(vumax,ul2zum(32ul)));
      qmov(f,l,rp.r2);
    }else{
      qmov(f,zum2ul(vumax),r);
    }
  }else{
    emit(f,"\tload%s\t",lst[type&NQ]);
    emit_obj(f,o,type);
    emit(f,",%s\n",regnames[r]);

  }
}

/*  Generates code to store register r into memory object o. */
static void store_reg(FILE *f,int r,struct obj *o,int type)
{
  type&=NQ;
  emit(f,"\tstore%s\t%s,",(type&NQ)==CHAR?"b":(type&NQ)==SHORT?"w":"",regnames[r]);
  emit_obj(f,o,type);
  emit(f,"\n");
}

/*  Yields log2(x)+1 or 0. */
static long pof2(zumax x)
{
  zumax p;int ln=1;
  p=ul2zum(1L);
  while(ln<=32&&zumleq(p,x)){
    if(zumeqto(x,p)) return ln;
    ln++;p=zumadd(p,p);
  }
  return 0;
}

static struct IC *preload(FILE *,struct IC *);

static void function_top(FILE *,struct Var *,long);
static void function_bottom(FILE *f,struct Var *,long);

#define isreg(x) ((p->x.flags&(REG|DREFOBJ))==REG)
#define isconst(x) ((p->x.flags&(KONST|DREFOBJ))==KONST)

static int q1reg,q2reg,zreg;

static char *ccs[]={"z","nz","mi","pl","--","nnnz",""};
static char *ccu[]={"z","nz","lo","hs","--","hi",""};
static char *logicals[]={"or","xor","and"};
static char *arithmetics[]={"shl","shr","add","sub","imult","div","div"};

/* compare if two objects are the same */
static int compare_objects(struct obj *o1,struct obj *o2)
{
  if((o1->flags&(REG|DREFOBJ))==REG&&(o2->flags&(REG|DREFOBJ))==REG&&o1->reg==o2->reg)
    return 1;
  if(o1->flags==o2->flags&&o1->am==o2->am){
    if(!(o1->flags&VAR)||(o1->v==o2->v&&zmeqto(o1->val.vmax,o2->val.vmax))){
      if(!(o1->flags&REG)||o1->reg==o2->reg){
        return 1;
      }
    }
  }
  return 0;
}


/* Does some pre-processing like fetching operands from memory to
   registers etc. */
static struct IC *preload(FILE *f,struct IC *p)
{
  int c,t;

  if(isreg(q1))
    q1reg=p->q1.reg;
  else
    q1reg=0;

  if(isreg(q2))
    q2reg=p->q2.reg;
  else
    q2reg=0;

  if(isreg(z)&&(!compare_objects(&p->q2,&p->z))){
    zreg=p->z.reg;
  }else{
    if(q1reg&&(p->code==COMPARE||p->code==TEST||scratch(p,q1reg,0)))
      zreg=q1reg;
    else if(ISFLOAT(ztyp(p)))
      zreg=f1;
    else
      zreg=t1;
  }
  
  if(p->code!=CALL&&p->code!=ADDRESS&&(p->q1.flags&(VAR|REG|VARADR))==VAR&&!direct_stack_var(&p->q1,q1typ(p))){
    load_address(f,t1,&p->q1,POINTER);
    p->q1.reg=t1;
    p->q1.flags^=(VAR|REG);
    if(p->q1.flags&DREFOBJ){
      emit(f,"\tload\t(%s),%s\n",regnames[t1],regnames[t1]);
    }else{
      p->q1.flags|=DREFOBJ;
      p->q1.dtyp=POINTER;
    }
  }
  if((p->q1.flags&(DREFOBJ|REG))==DREFOBJ&&!p->q1.am){
    p->q1.flags&=~DREFOBJ;
    load_reg(f,t1,&p->q1,q1typ(p));
    p->q1.reg=t1;
    p->q1.flags=(REG|DREFOBJ);
  }
#if 0
  if(p->q1.flags&&!isreg(q1)&&p->code!=CALL&&p->code!=SETRETURN){
    if(p->code==ASSIGN&&isreg(z))
      q1reg=p->z.reg;
    else if(ISFLOAT(q1typ(p)))
      q1reg=f1;
    else
      q1reg=t1;
    load_reg(f,q1reg,&p->q1,q1typ(p));
    p->q1.reg=q1reg;
    p->q1.flags=REG;
  }
#endif
if((p->q2.flags&(VAR|REG|VARADR))==VAR&&!direct_stack_var(&p->q2,q2typ(p))){
    load_address(f,t2,&p->q2,POINTER);
    p->q2.reg=t2;
    p->q2.flags^=(VAR|REG);
    if(p->q2.flags&DREFOBJ){
      emit(f,"\tload\t(%s),%s\n",regnames[t2],regnames[t2]);
    }else{
      p->q2.flags|=DREFOBJ;
      p->q2.dtyp=POINTER;
    }
  }
  if(p->q2.flags) t=q2typ(p);
  if((p->q2.flags&(DREFOBJ|REG))==DREFOBJ&&!p->q2.am){
    p->q2.flags&=~DREFOBJ;
    load_reg(f,t2,&p->q2,t);
    p->q2.reg=t2;
    p->q2.flags=(REG|DREFOBJ);
  }
  if(p->q2.flags&&!isreg(q2)){
    if(ISFLOAT(t))
      q2reg=f2;
    else{
      q2reg=t2;
      c=p->code;
    }
    if(q2reg==t2&&(p->q2.flags&(KONST|DREFOBJ|REG))==KONST){
      if(c==ADD||c==SUB||c==ADDI2P||c==SUBIFP||c==SUBPFP||c==LSHIFT||c==RSHIFT){
	if(qconst(&p->q2.val,t))
	  return p;
      }
      if(c==COMPARE){
	eval_const(&p->q2.val,t);
	if(zmleq(vmax,l2zm(15L))&&zmleq(l2zm(-16L),vmax))
	  return p;
      }
    }
    load_reg(f,q2reg,&p->q2,t);
    p->q2.reg=q2reg;
    p->q2.flags=REG;
  }
  return p;
}

/* save the result (in zreg) into p->z */
void save_result(FILE *f,struct IC *p)
{
  int t,r;

  if(p->z.flags) t=ztyp(p);

  if((p->z.flags&(VAR|REG))==VAR&&!direct_stack_var(&p->z,ztyp(p))){
    if(zreg==t1||zreg==f1) r=t2; else r=t1;
    load_address(f,r,&p->z,POINTER);
    p->z.reg=r;
    p->z.flags^=(VAR|REG);
    if(p->z.flags&DREFOBJ){
      emit(f,"\tload\t(%s),%s\n",regnames[r],regnames[r]);
    }else{
      p->z.flags|=DREFOBJ;
      p->z.dtyp=POINTER;
    }
  }

  if((p->z.flags&(REG|DREFOBJ))==DREFOBJ&&!p->z.am){
    p->z.flags&=~DREFOBJ;
    load_reg(f,t2,&p->z,POINTER);
    p->z.reg=t2;
    p->z.flags=(REG|DREFOBJ);
  }
  if(isreg(z)){
    if(p->z.reg!=zreg)
      emit(f,"\tmove\t%s,%s\n",regnames[zreg],regnames[p->z.reg]);
  }else{
    store_reg(f,zreg,&p->z,t);
  }
}

/* prints an object */
static void emit_obj(FILE *f,struct obj *p,int t)
{
  if(p->am&&(p->flags&DREFOBJ)){
    if(p->am->flags&GPR_IND) emit(f,"(%s+%s)",regnames[p->am->base],regnames[p->am->offset]);
    if(p->am->flags&IMM_IND) emit(f,"(%s+%ld)",regnames[p->am->base],p->am->offset/4);
    return;
  }
  if((p->flags&(KONST|DREFOBJ))==(KONST|DREFOBJ)){
    emitval(f,&p->val,p->dtyp&NU);
    return;
  }
  if(p->flags&DREFOBJ) emit(f,"(");
  if(p->flags&REG){
    emit(f,"%s",regnames[p->reg]);
  }else if(p->flags&VAR) {
    if(p->v->storage_class==AUTO||p->v->storage_class==REGISTER){
      long of=real_offset(p);
      if(of==0)
	emit(f,"(%s)",regnames[fp]);
      else
	emit(f,"(%s+%ld)",regnames[fp],real_offset(p)/4);
    }else{
      if(!zmeqto(l2zm(0L),p->val.vmax)){emitval(f,&p->val,LONG);emit(f,"+");}
      if(p->v->storage_class==STATIC){
        emit(f,"%s%ld",labprefix,zm2l(p->v->offset));
      }else{
        emit(f,"%s%s",idprefix,p->v->identifier);
      }
    }
  }
  if(p->flags&KONST){
    emitval(f,&p->val,t&NU);
  }
  if(p->flags&DREFOBJ) emit(f,")");
}

/*  Test if there is a sequence of FREEREGs containing FREEREG reg.
    Used by peephole. */
static int exists_freereg(struct IC *p,int reg)
{
  while(p&&(p->code==FREEREG||p->code==ALLOCREG)){
    if(p->code==FREEREG&&p->q1.reg==reg) return 1;
    p=p->next;
  }
  return 0;
}

static int fpsaveop(IC *p, struct obj *o)
{
  if((o->flags&(REG|VAR))==VAR&&(o->v->storage_class==AUTO||o->v->storage_class==REGISTER)){
    unsigned long of;int t;
    if(o==&p->q1) t=q1typ(p);
    else if(o==&p->q2) t=q2typ(p);
    else t=ztyp(p);
    t&=NQ;
    if(t==INT||t==LONG){
      of=real_offset(o); /* TODO: dynamic stack-offset not known */
      if(of>128||(of&3)) return 0;
      if(of>64) return 8;
      if(of>32) return 6;
      return 4;
    }
  }
  return 0;
}

/* search for possible addressing-modes */
static void peephole(struct IC *p)
{
  int c,c2,r;struct IC *p2;struct AddressingMode *am;

  fpsave=0;

  for(;p;p=p->next){
    c=p->code;
    if(c!=FREEREG&&c!=ALLOCREG&&(c!=SETRETURN||!isreg(q1)||p->q1.reg!=p->z.reg)) exit_label=0;
    if(c==LABEL) exit_label=p->typf;

    /* savings for using r14/r15 as fp */
    fpsave+=fpsaveop(p,&p->q1);
    fpsave+=fpsaveop(p,&p->q2);
    fpsave+=fpsaveop(p,&p->z);

    /* Try const(reg) */
    if(IMM_IND&&(c==ADDI2P||c==SUBIFP)&&isreg(z)&&(p->q2.flags&(KONST|DREFOBJ))==KONST){
      int base;unsigned long of;struct obj *o;
      eval_const(&p->q2.val,p->typf);
      of=zum2ul(vumax);
      if(c==SUBIFP) of=-of;
      if(of>0&&of<=128&&(of&3)==0){
	r=p->z.reg;
	if(isreg(q1)) base=p->q1.reg; else base=r;
	o=0;
	if(ISIDX(base)){
	  for(p2=p->next;p2;p2=p2->next){
	    c2=p2->code;
	    if(c2==CALL||c2==LABEL||(c2>=BEQ&&c2<=BRA)) break;
	    if(c2!=FREEREG&&(p2->q1.flags&(REG|DREFOBJ))==REG&&p2->q1.reg==r) break;
	    if(c2!=FREEREG&&(p2->q2.flags&(REG|DREFOBJ))==REG&&p2->q2.reg==r) break;
	    if(c2!=CALL&&(c2<LABEL||c2>BRA)/*&&c2!=ADDRESS*/){
	      int t;
	      if(!p2->q1.am&&(p2->q1.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->q1.reg==r){
		if(o) break;
		t=q1typ(p2)&NQ;
		if(t!=INT&&t!=LONG) break;
		o=&p2->q1;
	      }
	      if(!p2->q2.am&&(p2->q2.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->q2.reg==r){
		if(o) break;
		t=q2typ(p2)&NQ;
		if(t!=INT&&t!=LONG) break;
		o=&p2->q2;
	      }
	      if(!p2->z.am&&(p2->z.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->z.reg==r){
		if(o) break;
		t=ztyp(p2)&NQ;
		if(t!=INT&&t!=LONG) break;
		o=&p2->z;
	      }
	    }
	    if(c2==FREEREG||(p2->z.flags&(REG|DREFOBJ))==REG){
	      int m;
	      if(c2==FREEREG)
		m=p2->q1.reg;
	      else
		m=p2->z.reg;
	      if(m==r){
		if(o){
		  o->am=am=mymalloc(sizeof(*am));
		  am->flags=IMM_IND;
		  am->base=base;
		  am->offset=zm2l(of);
		  if(isreg(q1)){
		    p->code=c=NOP;p->q1.flags=p->q2.flags=p->z.flags=0;
		  }else{
		    p->code=c=ASSIGN;p->q2.flags=0;
		    p->typf=p->typf2;p->q2.val.vmax=sizetab[p->typf2&NQ];
		  }
		}
		break;
	      }
	      if(c2!=FREEREG&&m==base) break;
	      continue;
	    }
	  }
	}
      }
    }
    /* Try reg,reg */
    if(GPR_IND&&c==ADDI2P&&isreg(q2)&&isreg(z)&&(isreg(q1)||p->q2.reg!=p->z.reg)){
      int base,idx;struct obj *o;
      r=p->z.reg;idx=p->q2.reg;
      if(isreg(q1)) base=p->q1.reg; else base=r;
      o=0;
      if(ISIDX(base)){
	for(p2=p->next;p2;p2=p2->next){
	  c2=p2->code;
	  if(c2==CALL||c2==LABEL||(c2>=BEQ&&c2<=BRA)) break;
	  if(c2!=FREEREG&&(p2->q1.flags&(REG|DREFOBJ))==REG&&p2->q1.reg==r) break;
	  if(c2!=FREEREG&&(p2->q2.flags&(REG|DREFOBJ))==REG&&p2->q2.reg==r) break;
	  if((p2->z.flags&(REG|DREFOBJ))==REG&&p2->z.reg==idx&&idx!=r) break;
	  
	  if(c2!=CALL&&(c2<LABEL||c2>BRA)/*&&c2!=ADDRESS*/){
	    int t;
	    if(!p2->q1.am&&(p2->q1.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->q1.reg==r){
	      if(o) break;
	      t=q1typ(p2)&NQ;
	      if(t!=INT&&t!=LONG) break;
	      o=&p2->q1;
	    }
	    if(!p2->q2.am&&(p2->q2.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->q2.reg==r){
	      if(o) break;
	      t=q2typ(p2)&NQ;
	      if(t!=INT&&t!=LONG) break;
	      o=&p2->q2;
	    }
	    if(!p2->z.am&&(p2->z.flags&(REG|DREFOBJ))==(REG|DREFOBJ)&&p2->z.reg==r){
	      if(o) break;
	      t=ztyp(p2)&NQ;
	      if(t!=INT&&t!=LONG) break;
	      o=&p2->z;
	    }
	  }
	  if(c2==FREEREG||(p2->z.flags&(REG|DREFOBJ))==REG){
	    int m;
	    if(c2==FREEREG)
	      m=p2->q1.reg;
	    else
	      m=p2->z.reg;
	    if(m==r){
	      if(o){
		o->am=am=mymalloc(sizeof(*am));
		am->flags=GPR_IND;
		am->base=base;
		am->offset=idx;
		if(isreg(q1)){
		  p->code=c=NOP;p->q1.flags=p->q2.flags=p->z.flags=0;
		}else{
		  p->code=c=ASSIGN;p->q2.flags=0;
		  p->typf=p->typf2;p->q2.val.vmax=sizetab[p->typf2&NQ];
		}
	      }
	      break;
	    }
	    if(c2!=FREEREG&&m==base) break;
	    continue;
	  }
	}
      }
    }
  }
}

static int must_save_reg(int r)
{
  if((regused[r]&&!regsa[r]&&!regscratch[r])||(r==lr&&function_calls))
    return 1;
  return 0;
}

/* generates the function entry code */
static void function_top(FILE *f,struct Var *v,long offset)
{
  int r,rs=0,msp;

  fpmerk=fp;
  if(fpsave>4&&!regused[15]&&!function_calls) fp=15;
  else if(fpsave>8&&!regused[16]) fp=16;
  if(fp!=fpmerk){
    regused[fp]=regs[fp]=1;
    BSET(regs_modified,fp);
  }

  rsavesize=0;
  if(!special_section(f,v)&&section!=CODE){emit(f,codename);if(f) section=CODE;} 
  emit(f,"\t.align\t%s\n",(workaround&4)?"8":"2");
  if(v->storage_class==EXTERN){
    if((v->flags&(INLINEFUNC|INLINEEXT))!=INLINEFUNC)
      emit(f,"\t.global\t%s%s\n",idprefix,v->identifier);
    emit(f,"%s%s:\n",idprefix,v->identifier);
  }else
    emit(f,"%s%ld:\n",labprefix,zm2l(v->offset));
  if(workaround&4) emit(f,"%s%d:\n",labprefix,flabel=++label);
  for(r=1;r<=MAXR;r++)
    if(must_save_reg(r)) rsavesize++;
  if(rsavesize>=3&&!ISIDX(sp)) msp=15; else msp=sp;
  BSET(regs_modified,msp);
  if(ISIDX(msp)) qop(f,"sub",rsavesize*4,sp,t1);
  if(msp!=sp) emit(f,"\tmove\t%s,%s\n",regnames[sp],regnames[msp]);
  rs=rsavesize;
  for(r=1;r<=MAXR;r++){
    if(must_save_reg(r)){
      if(ISIDX(msp)){
	emit(f,"\tstore\t%s,(%s+%d)\n",regnames[r],regnames[msp],--rs);
      }else{
	emit(f,"\tsubq\t4,%s\n",regnames[msp]);
	emit(f,"\tstore\t%s,(%s)\n",regnames[r],regnames[msp]);
      }
    }
  }
  if(localsize) qop(f,"sub",localsize,fpmerk,t1);

  if(fp!=fpmerk)
    emit(f,"\tmove\t%s,%s\n",regnames[fpmerk],regnames[fp]);
}

/* generates the function exit code */
static void function_bottom(FILE *f,struct Var *v,long offset)
{
  int r,rs=0,msp;

  if(localsize+notpopped) qop(f,"add",localsize+notpopped,fpmerk,t1);
  fp=fpmerk;

  if(rsavesize>=3&&!ISIDX(sp)) msp=15; else msp=sp;

  if(msp!=sp) emit(f,"\tmove\t%s,%s\n",regnames[sp],regnames[msp]);
  
  for(r=MAXR;r>=1;r--){
    if(must_save_reg(r)){
      if(ISIDX(msp)){
	emit(f,"\tload\t(%s+%d),%s\n",regnames[msp],rs++,regnames[r]);
      }else{
	emit(f,"\tload\t(%s),%s\n",regnames[msp],regnames[r]);
	emit(f,"\taddq\t4,%s\n",regnames[msp]);
      }
    }
  }
  if(ISIDX(msp)) {emit(f,"# barrier\n");qop(f,"add",rsavesize*4,sp,t1);}
  emit(f,ret);
}

/****************************************/
/*  End of private data and functions.  */
/****************************************/

/*  Does necessary initializations for the code-generator. Gets called  */
/*  once at the beginning and should return 0 in case of problems.      */
int init_cg(void)
{
  int i;
  /*  Initialize some values which cannot be statically initialized   */
  /*  because they are stored in the target's arithmetic.             */
  maxalign=l2zm(8L);
  char_bit=l2zm(8L);
  stackalign=l2zm(4);

  for(i=0;i<=MAX_TYPE;i++){
    sizetab[i]=l2zm(msizetab[i]);
    align[i]=l2zm(malign[i]);
  }

  for(i=FIRST_GPR;i<=LAST_GPR;i++){
    regsize[i]=l2zm(4L);
    regtype[i]=&ltyp;
    if(!ISSHADOW(i)) reg_prio[i]=10;
    if(ISSHADOW(i)) regsa[i]=1;
  }
  for(i=FIRST_PAIR;i<=LAST_PAIR;i++){
    regsize[i]=l2zm(8L);
    regtype[i]=&lltyp;
    if(!ISSHADOW(i)) reg_prio[i]=10;
  }

  addr_vkonst=1;

  /*  Initialize the min/max-settings. Note that the types of the     */
  /*  host system may be different from the target system and you may */
  /*  only use the smallest maximum values ANSI guarantees if you     */
  /*  want to be portable.                                            */
  /*  That's the reason for the subtraction in t_min[INT]. Long could */
  /*  be unable to represent -2147483648 on the host system.          */
  t_min[CHAR]=l2zm(-128L);
  t_min[SHORT]=l2zm(-32768L);
  t_min[INT]=zmsub(l2zm(-2147483647L),l2zm(1L));
  t_min[LONG]=t_min(INT);
  t_min[LLONG]=zmlshift(l2zm(1L),l2zm(63L));
  t_min[MAXINT]=t_min(LLONG);
  t_max[CHAR]=ul2zum(127L);
  t_max[SHORT]=ul2zum(32767UL);
  t_max[INT]=ul2zum(2147483647UL);
  t_max[LONG]=t_max(INT);
  t_max[LLONG]=zumrshift(zumkompl(ul2zum(0UL)),ul2zum(1UL));
  t_max[MAXINT]=t_max(LLONG);
  tu_max[CHAR]=ul2zum(255UL);
  tu_max[SHORT]=ul2zum(65535UL);
  tu_max[INT]=ul2zum(4294967295UL);
  tu_max[LONG]=t_max(UNSIGNED|INT);
  tu_max[LLONG]=zumkompl(ul2zum(0UL));
  tu_max[MAXINT]=t_max(UNSIGNED|LLONG);
  
  /*  Reserve a few registers for use by the code-generator.      */
  /*  This is not optimal but simple.                             */
  sp=REG_SP;
  fp=REG_FP;


  t1=31;
  t2=29;
  lr=28;
  f1=31+32+16;
  f2=f1-1;


  regsa[t1]=regsa[t2]=1;
  regsa[f1]=regsa[f2]=1;
  regsa[fp]=regsa[sp]=regsa[lr]=1;
  regscratch[t1]=regscratch[t2]=0;
  regscratch[f1]=regscratch[f2]=0;
  regscratch[fp]=regscratch[sp]=regscratch[lr]=0;

  for(i=FIRST_GPR;i<FIRST_GPR+VOL_GPRS;i++){
    regscratch[i]=1;
    regscratch[FIRST_PAIR+(i-1)/2]=1;
  }

  target_macros=marray;

  declare_builtin("__mulint32",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__mulint32gpu",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__mulint32dsp",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__divint32",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__divint32gpu",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__divint32dsp",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__modint32",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__modint32gpu",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__modint32dsp",INT,INT,FIRST_GPR,INT,FIRST_GPR+1,1,0);
  declare_builtin("__divuint32",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);
  declare_builtin("__divuint32gpu",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);
  declare_builtin("__divuint32dsp",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);
  declare_builtin("__moduint32",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);
  declare_builtin("__moduint32gpu",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);
  declare_builtin("__moduint32dsp",UNSIGNED|INT,UNSIGNED|INT,FIRST_GPR+2,UNSIGNED|INT,FIRST_GPR+3,1,0);


  if(GPUMAIN) dmode=M_GPUMAIN;
  if(GPULOCAL) dmode=M_GPULOCAL;
  if(DSPMAIN) dmode=M_DSPMAIN;
  if(DSPLOCAL) dmode=M_DSPLOCAL;


  return 1;
}

void init_db(FILE *f)
{
}

int freturn(struct Typ *t)
/*  Returns the register in which variables of type t are returned. */
/*  If the value cannot be returned in a register returns 0.        */
/*  A pointer MUST be returned in a register. The code-generator    */
/*  has to simulate a pseudo register if necessary.                 */
{
  if(ISFLOAT(t->flags)) 
    return FIRST_PAIR;
  if(ISSTRUCT(t->flags)||ISUNION(t->flags)) 
    return 0;
  if(zmleq(szof(t),l2zm(4L))) 
    return FIRST_GPR;
  else
    return 0;
}

int reg_pair(int r,struct rpair *p)
/* Returns 0 if the register is no register pair. If r  */
/* is a register pair non-zero will be returned and the */
/* structure pointed to p will be filled with the two   */
/* elements.                                            */
{
  if(r>=FIRST_PAIR&&r<=LAST_PAIR){
    rp.r1=(r-FIRST_PAIR)*2+FIRST_GPR;
    rp.r2=rp.r1+1;
    return 1;
  }
  return 0;
}

int cost_load_reg(int r, Var *v)
{
  if(v->ctyp){
    /* todo: check for quick */
    return 3;
  }
  if(isauto(v->storage_class)){
    /* todo: check offset */
    return ISIDX(fp)?6:9;
  }
  /* extern/static */
  return 6;
}

/* estimate the cost-saving if object o from IC p is placed in
   register r */
int cost_savings(struct IC *p,int r,struct obj *o)
{
  int c=p->code,rt;
  if(ISSHADOW(r)) rt=0; else rt=2;
  if(o->flags&VKONST){
    if(o==&p->q2&&(c==ADD||c==SUB||c==ADDI2P||c==SUBIFP||c==SUBIFP||c==LSHIFT||c==RSHIFT)){
      if(qconst(&o->v->cobj.val,o->v->ctyp))
	return 0;
    }
    if(o==&p->q2&&c==COMPARE){
      eval_const(&o->v->cobj.val,o->v->ctyp);
      if(zmleq(vmax,l2zm(15L))&&zmleq(l2zm(-16L),vmax))
	return 0;
    }
    if(o==&p->q1&&p->code==ASSIGN){
      int q=0;
      eval_const(&o->v->cobj.val,o->v->ctyp);
      if(zum2ul(vumax)<=31) q=1;
      if((p->z.flags&DREFOBJ)||((p->z.flags&VAR)&&(p->z.v->storage_class==EXTERN||p->z.v->storage_class==STATIC)))
	return q?rt:rt+2;
      else
	return q?0:rt;
    }
  }
  if(c==ADDI2P&&ISIDX(r)&&!(o->flags&DREFOBJ)){
    if(o!=&p->q2&&p->q2.flags&(KONST|DREFOBJ)==KONST){
      unsigned long v;
      eval_const(&p->q2.val,p->typf&NU);
      v=zum2ul(vumax);
      if(v>0&&v<=128&&(v&3)==0){
	if(v>32) rt+=2;
	if(v>64) rt+=2;
	return o==&p->q1?rt+4:rt+3;
      }
    }
    if(o==&p->q1) return rt+3;
    if(o==&p->q1) return rt+3;
  }
  if(o->flags&DREFOBJ)
    return rt+4;
  if(c==SETRETURN&&r==p->z.reg&&!(o->flags&DREFOBJ)) return rt+3;
  if(c==GETRETURN&&r==p->q1.reg&&!(o->flags&DREFOBJ)) return rt+3;
  return rt+2;
}

int regok(int r,int t,int mode)
/*  Returns 0 if register r cannot store variables of   */
/*  type t. If t==POINTER and mode!=0 then it returns   */
/*  non-zero only if the register can store a pointer   */
/*  and dereference a pointer to mode.                  */
{
  if(r==0)
    return 0;
  t&=NQ;
  if((t==POINTER||t==FLOAT||(t>=CHAR&&t<=LONG))&&r>=FIRST_GPR&&r<=LAST_GPR)
    return 1;
  if((t==LLONG||t==DOUBLE||t==LDOUBLE)&&r>=FIRST_PAIR&&r<=LAST_PAIR)
    return 1;

  return 0;
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
  int c=p->code;
  if((p->q1.flags&DREFOBJ)||(p->q2.flags&DREFOBJ)||(p->z.flags&DREFOBJ))
    return 1;
  if((c==DIV||c==MOD)&&!isconst(q2))
    return 1;
  return 0;
}

int must_convert(int o,int t,int const_expr)
/*  Returns zero if code for converting np to type t    */
/*  can be omitted.                                     */
{
  int op=o&NQ,tp=t&NQ;
  if((op==INT||op==LONG||op==POINTER)&&(tp==INT||tp==LONG||tp==POINTER))
    return 0;
  if(op==DOUBLE&&tp==LDOUBLE) return 0;
  if(op==LDOUBLE&&tp==DOUBLE) return 0;
  return 1;
}

void gen_ds(FILE *f,zmax size,struct Typ *t)
/*  This function has to create <size> bytes of storage */
/*  initialized with zero.                              */
{
  emit(f,"\t.space\t%ld\n",zm2l(size));
}

void gen_align(FILE *f,zmax align)
/*  This function has to make sure the next data is     */
/*  aligned to multiples of <align> bytes.              */
{
  if(zm2l(align)>1) emit(f,"\t.balign\t%ld\n",zm2l(align));
}

void gen_var_head(FILE *f,struct Var *v)
/*  This function has to create the head of a variable  */
/*  definition, i.e. the label and information for      */
/*  linkage etc.                                        */
{
  int constflag;char *sec;
  if(v->clist) constflag=is_const(v->vtyp);
  if(v->storage_class==STATIC){
    if(ISFUNC(v->vtyp->flags)) return;
    if(!special_section(f,v)){
      if(v->clist&&!constflag&&section!=DATA){emit(f,dataname);if(f) section=DATA;}
      if(v->clist&&constflag&&section!=RODATA){emit(f,rodataname);if(f) section=RODATA;}
      if(!v->clist&&section!=BSS){emit(f,bssname);if(f) section=BSS;}
    }
    gen_align(f,falign(v->vtyp));
    emit(f,"%s%ld:\n",labprefix,zm2l(v->offset));
  }
  if(v->storage_class==EXTERN){
    emit(f,"\t.globl\t%s%s\n",idprefix,v->identifier);
    if(v->flags&(DEFINED|TENTATIVE)){
      if(!special_section(f,v)){
	if(v->clist&&!constflag&&section!=DATA){emit(f,dataname);if(f) section=DATA;}
	if(v->clist&&constflag&&section!=RODATA){emit(f,rodataname);if(f) section=RODATA;}
	if(!v->clist&&section!=BSS){emit(f,bssname);if(f) section=BSS;}
      }
      gen_align(f,falign(v->vtyp));
      emit(f,"%s%s:\n",idprefix,v->identifier);
    }
  }
}

void gen_dc(FILE *f,int t,struct const_list *p)
/*  This function has to create static storage          */
/*  initialized with const-list p.                      */
{
  if((t&NQ)==POINTER) t=UNSIGNED|LONG;
  emit(f,"\t.%s\t",dct[t&NQ]);
  if(!p->tree){
    if(ISFLOAT(t)){
      /*  auch wieder nicht sehr schoen und IEEE noetig   */
      unsigned char *ip;
      ip=(unsigned char *)&p->val.vdouble;
      emit(f,"0x%02x%02x%02x%02x",ip[0],ip[1],ip[2],ip[3]);
      if((t&NQ)!=FLOAT){
	emit(f,",0x%02x%02x%02x%02x",ip[4],ip[5],ip[6],ip[7]);
      }
    }else{
      emitval(f,&p->val,t&NU);
    }
  }else{
    emit_obj(f,&p->tree->o,t&NU);
  }
  emit(f,"\n");
}


/*  The main code-generation routine.                   */
/*  f is the stream the code should be written to.      */
/*  p is a pointer to a doubly linked list of ICs       */
/*  containing the function body to generate code for.  */
/*  v is a pointer to the function.                     */
/*  offset is the size of the stackframe the function   */
/*  needs for local variables.                          */

void gen_code(FILE *f,struct IC *p,struct Var *v,zmax offset)
/*  The main code-generation.                                           */
{
  int c,t,i,cmpt;
  struct IC *m;
  argsize=0;
  if(DEBUG&1) printf("gen_code()\n");
  for(c=1;c<=MAXR;c++) regs[c]=regsa[c];
  stackoffset=notpopped=dontpop=maxpushed=0;


  cmode=mmode(v);
  if(cmode!=M_GPULOCAL&&cmode!=M_DSPLOCAL)
    workaround=WORKAROUND;
  else
    workaround=0;

  if((workaround&3)==3)
    ret=LALIGNSTR "\tjump\t(r27)\n\tnop\n\tnop\n";
  else if(workaround&1)
    ret=LALIGNSTR "\tjump\t(r27)\n\tnop\n";
  else if(workaround&2)
    ret="\tjump\t(r27)\n\tnop\n\tnop\n";
  else
    ret="\tjump\t(r27)\n\tnop\n";

  for(m=p;m;m=m->next){
    c=m->code;t=m->typf&NU;
    if(c==ALLOCREG) {regs[m->q1.reg]=1;continue;}
    if(c==FREEREG) {regs[m->q1.reg]=0;continue;}

    if(c==CALL&&argsize<zm2l(m->q2.val.vmax)) argsize=zm2l(m->q2.val.vmax);
  }

  peephole(p);

  for(c=1;c<=MAXR;c++){
    if(regsa[c]||regused[c]){
      BSET(regs_modified,c);
    }
  }

  localsize=(zm2l(offset)+3)/4*4;
#if FIXED_SP
  /*FIXME: adjust localsize to get an aligned stack-frame */
#endif

  function_top(f,v,localsize);

  for(;p;p=p->next){
    c=p->code;t=p->typf;

    if(notpopped&&!dontpop){
      if(c==LABEL||c==COMPARE||c==TEST||(c>=BEQ&&c<=BRA)){
	qop(f,"add",notpopped,fp,t1);
	pop(notpopped);
	notpopped=0;
      }
    }
    
    if(c==NOP) {p->z.flags=0;continue;}
    if(c==ALLOCREG) {regs[p->q1.reg]=1;continue;}
    if(c==FREEREG) {regs[p->q1.reg]=0;continue;}
    if(c==LABEL){
      if(workaround&4)
	LALIGN();
      emit(f,"%s%d:\n",labprefix,t);
      if(workaround&4)
	emit(f,"\tnop\n");
      continue;
    }
    if(c>=BEQ&&c<=BRA){
      if(0/*t==exit_label&&framesize==0*/)
	emit(f,ret);
      else{
	if(workaround&1){
	  int here=++label;
	  emit(f,"\tmovei\t%s%d",labprefix,t);
	  if(workaround&4) emit(f,"+(((%s%d-%s%d)&0xffffff00)==((%s%d-%s%d)&0xffffff00))*2",labprefix,here,labprefix,flabel,labprefix,t,labprefix,flabel);
	  emit(f,",%s\n",regnames[t1]);
	  LALIGN();
	  if(workaround&4) emit(f,"%s%d:\n",labprefix,here);
	  emit(f,"\tjump\t%s,(%s)\n",c==BRA?"t":(cmpt&UNSIGNED)?ccu[c-BEQ]:ccs[c-BEQ],regnames[t1]);
	}else
	  emit(f,"\tjr\t%s,%s%d\n",c==BRA?"t":(cmpt&UNSIGNED)?ccu[c-BEQ]:ccs[c-BEQ],labprefix,t);
	if(workaround&2) emit(f,"\tnop\n");
      }
      emit(f,"\tnop\n");
      continue;
    }
#if 0
    if(c>=BEQ&&c<BRA){
      if(workaround&1) LALIGN();
      emit(f,"\tjr\t%s,%s%d\n",(cmpt&UNSIGNED)?ccu[c-BEQ]:ccs[c-BEQ],labprefix,t);
      emit(f,"\tnop\n");
      if(workaround&2) emit(f,"\tnop\n");
      continue;
    }
#endif
    if(c==MOVETOREG||c==MOVEFROMREG){
      p->code=c=ASSIGN;
      p->typf=t=INT;
      p->q2.val.vmax=l2zm(4L);
      p->typf2=4;
    }
    if((c==ASSIGN||c==PUSH)&&((t&NQ)>POINTER||((t&NQ)==CHAR&&zm2l(p->q2.val.vmax)!=1))){
      int sreg,dreg,creg,treg,ct;unsigned long cnt;
      if(c==PUSH) p->typf2=4;
      if((p->typf2&3)==0&&p->typf2>=4) ct=4;
      else if((p->typf2&1)==0&&p->typf2>=2) ct=2;
      else ct=1;
      cnt=zum2ul(p->q2.val.vumax);
      if((p->q1.flags&(REG|DREFOBJ))==(REG|DREFOBJ)){
	if(scratch(p->next,p->q1.reg,0)){
	  sreg=p->q1.reg;
	}else{
	  sreg=t1;
	  emit(f,"\tmove\t%s,%s\n",regnames[p->q1.reg],regnames[sreg]);
	}
      }else{
	sreg=t1;
	if(p->q1.flags&DREFOBJ){
	  load_address(f,sreg,&p->q1,POINTER);
	  emit(f,"\tload\t(%s),%s\n",regnames[sreg],regnames[sreg]);
	}else
	  load_address(f,sreg,&p->q1,POINTER);
      }
      if(c==PUSH){
	dreg=t2;
	qop(f,"sub",cnt,fpmerk,t2);
	push(cnt);
	dontpop+=zm2l(cnt);
	emit(f,"\tmove\t%s,%s\n",regnames[fpmerk],regnames[t2]);
      }else if((p->z.flags&(REG|DREFOBJ))==(REG|DREFOBJ)){
	if(scratch(p->next,p->z.reg,0)/*&&(!optspeed||ISIDX(p->z.reg))*/){
	  dreg=p->z.reg;
	}else{
	  dreg=t2;
	  emit(f,"\tmove\t%s,%s\n",regnames[p->z.reg],regnames[dreg]);
	}
      }else{
	dreg=t2;
	if(p->z.flags&DREFOBJ){
	  load_address(f,dreg,&p->z,POINTER);
	  emit(f,"\tload\t(%s),%s\n",regnames[dreg],regnames[dreg]);
	}else
	  load_address(f,dreg,&p->z,POINTER);
      }
      if(sreg!=t1&&dreg!=t1) creg=t1;
      else if(sreg!=t2&&dreg!=t2) creg=t2;
      else{
	creg=get_reg(f,p,INT);
      }
      if(sreg!=t1&&dreg!=t1&&creg!=t1) treg=t1;
      else if(sreg!=t2&&dreg!=t2&&creg!=t2) treg=t2;
      else{
	treg=get_reg(f,p,INT);
      }
      emit(f,"\tmovei\t%lu,%s\n",cnt/ct,regnames[creg]);
      if(workaround&4) emit(f,"\t.p2alignw 4,0xe400\n\tnop\n");
      emit(f,"%s%d:\n",labprefix,++label);
      emit(f,"\tload%s\t(%s),%s\n",lss[ct],regnames[sreg],regnames[treg]);
      emit(f,"\tstore%s\t%s,(%s)\n",lss[ct],regnames[treg],regnames[dreg]);
      emit(f,"\tsubq\t1,%s\n",regnames[creg]);
      emit(f,"\taddqt\t%d,%s\n",ct,regnames[sreg]);
      if(workaround&2){
	emit(f,"\taddqt\t%d,%s\n",ct,regnames[dreg]);
	if(workaround&1) LALIGN();
	emit(f,"\tjr\tnz,%s%d\n",labprefix,label);
	emit(f,"\tnop\n");
	emit(f,"\tnop\n");
      }else{
	if(workaround&1) LALIGN();
	emit(f,"\tjr\tnz,%s%d\n",labprefix,label);
	emit(f,"\taddqt\t%d,%s\n",ct,regnames[dreg]);
      }

      restore_regs(f);
      continue;
    }
    /* switch commutative operands if suitable */
    switch_IC(p);

    if(c==COMPARE){
      m=p->next;
      if(!m) ierror(0);
      while(m->code<BEQ||m->code>BGT) m=m->next;
      if(m->code==BLE){
	obj tmp;
	tmp=p->q1;
	p->q1=p->q2;
	p->q2=tmp;
	m->code=BGE;
      }
    }

    if(c==CONVERT&&zmeqto(sizetab[p->typf&NQ],sizetab[p->typf2&NQ])){
      p->code=c=ASSIGN;
      p->q2.val.vmax=sizetab[p->typf&NQ];
      p->typf2=4;
    }

    p=preload(f,p);
    c=p->code;
    if(c==SUBPFP) c=SUB;
    if(c==ADDI2P) c=ADD;
    if(c==SUBIFP) c=SUB;
    if(c==CONVERT){
      int to=q1typ(p)&NU;
      if(ISFLOAT(to)||ISFLOAT(t)) ierror(0);
      load_reg(f,zreg,&p->q1,to);
      if((zm2l(sizetab[t&NQ])>zm2l(sizetab[to&NQ]))||isreg(z)){
	if(!(to&UNSIGNED)){
	  if((to&NQ)==CHAR){
	    emit(f,"\tshlq\t24,%s\n",regnames[zreg]);
	    emit(f,"\tsharq\t24,%s\n",regnames[zreg]);
	  }else{
	    emit(f,"\tshlq\t16,%s\n",regnames[zreg]);
	    emit(f,"\tsharq\t16,%s\n",regnames[zreg]);
	  }
	}else if(isreg(q1)){
	  if((to&NQ)==CHAR){
	    emit(f,"\tshlq\t24,%s\n",regnames[zreg]);
	    emit(f,"\tshrq\t24,%s\n",regnames[zreg]);
	  }else{
	    emit(f,"\tshlq\t16,%s\n",regnames[zreg]);
	    emit(f,"\tshrq\t16,%s\n",regnames[zreg]);
	  }
	}
      }
      save_result(f,p);
      continue;
    }
    if(c==KOMPLEMENT){
      load_reg(f,zreg,&p->q1,t);
      emit(f,"\tnot\t%s\n",regnames[zreg]);
      save_result(f,p);
      continue;
    }
    if(c==SETRETURN){
      load_reg(f,p->z.reg,&p->q1,t);
      BSET(regs_modified,p->z.reg);
      continue;
    }
    if(c==GETRETURN){
      if(p->q1.reg){
        zreg=p->q1.reg;
	save_result(f,p);
      }else
        p->z.flags=0;
      continue;
    }
    if(c==CALL){
      int reg;
      /*FIXME*/
#if 0      
      if(stack_valid&&(p->q1.flags&(VAR|DREFOBJ))==VAR&&p->q1.v->fi&&(p->q1.v->fi->flags&ALL_STACK)){
	if(framesize+zum2ul(p->q1.v->fi->stack1)>stack)
	  stack=framesize+zum2ul(p->q1.v->fi->stack1);
      }else
	stack_valid=0;
#endif
      if((p->q1.flags&(VAR|DREFOBJ))==VAR&&!strcmp("__va_start",p->q1.v->identifier)){
	long of=va_offset(v)+localsize-stackoffset;
	emit(f,"\tmove\t%s,%s\n",regnames[fp],regnames[1]);
	if(of) qop(f,"add",of,1,t1);
	continue;
      }
      if((p->q1.flags&(VAR|DREFOBJ))==VAR&&p->q1.v->fi&&p->q1.v->fi->inline_asm){
        emit_inline_asm(f,p->q1.v->fi->inline_asm);
      }else{
	int banknr;
	if((p->q1.flags&(REG|DREFOBJ))!=(REG|DREFOBJ)){
	  if((p->q1.flags&(VAR|DREFOBJ))==VAR&&(banknr=bank(p->q1.v))){
	    ierror(0);
	    emit(f,"\tmovei\t___callbanked,%s\n",regnames[t1]);
	    load_address(f,16,&p->q1,POINTER);
	    emit(f,"\tmove%c\t%d,%s\n",banknr>31?'i':'q',banknr,regnames[t2]);
	  }else{
	    load_address(f,t1,&p->q1,POINTER);
	    i=t1;
	  }
	}else
	  i=p->q1.reg;
	if((workaround&3)==3) LALIGN();
	emit(f,"\tmove\tpc,%s\n",regnames[lr]);
	if(workaround&2){ /* TODO: direct recursion */
	  emit(f,"\taddq\t8,%s\n",regnames[lr]);
	  emit(f,"\tjump\t(%s)\n",regnames[i]);
	  emit(f,"\tnop\n");
	  emit(f,"\tnop\n");
	}else{
	  if(workaround&1) LALIGN();
	  emit(f,"\tjump\t(%s)\n",regnames[i]);
	  emit(f,"\taddq\t6,%s\n",regnames[lr]);
	}
      }

      if(!zmeqto(p->q2.val.vmax,l2zm(0L))){
        notpopped+=zm2l(p->q2.val.vmax);
        dontpop-=zm2l(p->q2.val.vmax);
        if(!NODELAYEDPOP&&!vlas&&stackoffset==-notpopped){
          /*  Entfernen der Parameter verzoegern  */
        }else{
	  qop(f,"add",zm2l(p->q2.val.vmax),fp,t1);
	  pop(zm2l(p->q2.val.vmax));
          notpopped-=zm2l(p->q2.val.vmax);
        }
      }

      if((p->q1.flags&(VAR|DREFOBJ))==VAR&&p->q1.v->fi&&(p->q1.v->fi->flags&ALL_REGS)){
	bvunite(regs_modified,p->q1.v->fi->regs_modified,RSIZE);
      }else{
	int i;
	for(i=1;i<=MAXR;i++){
	  if(regscratch[i]) BSET(regs_modified,i);
	}
      }
      continue;
    }

    if(c==ASSIGN||c==PUSH){
      if(t==0) ierror(0);
      if(c==PUSH){
	qop(f,"sub",zm2l(p->q2.val.vmax),fpmerk,zreg==t1?t2:t1);
	if(isreg(q1)) zreg=p->q1.reg;
	load_reg(f,zreg,&p->q1,t);
	emit(f,"\tstore%s\t%s,(%s)\n",lst[t&NQ],regnames[zreg],regnames[fpmerk]);
	push(zm2l(p->q2.val.vmax));
	dontpop+=zm2l(p->q2.val.vmax);
	continue;
      }
      if(c==ASSIGN){
	if(isreg(q1))
	  zreg=p->q1.reg;
	else
	  load_reg(f,zreg,&p->q1,t);
	save_result(f,p);
      }
      continue;
    }
    if(c==ADDRESS){
      load_address(f,zreg,&p->q1,POINTER);
      save_result(f,p);
      continue;
    }
    if(c==MINUS){
      load_reg(f,zreg,&p->q1,t);
      emit(f,"\tneg\t%s\n",regnames[zreg]);
      save_result(f,p);
      continue;
    }
    if(c==TEST){
      load_reg(f,zreg,&p->q1,t);
      emit(f,"\tcmpq\t0,%s\n",regnames[zreg]);
      cmpt=t;
      continue;
    }
    if(c==COMPARE){
      cmpt=t;
      load_reg(f,zreg,&p->q1,t);
      emit(f,"\tcmp%s\t",(p->q2.flags&(KONST|DREFOBJ))==KONST?"q":"");
      emit_obj(f,&p->q2,t);
      emit(f,",%s\n",regnames[zreg]);
      continue;
    }
    if((c>=OR&&c<=AND)||(c>=LSHIFT&&c<=MOD)){
      load_reg(f,zreg,&p->q1,t);
      if(c>=OR&&c<=AND)
	emit(f,"\t%s\t",logicals[c-OR]);
      else{
	if(isreg(q2)){
	  char *s;
	  if(c==LSHIFT){
	    if(!scratch(p,p->q2.reg,0)){
	      if(zreg==t1) i=t2; else i=t1;
	      emit(f,"\tmove\t%s,%s\n",regnames[p->q2.reg],regnames[i]);
	      p->q2.reg=i;
	    }
	    emit(f,"\tneg\t%s\n",regnames[p->q2.reg]);
	  }
	  if(c==LSHIFT||c==RSHIFT)
	    s=(t&UNSIGNED)?"sh":"sha";
	  else
	    s=arithmetics[c-LSHIFT];
	  emit(f,"\t%s\t",s);
	}else
	  emit(f,"\t%sq\t",(c==RSHIFT&&!(t&UNSIGNED))?"shar":arithmetics[c-LSHIFT]);
      }
      emit_obj(f,&p->q2,t);
      emit(f,",%s\n",regnames[zreg]);
      if(c==MOD){
	/* do we need this to wait? TODO: load address before div */
	emit(f,"\tor\t%s,%s\n",regnames[zreg],regnames[zreg]);
	emit(f,"\tmovei\t%d,%s\n",dsp?0xf1a11c:0xf0211c,regnames[zreg]);
	emit(f,"\tload\t(%s),%s\n",regnames[zreg],regnames[zreg]);
#if 1
       	emit(f,"\tcmpq\t0,%s\n",regnames[zreg]);
	if(workaround&1) LALIGN();
	emit(f,"\tjr\tpl ,%s%d\n",labprefix,++label);
	if(workaround==0) emit(f,"\tnop\n");
	if(workaround&2){
	  emit(f,"\tnop\n");
	  emit(f,"\tnop\n");
	}
	emit(f,"\tadd\t%s,%s\n",regnames[p->q2.reg],regnames[zreg]);
	emit(f,"%s%d:\n",labprefix,label);
#endif
      }
      save_result(f,p);
      continue;
    }
    pric2(stdout,p);
    ierror(0);
  }
  function_bottom(f,v,localsize);
  if(stack_valid){
    if(!v->fi) v->fi=new_fi();
    v->fi->flags|=ALL_STACK;
    v->fi->stack1=stack;
  }
  emit(f,"# stacksize=%lu%s\n",zum2ul(stack),stack_valid?"":"+??");
}

int shortcut(int code,int typ)
{
  return 0;
}

int reg_parm(struct reg_handle *m, struct Typ *t,int vararg,struct Typ *d)
{
  int f;
  if(vararg) return 0;
  f=t->flags&NQ;
  if(f<=LONG||f==POINTER||f==FLOAT){
    if(m->gregs>=GPR_ARGS)
      return 0;
    else
      return FIRST_GPR+2+m->gregs++;
  }
  return 0;
}

int handle_pragma(const char *s)
{
}
void cleanup_cg(FILE *f)
{
}
void cleanup_db(FILE *f)
{
  if(f) section=-1;
}

static char *mode_suffix(char *p)
{
  extern Var *cur_funcv;
  int m;
  if(cur_funcv)
    m=mmode(cur_funcv);
  else
    m=dmode;
  if(m==M_GPULOCAL) strcat(p,"gpu");
  if(m==M_DSPLOCAL) strcat(p,"dsp");

  return p;
}

char *use_libcall(int c,int t,int t2)
{
  static char ret[20];
  if(c==MULT) return mode_suffix(strcpy(ret,"__mulint32"));
  if(DIVBUG&&(t&UNSIGNED)){
    if(c==DIV) return mode_suffix(strcpy(ret,"__divuint32"));
    if(c==MOD) return mode_suffix(strcpy(ret,"__moduint32"));
  }
  if(!(t&UNSIGNED)){
    if(c==DIV) return mode_suffix(strcpy(ret,"__divint32"));
    if(c==MOD) return mode_suffix(strcpy(ret,"__modint32"));
  }

  return 0;
}
