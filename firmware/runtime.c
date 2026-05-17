/* SPDX-License-Identifier: MIT */
/*
 * Small libc replacements to reduce the firmware size.
 *
 * These functions are based on Linux tools/include/nolibc/string.h,
 * originally licensed LGPL-2.1 OR MIT. Used here under the MIT option.
 */
 
int memcmp(const void *s1, const void *s2, size_t n)
{
    size_t ofs = 0;
    int c1 = 0;

    while (ofs < n && !(c1 = ((unsigned char *)s1)[ofs] - ((unsigned char *)s2)[ofs]))
    {
        ofs++;
    }

    return c1;
}

void *memmove(void *dst, const void *src, size_t len)
{
    size_t dir, pos;

    pos = len;
    dir = -1;

    if (dst < src) {
        pos = -1;
        dir = 1;
    }

    while (len) {
        pos += dir;
        ((char *)dst)[pos] = ((const char *)src)[pos];
        len--;
    }
    return dst;
}

EXPORT void *memcpy(void *dst, const void *src, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        ((char *)dst)[pos] = ((const char *)src)[pos];
        pos++;
    }
    return dst;
}

void *memset(void *dst, int b, size_t len)
{
    char *p = dst;

    while (len--) {
        /* prevent gcc from recognizing memset() here */
        __asm__ volatile("");
        *(p++) = b;
    }
    return dst;
}

int strcmp(const char *a, const char *b)
{
    unsigned int c;
    int diff;

    while (!(diff = (unsigned char)*a++ - (c = (unsigned char)*b++)) && c)
        ;
    return diff;
}

char *strcpy(char *dst, const char *src)
{
    char *ret = dst;

    while ((*dst++ = *src++));
    return ret;
}

size_t strlen(const char *str)
{
    size_t len;

    for (len = 0; str[len]; len++)
        __asm__("");
    return len;
}
