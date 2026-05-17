#include <string.h>

// Use chk_chr from FatFs R0.14a to keep firmware size down
static int fatfs_chk_chr(const char *str, int chr)
{
    while (*str && *str != chr) str++;
    return *str;
}

#define strchr fatfs_chk_chr
#define dir_read fatfs_dir_read
#include "ff.c"
#undef strchr
#undef dir_read

#include "ffunicode.c"
