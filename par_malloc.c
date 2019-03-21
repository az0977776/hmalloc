#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#include "xmalloc.h"

        // temporary
        #include <stdio.h>

// because vscode doesn't recognize MAP_ANONYMOUS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0xDEADBEEF
#endif


// ============================== OTHER CONSTANTS =================================== //

// The mmap allocation size threshold
const int THRESHOLD_MMAP_SIZE = 4096;

#define BUCKET_NUM_BUCKETS 10

// ======================== BUCKET THRESHOLD CONSTANTS ============================== //


// The size of a linked list node allocation
//                                 (16 = sizeof(cell))
#define BUCKET_LINKED_LIST_CELL 16 + sizeof(long*)

// The size of an empty ivec
//                           (24 = sizeof(ivec))
#define BUCKET_EMPTY_IVEC 24 + sizeof(long*)

// The size of ivec->data when ivec->cap == 4
//                            (32 = sizeof(long) * 4)
#define BUCKET_IVEC_DATA_4 32 + sizeof(long*)

// The size of tasks[ii] AND ivec->data when ivec->cap == 8
//                         (64 = sizeof(num_task))
//                         (64 = sizeof(long) * 8)
#define BUCKET_NUM_TASK 64 + sizeof(long*)

// The size of ivec->data when ivec->cap == 16
//                             (128 = sizeof(long) * 16)
#define BUCKET_IVEC_DATA_16 128 + sizeof(long*)

// The size of ivec->data when ivec->cap == 32
//                             (256 = sizeof(long) * 32)
#define BUCKET_IVEC_DATA_32 256 + sizeof(long*)

// The size of ivec->data when ivec->cap == 64
//                             (512 = sizeof(long) * 64)
#define BUCKET_IVEC_DATA_64 512 + sizeof(long*)

// The size of tasks when data_top = 100 (this is the case for some tests)
//       (sizeof(num_tasks*)) = 8;  800 = 8 * data_top
#define BUCKET_TASKS_DATATOP100 800 + sizeof(long*) 

// The size of ivec->data when ivec->cap == 128
//                              (1024 = sizeof(long) * 128)
#define BUCKET_IVEC_DATA_128 1024 + sizeof(long*)

// The size of ivec->data when ivec->cap == 256
//                              (2048 = sizeof(long) * 256)
#define BUCKET_IVEC_DATA_256 2048 + sizeof(long*)

// Anything above this is larger than THRESHOLD_MMAP_SIZE and will be directed to mmap directly



// ================================== TYPEDEFS ========================================= //

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

// The bucket system consists of an array of long pointers
typedef struct bucket_allocator_t {
    // the array of pointers to linked lists of pages for each size
    long buckets[BUCKET_NUM_BUCKETS];
} bucket_allocator_t;

// ============================== GLOBAL POINTERS =================================== //

// The bucket allocator pointer
bucket_allocator_t* bucket_allocator = 0;

size_t BUCKET_THRESHOLD_ARRAY[BUCKET_NUM_BUCKETS] = {   
        BUCKET_LINKED_LIST_CELL,    BUCKET_EMPTY_IVEC,      BUCKET_IVEC_DATA_4,     BUCKET_NUM_TASK, 
        BUCKET_IVEC_DATA_16,        BUCKET_IVEC_DATA_32,    BUCKET_IVEC_DATA_64,    BUCKET_TASKS_DATATOP100, 
        BUCKET_IVEC_DATA_128,       BUCKET_IVEC_DATA_256 
    };

// ======================================= CODE ======================================= //

// To convert the size of the allocation
// this is one *ugly* function 
int sizeToBucketIndex(size_t size)
{ 
    for (int i = 0; i < BUCKET_NUM_BUCKETS; i++)
    {
        if (BUCKET_THRESHOLD_ARRAY[i] == size)
        {
            return i;
        }
    }
    assert(0);
    // //switch-case it at first
    // switch (size) {
    //     case BUCKET_LINKED_LIST_CELL: 
    //         return 0;
    //     case BUCKET_EMPTY_IVEC: 
    //         return 1;
    //     case BUCKET_IVEC_DATA_4:
    //         return 2;
    //     case BUCKET_NUM_TASK:
    //         return 3;
    //     case BUCKET_IVEC_DATA_16:
    //         return 4;
    //     case BUCKET_IVEC_DATA_32:
    //         return 5;
    //     case BUCKET_IVEC_DATA_64:
    //         return 6;
    //     case BUCKET_TASKS_DATATOP100:
    //         return 7;
    //     case BUCKET_IVEC_DATA_128:
    //         return 8;
    //     case BUCKET_IVEC_DATA_256:
    //         return 9;
    //     default:
    //         assert(0);
    // }
}

// To determine if an allocation of the given size is large enough to be passed directly to mmap
int shouldDirectlyMmap(size_t size)
{
    return (size >= THRESHOLD_MMAP_SIZE);
}

// To initialize the bucket allocator upon the first xmalloc call
int initBucketAllocator()
{
    // init the threshold array 

    // init pages for each pointer: favor one-time overhead; instantiate many pages at first? 
    for (int i = 0; i < BUCKET_NUM_BUCKETS; i++)
    {
        // get the size for this bucket
        size_t size = BUCKET_THRESHOLD_ARRAY[i];
        printf("%ld\n", size);
    }
    return 0;
}

void*
xmalloc(size_t bytes)
{
    // step -1: determine if the bucket system needs to be instantiated
    // this runs on the very first xmalloc call
    if (bucket_allocator == 0)
    {
        initBucketAllocator();
    }
    // step 0: prepend (sizeof(size_t)) bytes onto the size
    bytes += sizeof(size_t);
    // step 1: determine if this allocation is big enough for a direct syscall allocation
    int mmapDirectly = shouldDirectlyMmap(bytes);
    // if so, do it
    if (mmapDirectly)
    {
        // retrieve the pointer to memory
        long* ptr = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // write the size at the beginning
        *ptr = bytes;
        // return a pointer to the memory after the size field
        long* ptrToUserData = (long*) ((size_t*)ptr + 1);
        return ptrToUserData;
    }
    // step 2: obtain the index of the bucket to use in the bucket list
    int bucketIndex = sizeToBucketIndex(bytes);
    // step 3: get the pointer to the page 

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




