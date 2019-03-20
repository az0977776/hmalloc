
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>

#include "hmalloc.h"

int main(int c, char** v)
{ 
    long* a = hmalloc(128);
    *a = 3;
    long* b = hmalloc(128);
    *b = 5;
    hfree(a);
    hfree(b);
    return 0;
}