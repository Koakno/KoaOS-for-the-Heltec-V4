/* koaos.c -- SDK runtime: entry point, mini libc, printf. Linked into every app. */
#include "koaos.h"

/* ---------- entry point: the firmware looks for an exported _start ---------- */
#ifdef __wasm__
__attribute__((export_name("_start")))
void _start(void) { koa_exit(koa_main()); }

/* ---------- mini libc (built with -fno-builtin so these don't turn into calls to themselves) ---------- */
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *a = d; const unsigned char *b = s;
  while (n--) *a++ = *b++;
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  unsigned char *a = d; const unsigned char *b = s;
  if (a < b) { while (n--) *a++ = *b++; }
  else { a += n; b += n; while (n--) *--a = *--b; }
  return d;
}
void *memset(void *d, int c, size_t n) {
  unsigned char *a = d;
  while (n--) *a++ = (unsigned char)c;
  return d;
}
int memcmp(const void *x, const void *y, size_t n) {
  const unsigned char *a = x, *b = y;
  for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
  return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
    if (!*a) return 0;
  }
  return 0;
}
char *strchr(const char *s, int c) {
  for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
#endif

/* ---------- output ---------- */
static int write_all(const char *p, int n) {
  int done = 0;
  while (done < n) {
    int r = koa_write(1, p + done, n - done);
    if (r <= 0) return done ? done : r;
    done += r;
  }
  return done;
}
int koa_putc(char c) { return write_all(&c, 1); }
int koa_print(const char *s) { return write_all(s, (int)strlen(s)); }
int koa_puts(const char *s) { int a = koa_print(s); int b = koa_putc('\n'); return a < 0 ? a : (b < 0 ? b : a + b); }

/* ---------- printf ---------- */
typedef struct { char *buf; size_t cap, len; } sink_t;     /* len counts everything, even what didn't fit */
static void put(sink_t *s, char c) { if (s->len + 1 < s->cap) s->buf[s->len] = c; s->len++; }

static void emit(sink_t *s, const char *pre, int prel, const char *body, int bl,
                 int width, int left, int zero) {
  int pad = width - prel - bl;
  if (pad < 0) pad = 0;
  if (!left && !zero) while (pad-- > 0) put(s, ' ');
  for (int i = 0; i < prel; i++) put(s, pre[i]);
  if (!left && zero) while (pad-- > 0) put(s, '0');
  for (int i = 0; i < bl; i++) put(s, body[i]);
  if (left) while (pad-- > 0) put(s, ' ');
}

int koa_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  sink_t s = { buf, cap, 0 };
  for (; *fmt; fmt++) {
    if (*fmt != '%') { put(&s, *fmt); continue; }
    fmt++;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
    for (;; fmt++) {
      if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1;
      else if (*fmt == '+') plus = 1; else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else break;
    }
    int width = 0, prec = -1;
    if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
    else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      fmt++; prec = 0;
      if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
      else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    int lng = 0;                                   /* 0 int, 1 long, 2 long long */
    for (;; fmt++) {
      if (*fmt == 'l') lng++; else if (*fmt == 'z') lng = 1;
      else if (*fmt == 'h') { } else break;
    }
    char tmp[72];
    switch (*fmt) {
      case 0: goto done;
      case '%': put(&s, '%'); break;
      case 'c': { char c = (char)va_arg(ap, int); emit(&s, "", 0, &c, 1, width, left, 0); break; }
      case 's': {
        const char *p = va_arg(ap, const char *);
        if (!p) p = "(null)";
        int n = 0;
        while (p[n] && (prec < 0 || n < prec)) n++;
        emit(&s, "", 0, p, n, width, left, 0);
        break;
      }
      case 'd': case 'i': {
        long long v = lng >= 2 ? va_arg(ap, long long) : (lng == 1 ? va_arg(ap, long) : va_arg(ap, int));
        unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
        int n = 0;
        do { tmp[sizeof tmp - 1 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (n < prec && n < (int)sizeof tmp) tmp[sizeof tmp - 1 - n++] = '0';
        char sg[1]; int sl = 0;
        if (v < 0) sg[sl++] = '-'; else if (plus) sg[sl++] = '+'; else if (space) sg[sl++] = ' ';
        emit(&s, sg, sl, tmp + sizeof tmp - n, n, width, left, zero && prec < 0);
        break;
      }
      case 'u': case 'x': case 'X': case 'p': {
        unsigned long long u;
        unsigned base = (*fmt == 'u') ? 10 : 16;
        const char *dig = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
        if (*fmt == 'p') u = (unsigned long long)(uintptr_t)va_arg(ap, void *);
        else u = lng >= 2 ? va_arg(ap, unsigned long long) : (lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned));
        int n = 0;
        do { tmp[sizeof tmp - 1 - n++] = dig[u % base]; u /= base; } while (u);
        while (n < prec && n < (int)sizeof tmp) tmp[sizeof tmp - 1 - n++] = '0';
        int pfx = (*fmt == 'p') || (alt && base == 16 && (n > 1 || tmp[sizeof tmp - 1] != '0'));
        emit(&s, *fmt == 'X' ? "0X" : "0x", pfx ? 2 : 0, tmp + sizeof tmp - n, n, width, left, zero && prec < 0);
        break;
      }
      case 'f': {
        double v = va_arg(ap, double);
        if (prec < 0) prec = 6;
        if (prec > 9) prec = 9;
        char sg[1]; int sl = 0;
        if (v != v) { emit(&s, "", 0, "nan", 3, width, left, 0); break; }
        if (v < 0) { sg[sl++] = '-'; v = -v; } else if (plus) sg[sl++] = '+'; else if (space) sg[sl++] = ' ';
        if (v >= 1e18) { emit(&s, sg, sl, "inf", 3, width, left, 0); break; }   /* too big for this simple formatter */
        unsigned long long scale = 1;
        for (int i = 0; i < prec; i++) scale *= 10;
        unsigned long long ip = (unsigned long long)v;
        unsigned long long fp = (unsigned long long)((v - (double)ip) * (double)scale + 0.5);
        if (fp >= scale) { ip++; fp -= scale; }
        int n = 0;
        if (prec) { for (int i = 0; i < prec; i++) { tmp[sizeof tmp - 1 - n++] = (char)('0' + fp % 10); fp /= 10; } tmp[sizeof tmp - 1 - n++] = '.'; }
        do { tmp[sizeof tmp - 1 - n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < (int)sizeof tmp - 1);
        emit(&s, sg, sl, tmp + sizeof tmp - n, n, width, left, zero);
        break;
      }
      default: put(&s, '%'); put(&s, *fmt); break;      /* unknown conversion: print it as-is */
    }
  }
done:
  if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
  return (int)s.len;
}

int koa_snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int n = koa_vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

int koa_printf(const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  int n = koa_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  return write_all(buf, n);
}

/* ---------- helpers ---------- */
const char *koa_strerror(int e) {
  switch (e) {
    case KE_OK:      return "ok";
    case KE_PERM:    return "not permitted (capability missing in manifest, or not acquired)";
    case KE_BUSY:    return "resource busy";
    case KE_NOENT:   return "not found";
    case KE_INVAL:   return "invalid argument";
    case KE_NOMEM:   return "out of memory";
    case KE_TIMEOUT: return "timeout";
    case KE_KILLED:  return "killed";
    case KE_NOSYS:   return "not implemented";
    case KE_IO:      return "I/O error";
    default:         return e < 0 ? "error" : "ok";
  }
}

int koa_alive(void) { return koa_yield() != KE_KILLED; }
