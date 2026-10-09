#include <string.h>

char m68k_locale[32];

const char * __locale_ctype_ptr (void)
{
  memcpy(m68k_locale,"437\0",4);  
  return &m68k_locale[0];
}