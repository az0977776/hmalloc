

#include <stdio.h>
#include <string.h>

#include "xmalloc.h"
#include "hmalloc.h"

void*
xmalloc(size_t bytes)
{
    return hmalloc(bytes);
}

void
xfree(void* ptr)
{
    hfree(ptr);
}

void*
xrealloc(void* prev, size_t bytes)
{
    void* new = xmalloc(bytes);
    memcpy(new, prev, bytes);
    xfree(prev);
    return new;
}

