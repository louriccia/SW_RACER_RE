// The game's statically linked C runtime (src/Platform/cstdlib.c reverse-hooks these to retail),
// served by the host CRT instead.

#include <windows.h>
#include <ctype.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "standalone.h"

int c__close(int fd) { return _close(fd); }
long c__filelength(int fd) { return _filelength(fd); }
int c__open(const char* filename, int oflag) { return _open(filename, oflag); }
int c__isctype(int c, int desc) { return _isctype(c, desc); }
int c_tolower(int c) { return tolower(c); }
void c__fpmath(void) {}
long c__ftol(double f) { return (long)f; }
void c_exit(int status) { exit(status); }
double c_atof(const char* str) { return atof(str); }
long c_atol(const char* str) { return atol(str); }
int c_atoi(const char* str) { return atoi(str); }
void* c_bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*))
{
    return bsearch(key, base, nmemb, size, compar);
}
void* c_calloc(size_t num_elements, size_t element_size) { return calloc(num_elements, element_size); }
void c_free(void* ptr) { free(ptr); }
void* c_malloc(size_t size) { return malloc(size); }
void* c_realloc(void* ptr, size_t size) { return realloc(ptr, size); }
void c_qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) { qsort(base, nmemb, size, compar); }

// Retail's single-threaded MSVC rand(): never seeded by the game, so it starts at 1 like retail.
static unsigned long c_holdrand = 1;
int c_rand(void)
{
    c_holdrand = c_holdrand * 214013L + 2531011L;
    return (int)((c_holdrand >> 16) & 0x7fff);
}

void c__amsg_exit(int status, const char* msg)
{
    fprintf(standalone_log, "_amsg_exit(%d): %s\n", status, msg ? msg : "");
    fflush(standalone_log);
    exit(255);
}

// MSVC 5 _finddata_t: 32-bit time_t.
intptr_t c__findfirst(const char* filespec, void* fileinfo) { return _findfirst32(filespec, (struct _finddata32_t*)fileinfo); }
int c__findclose(intptr_t handle) { return _findclose(handle); }
int c__findnext(intptr_t handle, void* fileinfo) { return _findnext32(handle, (struct _finddata32_t*)fileinfo); }

int c__snprintf(char* buffer, size_t count, char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = _vsnprintf(buffer, count, format, ap);
    va_end(ap);
    return r;
}
int c__sopen(const char* filename, int oflag, int shflag, int pmode) { return _sopen(filename, oflag, shflag, pmode); }
time_t c__timet_from_ft(const void* ft)
{
    const FILETIME* f = (const FILETIME*)ft;
    unsigned long long t = ((unsigned long long)f->dwHighDateTime << 32) | f->dwLowDateTime;
    return (time_t)((t - 116444736000000000ULL) / 10000000ULL);
}
int c__vsnprintf(char* s, size_t n, const char* format, va_list arg) { return _vsnprintf(s, n, format, arg); }
int c_fclose(FILE* stream) { return fclose(stream); }
int c_feof(FILE* stream) { return feof(stream); }
char* c_fgets(char* str, int n, FILE* stream) { return fgets(str, n, stream); }
FILE* c_fopen(const char* filename, const char* mode) { return fopen(filename, mode); }
size_t c_fread(void* ptr, size_t size, size_t nmemb, FILE* stream) { return fread(ptr, size, nmemb, stream); }
int c_fseek(FILE* stream, long int offset, int origin) { return fseek(stream, offset, origin); }
long int c_ftell(FILE* stream) { return ftell(stream); }
size_t c_fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) { return fwrite(ptr, size, nmemb, stream); }
int c_sprintf(char* str, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vsprintf(str, format, ap);
    va_end(ap);
    return r;
}
int c_ungetc(int c, FILE* stream) { return ungetc(c, stream); }
char* c__strdup(const char* strSource) { return _strdup(strSource); }
int c__stricmp(const char* string1, const char* string2) { return _stricmp(string1, string2); }
size_t c__strncnt(const char* str, size_t count) { return strnlen(str, count); }
int c__strnicmp(const char* str1, const char* str2, size_t n) { return _strnicmp(str1, str2, n); }
void* c_memcpy(void* dest, const void* src, size_t n) { return memcpy(dest, src, n); }
char* c_strchr(const char* str, int c) { return strchr(str, c); }
size_t c_strcspn(const char* str1, const char* str2) { return strcspn(str1, str2); }
char* c_strlwr(char* str) { return _strlwr(str); }
char* c_strncat(char* dest, const char* src, size_t n) { return strncat(dest, src, n); }
int c_strncmp(const char* str1, const char* str2, size_t n) { return strncmp(str1, str2, n); }
char* c_strncpy(char* dest, const char* src, size_t n) { return strncpy(dest, src, n); }
char* c_strpbrk(const char* str1, const char* str2) { return strpbrk(str1, str2); }
char* c_strrchr(const char* str, int c) { return strrchr(str, c); }
size_t c_strspn(const char* str1, const char* str2) { return strspn(str1, str2); }
char* c_strstr(const char* haystack, const char* needle) { return strstr(haystack, needle); }
char* c_strtok(char* str, const char* delim) { return strtok(str, delim); }
char* c_strupr(char* str) { return _strupr(str); }
void c__tzset(void) { _tzset(); }
int c__snwprintf(wchar_t* str, size_t size, const wchar_t* format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = _vsnwprintf(str, size, format, ap);
    va_end(ap);
    return r;
}
unsigned char* c__mbschr(unsigned char* str, unsigned int c) { return (unsigned char*)strchr((char*)str, (int)c); }
size_t c__wcsncnt(const wchar_t* str, size_t count) { return wcsnlen(str, count); }
wchar_t* c_fgetws(wchar_t* ws, int num, FILE* stream) { return fgetws(ws, num, stream); }
int c_mbtowc(wchar_t* pwc, const char* str, size_t n) { return mbtowc(pwc, str, n); }
size_t c_wcslen(const wchar_t* str) { return wcslen(str); }
wchar_t* c_wcsncpy(wchar_t* dest, const wchar_t* src, size_t n) { return wcsncpy(dest, src, n); }
wchar_t* c_wcsrchr(const wchar_t* str, wchar_t ch) { return wcsrchr(str, ch); }
wchar_t* c_wcstok(wchar_t* str, const wchar_t* delim) { return wcstok(str, delim, NULL); }
int c_wctomb(char* s, wchar_t wc) { return wctomb(s, wc); }
size_t c_wcstombs(char* dst, const wchar_t* src, size_t len) { return wcstombs(dst, src, len); }


// MSVC's _ftol as retail's code calls it: the value in ST0 (popped), the truncated result in
// EDX:EAX. c__ftol above is the C-convention stand-in for src callers; retail's own calls land here
// (standalone_hook.c points 0x0049ed20 at it).
#if defined(__i386__)
__asm__(".globl _standalone_ftol\n"
        "_standalone_ftol:\n"
        "    subl $12, %esp\n"
        "    fnstcw 8(%esp)\n"
        "    movzwl 8(%esp), %eax\n"
        "    orl $0x0c00, %eax\n"
        "    movw %ax, 10(%esp)\n"
        "    fldcw 10(%esp)\n"
        "    fistpll (%esp)\n"
        "    fldcw 8(%esp)\n"
        "    movl (%esp), %eax\n"
        "    movl 4(%esp), %edx\n"
        "    addl $12, %esp\n"
        "    ret\n");
#endif
