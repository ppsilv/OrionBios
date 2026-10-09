#ifndef __TIME_H__
#define __TIME_H__

#include <stddef.h>

#ifndef __machine_clock_t_defined
#define _CLOCK_T_   unsigned long   /* clock() */
#endif

typedef _CLOCK_T_   __clock_t;


#ifndef __clock_t_defined
#define __clock_t_defined 1

/*#include <bits/types.h>*/

/* Returned by `clock'.  */
typedef __clock_t clock_t;

#endif

typedef unsigned long time_t;


struct tm
{
    int   tm_sec;
    int   tm_min;
    int   tm_hour;
    int   tm_mday;
    int   tm_mon;
    int   tm_year;
    int   tm_wday;
    int   tm_yday;
    int   tm_isdst;
    #ifdef __TM_ZONE
    const char *__TM_ZONE;
    #endif
};

clock_t    clock (void);
double     difftime (time_t _time2, time_t _time1);
time_t     mktime (struct tm *_timeptr);
time_t     time (time_t *_timer);
size_t     strftime (char *__restrict _s,
                     size_t _maxsize, const char *__restrict _fmt,
                     const struct tm *__restrict _t);



#endif
