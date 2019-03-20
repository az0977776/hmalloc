#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "xmalloc.h"

// A metadata header appended onto the beginning of every page allocated in the bucket system
// Contains all necessary data for allocating and freeing chunks within this page
typedef struct page_header_t {
    // the size of each chunk within this page                                      8 bytes
    size_t page_chunks_size;
    // the pointer to the next page in the list                                     8 bytes
    long* next_page;
    // the mutex for this page                                                      40 bytes (!)
    pthread_mutex_t page_mutex;                             
    // the set of longs containing the bitflags for the free status of this page    16 bytes
    long* bitflags;                 
} page_header_t;                                                //                  72 bytes (!!!)
// percent data usable = 1 - (72 / 4096) = 98.2%

// A metadata header appended onto the top of every piece of data allocated within a page in the bucket system
// Contains the address of the start of the page 
typedef struct data_chunk_header_t {
    // the address of the start of this page (address to the page_header_t)         8 bytes
    long* page_header_address;
} data_chunk_header_t;                                          //                  8 bytes




void*
xmalloc(size_t bytes)
{
    //return opt_malloc(bytes);
    return 0;
}

void
xfree(void* ptr)
{
    //opt_free(ptr);
}

void*
xrealloc(void* prev, size_t bytes)
{
    //return opt_realloc(prev, bytes);
    return 0;
}




