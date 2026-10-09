/* sscanf.c - sscanf/vsscanf minimo e independente de libc (para Orion68)
 *
 * Suporta: %d %i %u %o %x %X %p %c %s %n %%
 * Modificadores: hh h l ll z j t   |   largura (%5d)   |   supressao (%*d)
 * Nao suporta: %f/%e/%g, %[...]
 *
 * Retorno: numero de campos atribuidos, ou -1 (EOF) se a entrada acabar
 * antes da primeira conversao.
 */
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>

#define L_HH (-2)
#define L_H  (-1)
#define L_N  0
#define L_L  1
#define L_LL 2

static int sc_isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int sc_isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static int sc_digval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str;
    int count = 0;

    while (*fmt) {
        int suppress = 0, width = 0, len = L_N, conv, remaining;

        /* espaco no formato: consome qualquer espaco na entrada */
        if (sc_isspace((unsigned char)*fmt)) {
            while (sc_isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }

        /* caractere literal */
        if (*fmt != '%') {
            if (*s != *fmt) goto done;
            s++; fmt++;
            continue;
        }
        fmt++; /* pula '%' */

        if (*fmt == '*') { suppress = 1; fmt++; }

        while (sc_isdigit((unsigned char)*fmt))
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'h': fmt++; if (*fmt == 'h') { fmt++; len = L_HH; } else len = L_H; break;
        case 'l': fmt++; if (*fmt == 'l') { fmt++; len = L_LL; } else len = L_L; break;
        case 'z': case 'j': case 't': fmt++; len = L_L; break;
        }

        conv = (unsigned char)*fmt;
        if (conv == 0) goto done;
        fmt++;

        if (conv == '%') {
            while (sc_isspace((unsigned char)*s)) s++;
            if (*s != '%') goto done;
            s++;
            continue;
        }

        if (conv == 'n') {
            if (!suppress) {
                int n = (int)(s - str);
                switch (len) {
                case L_HH: *va_arg(ap, signed char *) = (signed char)n; break;
                case L_H:  *va_arg(ap, short *) = (short)n; break;
                case L_L:  *va_arg(ap, long *) = n; break;
                case L_LL: *va_arg(ap, long long *) = n; break;
                default:   *va_arg(ap, int *) = n; break;
                }
            }
            continue;
        }

        if (conv != 'c')
            while (sc_isspace((unsigned char)*s)) s++;

        if (*s == 0) {
            if (count == 0) return -1; /* EOF antes da 1a conversao */
            goto done;
        }

        remaining = width ? width : INT_MAX;

        switch (conv) {
        case 'c': {
            int w = width ? width : 1;
            char *p = suppress ? NULL : va_arg(ap, char *);
            while (w-- > 0) {
                if (*s == 0) goto done;
                if (p) *p++ = *s;
                s++;
            }
            break;
        }
        case 's': {
            char *p = suppress ? NULL : va_arg(ap, char *);
            while (remaining > 0 && *s && !sc_isspace((unsigned char)*s)) {
                if (p) *p++ = *s;
                s++; remaining--;
            }
            if (p) *p = 0;
            break;
        }
        case 'd': case 'i': case 'u': case 'o':
        case 'x': case 'X': case 'p': {
            int base, neg = 0, ndig = 0, d;
            unsigned long long val = 0;

            base = (conv == 'd' || conv == 'u') ? 10 :
                   (conv == 'o') ? 8 :
                   (conv == 'i') ? 0 : 16;

            if ((*s == '-' || *s == '+') && remaining > 1) {
                neg = (*s == '-');
                s++; remaining--;
            }

            /* prefixo 0x (so se vier digito hex depois) */
            if ((base == 0 || base == 16) && remaining >= 3 &&
                s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
                sc_digval((unsigned char)s[2]) >= 0 && sc_digval((unsigned char)s[2]) < 16) {
                s += 2; remaining -= 2;
                base = 16;
            } else if (base == 0) {
                base = (s[0] == '0') ? 8 : 10;
            }

            while (remaining > 0 && (d = sc_digval((unsigned char)*s)) >= 0 && d < base) {
                val = val * (unsigned)base + (unsigned)d;
                s++; remaining--; ndig++;
            }
            if (ndig == 0) goto done; /* falha de casamento */

            if (neg) val = (unsigned long long)(-(long long)val);

            if (!suppress) {
                if (conv == 'p') {
                    *va_arg(ap, void **) = (void *)(unsigned long)val;
                } else if (conv == 'd' || conv == 'i') {
                    switch (len) {
                    case L_HH: *va_arg(ap, signed char *) = (signed char)val; break;
                    case L_H:  *va_arg(ap, short *) = (short)val; break;
                    case L_L:  *va_arg(ap, long *) = (long)val; break;
                    case L_LL: *va_arg(ap, long long *) = (long long)val; break;
                    default:   *va_arg(ap, int *) = (int)val; break;
                    }
                } else {
                    switch (len) {
                    case L_HH: *va_arg(ap, unsigned char *) = (unsigned char)val; break;
                    case L_H:  *va_arg(ap, unsigned short *) = (unsigned short)val; break;
                    case L_L:  *va_arg(ap, unsigned long *) = (unsigned long)val; break;
                    case L_LL: *va_arg(ap, unsigned long long *) = val; break;
                    default:   *va_arg(ap, unsigned int *) = (unsigned int)val; break;
                    }
                }
            }
            break;
        }
        default:
            goto done; /* conversao nao suportada */
        }

        if (!suppress) count++;
    }

done:
    return count;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
