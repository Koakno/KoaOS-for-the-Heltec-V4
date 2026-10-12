/*
 * koaos.h -- KoaOS SDK helper library (on top of koaos_abi.h)
 *
 * An app is a C file that defines   int koa_main(void)   and returns an exit code.
 * There is no libc: this header gives you printf-style output, a few string/memory
 * functions and error strings. Everything else is a koa_* call from koaos_abi.h.
 */
#ifndef KOAOS_H
#define KOAOS_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "koaos_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __wasm__
/* freestanding: the SDK supplies these (src/koaos.c) */
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
_Static_assert(sizeof(koa_gps_t) == 72, "koa_gps_t must match the firmware layout");
#else
#include <string.h>   /* host-side builds (tests) */
#endif

/* The one function your app defines. Return value = exit code. */
int koa_main(void);

/* ---- output ---- */
int  koa_putc(char c);                    /* one character to the console */
int  koa_print(const char *s);            /* string, no newline */
int  koa_puts(const char *s);             /* string + newline */
int  koa_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* max 255 bytes per call */
int  koa_snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  koa_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
/*   supported: %d %i %u %x %X %c %s %p %% %f   flags - 0 + space   width, .precision, * ,
 *              length h hh l ll z.   %f takes a double, default 6 decimals, max 9. No %e/%g. */

/* ---- helpers ---- */
const char *koa_strerror(int err);        /* KE_* code -> short text */
int  koa_alive(void);                     /* 1 while running, 0 once the OS asked you to stop; also lets the OS breathe.
                                             Use as:  while (koa_alive()) { ... }  */
#define KOA_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __cplusplus
}
#endif
#endif /* KOAOS_H */
