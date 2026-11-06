/* Lia — memória, strings e buffer de saída. Sem libc. */
#include "lia.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t lia_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int lia_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int lia_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

int lia_escrever_tudo(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        long r = lia_write(fd, c, n);
        if (r == -EINTR)
            continue;
        if (r <= 0)
            return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

void b_init(Buf *b, char *mem, size_t cap, int fd)
{
    b->p = mem;
    b->cap = cap;
    b->n = 0;
    b->fd = fd;
    b->err = 0;
}

void b_flush(Buf *b)
{
    if (b->fd >= 0 && b->n) {
        if (lia_escrever_tudo(b->fd, b->p, b->n) < 0)
            b->err = 1;
        b->n = 0;
    }
}

void b_put(Buf *b, const void *s, size_t n)
{
    if (b->n + n > b->cap) {
        if (b->fd < 0) {
            b->err = 1;
            return;
        }
        b_flush(b);
        if (n > b->cap) {
            if (lia_escrever_tudo(b->fd, s, n) < 0)
                b->err = 1;
            return;
        }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void b_s(Buf *b, const char *s) { b_put(b, s, lia_strlen(s)); }
void b_c(Buf *b, char c) { b_put(b, &c, 1); }

void b_u(Buf *b, u64 v)
{
    char t[20];
    int i = 20;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    b_put(b, t + i, (size_t)(20 - i));
}

void b_i(Buf *b, i64 v)
{
    if (v < 0) {
        b_c(b, '-');
        b_u(b, (u64)0 - (u64)v);
    } else {
        b_u(b, (u64)v);
    }
}

void b_oct(Buf *b, u32 v)
{
    char t[11];
    int i = 11;
    do {
        t[--i] = (char)('0' + (v & 7));
        v >>= 3;
    } while (v);
    b_put(b, t + i, (size_t)(11 - i));
}

void b_pad(Buf *b, const char *s, size_t largura)
{
    size_t n = lia_strlen(s);
    b_put(b, s, n);
    do {
        b_c(b, ' ');
        n++;
    } while (n < largura);
}

/* Escapa para JSON; bytes que não formam UTF-8 válido viram U+FFFD. */
void b_json(Buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    b_c(b, '"');
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            b_c(b, '\\');
            b_c(b, (char)c);
            s++;
        } else if (c < 0x20) {
            b_s(b, "\\u00");
            b_c(b, hex[c >> 4]);
            b_c(b, hex[c & 15]);
            s++;
        } else if (c < 0x80) {
            b_c(b, (char)c);
            s++;
        } else {
            int len = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2 && c < 0xE0) ? 2 : 0;
            int ok = len > 0 && c < 0xF8;
            for (int i = 1; ok && i < len; i++)
                if (((unsigned char)s[i] & 0xC0) != 0x80)
                    ok = 0;
            if (ok) {
                b_put(b, s, (size_t)len);
                s += len;
            } else {
                b_s(b, "\\ufffd");
                s++;
            }
        }
    }
    b_c(b, '"');
}

/* AAAA-MM-DD HH:MM:SS UTC (algoritmo civil-from-days) */
void b_data_utc(Buf *b, i64 t)
{
    i64 dias = t / 86400, resto = t % 86400;
    if (resto < 0) {
        resto += 86400;
        dias--;
    }
    dias += 719468;
    i64 era = (dias >= 0 ? dias : dias - 146096) / 146097;
    i64 doe = dias - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1;
    i64 m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    i64 campos[6] = {y, m, d, resto / 3600, (resto % 3600) / 60, resto % 60};
    static const char sep[6] = {'-', '-', ' ', ':', ':', 0};
    for (int i = 0; i < 6; i++) {
        if (i > 0 && campos[i] < 10 && i != 0)
            b_c(b, '0');
        b_u(b, (u64)campos[i]);
        if (sep[i])
            b_c(b, sep[i]);
    }
    b_s(b, " UTC");
}
