#include "vsc.h"

char tg_copyright[]="jrisc scheduler V0.0 (c) in 2025 by Volker Barthelmann";

int sched_init(void)
{
  return 1;
}
void sched_cleanup(void)
{
}
int sched_info(struct sinfo *p)
{
  static int setcc=1;
  char buf[20];int r1,r2,i;char c;
  if(sscanf(p->txt,"l%d:",&i)==1){
    p->label=i;
    p->flags=LABEL;
    return 1;
  }

  BSET(p->pipes,0);
  p->latency=3;

  if(sscanf(p->txt,"# setcc %d",&i)==1){
    setcc=i;
    p->flags=BARRIER;
    return 1;
  }


#if 0
  /* jr cc,lab */
  if(sscanf(p->txt,"jr %c",buf,&c)==1){
    char *s=strchr(p->txt,',')+1;
    if(sscanf(s,"l%d",&i)==1){
      p->label=i;
      p->flags=COND_BRANCH; /* TODO: check for unconditional */
    }
  }
#endif

  if(sscanf(p->txt,"%19s",buf)==1&&!strcmp(buf,"nop")){
    p->flags=BARRIER;
    p->latency=1;
    return 1;
  }

  /* move pc,r1 */
  if(sscanf(p->txt,"move pc,r%d",&r1)==1){
    BSET(p->modifies,r1);
    p->latency=2;
    return 1;
  }

  if(sscanf(p->txt,"%19s",buf)==1&&!strcmp(buf,"movei")){
    char *tp=strrchr(p->txt,',');
    if(tp&&sscanf(tp,",r%d",&r1)==1){
      BSET(p->modifies,r1);
      return 1;
    }
  }

  /* op rx,ry */
  if(sscanf(p->txt,"%19s r%d,r%d",buf,&r1,&r2)==3){
    BSET(p->uses,r1);
    if(strcmp(buf,"move"))
      BSET(p->uses,r2);
    else
      p->latency=2;
    if(strcmp(buf,"cmp"))
      BSET(p->modifies,r2);
    if(strcmp(buf,"move")&&(setcc||!strcmp(buf,"cmp")))
      BSET(p->modifies,CCR);
    if(strcmp(buf,"move")&&strcmp(buf,"cmp"))
      BSET(p->modifies,VCCR);
    if(!strcmp(buf,"addc")||!strcmp(buf,"subc"))
      BSET(p->uses,CCR);
    if(!strcmp(buf,"div"))
      p->latency=18;

    /* TODO: imacn,imultn */

    return 1;
  }

  /* op imm,ry */
  if(sscanf(p->txt,"%19s %d,r%d",buf,&r1,&r2)==3){
    BSET(p->uses,r2);
    if(strcmp(buf,"cmpq"))
      BSET(p->modifies,r2);
    if(strcmp(buf,"addqt")&&strcmp(buf,"subqt")&&strcmp(buf,"moveq")&&strcmp(buf,"movei")&&(setcc||!strcmp(buf,"cmpq")))
      BSET(p->modifies,CCR);
    if(!strcmp(buf,"moveq"))
      p->latency=2;
    return 1;
  }

  /* load (rx),ry */
  if(sscanf(p->txt,"%19s (r%d),r%d",buf,&r1,&r2)==3){
    p->latency=4;
    BSET(p->uses,r1);
    BSET(p->modifies,r2);
    BSET(p->uses,MEM);
    return 1;
  }

  /* load (rx+i),ry */
  if(sscanf(p->txt,"%19s (r%d+%d),r%d",buf,&r1,&i,&r2)==4){
    p->latency=6;
    BSET(p->uses,r1);
    BSET(p->modifies,r2);
    BSET(p->uses,MEM);
    return 1;
  }

  /* load (rx+ri),ry */
  if(sscanf(p->txt,"%19s (r%d+r%d),r%d",buf,&r1,&i,&r2)==4){
    p->latency=6;
    BSET(p->uses,r1);
    BSET(p->uses,i);
    BSET(p->modifies,r2);
    BSET(p->uses,MEM);
    return 1;
  }

  /* store rx,(ry) */
  if(sscanf(p->txt,"%19s r%d,(r%d)",buf,&r1,&r2)==3){
    p->latency=4;
    BSET(p->uses,r1);
    BSET(p->uses,r2);
    BSET(p->modifies,r2);
    BSET(p->modifies,MEM);
    return 1;
  }

  /* store rx,(ry+i) */
  if(sscanf(p->txt,"%19s r%d,(r%d+%d)",buf,&r1,&r2,&i)==4){
    p->latency=6;
    BSET(p->uses,r1);
    BSET(p->uses,r2);
    BSET(p->modifies,r2);
    BSET(p->modifies,MEM);
    return 1;
  }

  /* store rx,(ry+ri) */
  if(sscanf(p->txt,"%19s r%d,(r%d+%d)",buf,&r1,&r2,&i)==4){
    p->latency=6;
    BSET(p->uses,r1);
    BSET(p->uses,r2);
    BSET(p->uses,i);
    BSET(p->modifies,r2);
    BSET(p->modifies,MEM);
    return 1;
  }

  /* op r1 */
  if(sscanf(p->txt,"%19s r%d",buf,&r1)==2){
    BSET(p->uses,r1);
    BSET(p->modifies,r1);
    if(setcc||!strcmp(buf,"test"))
      BSET(p->modifies,CCR);
    return 1;
  }

  p->flags=BARRIER;
  return 1;
}
