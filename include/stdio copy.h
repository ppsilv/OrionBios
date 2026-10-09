#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>

#define _IOFBF 0   /* full buffered  - descarrega só quando o buffer enche */
#define _IOLBF 1   /* line buffered  - descarrega a cada '\n'              */
#define _IONBF 2   /* unbuffered     - descarrega a cada byte/chamada      */

#define EOF (-1)

#define SEEK_SET	0	/* Seek from beginning of file.  */
#define SEEK_CUR	1	/* Seek from current position.  */
#define SEEK_END	2	/* Seek from end of file.  */

typedef struct FILE {
    int             fd;         /* descriptor real, do seu VFS             */
    unsigned char  *buf;        /* buffer alocado (NULL se unbuffered)     */
    size_t          buf_size;   /* tamanho total do buffer                 */
    size_t          buf_pos;    /* quantos bytes já estão pendentes nele   */
    int             buf_mode;   /* _IOFBF / _IOLBF / _IONBF                */
    int             error;      /* flag de erro (ferror)                   */
    int             eof;        /* flag de fim de arquivo (feof)           */
    struct FILE    *next;       /* próximo na lista de arquivos abertos    */
} FILE;

static FILE *stdin, *stdout, *stderr;

/* lista de TODOS os FILE* abertos no momento (stdin/stdout/stderr inclusos) -
 é isso que __stdi*o_flush_all() percorre no exit(), não só os 3 fixos */
extern FILE *__open_files;

void __stdio_register(FILE *f);
void __stdio_unregister(FILE *f);
int fflush(FILE *f);



void putchar(char c);
char getchar(void);

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);

void puts(const char *str);
char* gets_s( char* str, int n );
char kbdhit(void);

int sscanf(const char *str, const char *fmt, ...);

#endif
