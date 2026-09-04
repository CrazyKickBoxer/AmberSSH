/* os_alloc.c — the never-fail allocators the X.Org core expects from its os
 * layer. Original AmberSSH file; replaces the allocation half of
 * xserver/os/utils.c.
 *
 * "XNF" is "X no-fail": the core calls these where running out of memory
 * has no recovery, and the contract is that they either return memory or
 * end the server. FatalError does the ending. */
#include <dix-config.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

void *
XNFalloc(unsigned long amount)
{
    void *ptr = malloc(amount);
    if (!ptr)
        FatalError("Out of memory");
    return ptr;
}

void *
XNFcalloc(unsigned long amount)
{
    void *ptr = calloc(1, amount);
    if (!ptr)
        FatalError("XNFcalloc: Out of memory");
    return ptr;
}

void *
XNFrealloc(void *ptr, unsigned long amount)
{
    void *ret = realloc(ptr, amount);
    if (!ret)
        FatalError("XNFrealloc: Out of memory");
    return ret;
}

void *
XNFreallocarray(void *ptr, size_t nmemb, size_t size)
{
    void *ret = xreallocarray(ptr, nmemb, size);
    if (!ret)
        FatalError("XNFreallocarray: Out of memory");
    return ret;
}

void *
XNFcallocarray(size_t nmemb, size_t size)
{
    void *ret = calloc(nmemb, size);
    if (!ret)
        FatalError("XNFcallocarray: Out of memory");
    return ret;
}

char *
Xstrdup(const char *s)
{
    if (s == NULL)
        return NULL;
    return strdup(s);
}

char *
XNFstrdup(const char *s)
{
    char *ret;
    if (s == NULL)
        return NULL;
    ret = strdup(s);
    if (!ret)
        FatalError("XNFstrdup: Out of memory");
    return ret;
}
