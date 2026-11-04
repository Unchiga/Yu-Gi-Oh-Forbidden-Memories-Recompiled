#ifndef MEMORIES_TRANSLATED_LIBC_H
#define MEMORIES_TRANSLATED_LIBC_H
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
long GuestRuntime_strtol(const char *, char **, int);
unsigned long GuestRuntime_strtoul(const char *, char **, int);
double GuestRuntime_strtod(const char *, char **);
int GuestRuntime_atoi(const char *);
char *GuestRuntime_fgets(char *, int, FILE *);
size_t GuestRuntime_fread(void *, size_t, size_t, FILE *);
size_t GuestRuntime_fwrite(const void *, size_t, size_t, FILE *);
int GuestRuntime_fseek(FILE *, long, int);
long GuestRuntime_ftell(FILE *);
int GuestRuntime_fclose(FILE *);
int GuestRuntime_isalnum(int);
int GuestRuntime_isdigit(int);
int GuestRuntime_isspace(int);
int GuestRuntime_tolower(int);
void *GuestRuntime_memcpy(void *, const void *, size_t);
void *GuestRuntime_memmove(void *, const void *, size_t);
void *GuestRuntime_memset(void *, int, size_t);
void GuestRuntime_bzero(void *, size_t);
int GuestRuntime_memcmp(const void *, const void *, size_t);
size_t GuestRuntime_strlen(const char *);
int GuestRuntime_strcmp(const char *, const char *);
int GuestRuntime_strncmp(const char *, const char *, size_t);
char *GuestRuntime_strcpy(char *, const char *);
char *GuestRuntime_strncpy(char *, const char *, size_t);
char *GuestRuntime_strcat(char *, const char *);
char *GuestRuntime_strncat(char *, const char *, size_t);
void GuestRuntime_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *GuestRuntime_bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int GuestRuntime_printf(const char *, ...);
int GuestRuntime_sprintf(char *, const char *, ...);
void *GuestRuntime___memcpy_chk(void *, const void *, size_t, size_t);
void *GuestRuntime___memmove_chk(void *, const void *, size_t, size_t);
void *GuestRuntime___memset_chk(void *, int, size_t, size_t);
char *GuestRuntime___strcpy_chk(char *, const char *, size_t);
char *GuestRuntime___strncpy_chk(char *, const char *, size_t, size_t);
char *GuestRuntime___strcat_chk(char *, const char *, size_t);
int GuestRuntime_snprintf(char *, size_t, const char *, ...);
int GuestRuntime_vsnprintf(char *, size_t, const char *, va_list);
int GuestRuntime___snprintf_chk(char *, size_t, int, size_t, const char *, ...);
int GuestRuntime___vsnprintf_chk(char *, size_t, int, size_t, const char *, va_list);
int GuestRuntime___sprintf_chk(char *, int, size_t, const char *, ...);
char *GuestRuntime_strchr(const char *, int);
char *GuestRuntime_strrchr(const char *, int);
size_t GuestRuntime_strnlen(const char *, size_t);
size_t GuestRuntime_strcspn(const char *, const char *);
size_t GuestRuntime_strspn(const char *, const char *);
char *GuestRuntime_strstr(const char *, const char *);
void *GuestRuntime_memchr(const void *, int, size_t);
#endif
