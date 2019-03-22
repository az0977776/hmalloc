#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>

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

// The number of longs that comprise the bitflag field in each page header
#define PAGE_HEADER_NUM_BITFLAG_LONGS 3

// The number of bytes per page 
#define PAGE_SIZE 4096

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
    // NOTE: a bit value of '0' signifies a FREE chunk; a bit value of '1' signifies an ALLOCATED chunk
    long bitflags[PAGE_HEADER_NUM_BITFLAG_LONGS];                 
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
    long* buckets[BUCKET_NUM_BUCKETS]; 
} bucket_allocator_t;

// ============================== GLOBAL POINTERS =================================== //

// The bucket allocator
bucket_allocator_t bucket_allocator;

// a flag representing whether the allocator has been initialized
char bucket_allocator_has_been_allocated = 0;

// An array of all possible allocation sizes
size_t BUCKET_THRESHOLD_ARRAY[BUCKET_NUM_BUCKETS] = {   
        BUCKET_LINKED_LIST_CELL,    BUCKET_EMPTY_IVEC,      BUCKET_IVEC_DATA_4,     BUCKET_NUM_TASK, 
        BUCKET_IVEC_DATA_16,        BUCKET_IVEC_DATA_32,    BUCKET_IVEC_DATA_64,    BUCKET_TASKS_DATATOP100, 
        BUCKET_IVEC_DATA_128,       BUCKET_IVEC_DATA_256 
    };

// ================================== FUNCTIONS ====================================== //

// To ensure that the return value of a syscall signifies success
// as taken from Prof. Tuck (CS3650 prof)
void
check_rv(long rv)
{
    if (rv == -1) {
        perror("oops");
        fflush(stdout);
        fflush(stderr);
        abort();
    }
}

// To convert the size of the allocation 
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
}

// To determine if an allocation of the given size is large enough to be passed directly to mmap
int shouldDirectlyMmap(size_t size)
{
    return (size >= THRESHOLD_MMAP_SIZE);
}

// To initialize the bucket allocator upon the first xmalloc call
int initBucketAllocator()
{
    // init the bucket allocator itself 
    bucket_allocator_has_been_allocated = 1;

    // init pages for each pointer: favor one-time overhead; instantiate many pages at first? 
    for (int i = 0; i < BUCKET_NUM_BUCKETS; i++)
    {
        // step 1: get the size for this bucket
        size_t size = BUCKET_THRESHOLD_ARRAY[i];
        // step 2: compute all required values for the page header  
        page_header_t header;
        // set the size for each chunk in this page as the size for this bucket
        header.page_chunks_size = size;
        // set the next page pointer to null
        header.next_page = 0;
        // set the bitflag longs to 0: nothing is allocated yet 
        for (int i = 0; i < PAGE_HEADER_NUM_BITFLAG_LONGS; i++)
        {
            header.bitflags[i] = 0;
        }
        // initialize the mutex 
        pthread_mutex_t mutex;
        int rv = pthread_mutex_init(&mutex, 0);
        check_rv(rv);
        // step 3: allocate the page
        long* pagePtr = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // write the header to the page
        memcpy(pagePtr, &header, sizeof(page_header_t));
        // step 4: store the pointer to this page in the bucket table
        bucket_allocator.buckets[i] = pagePtr;
        // done!
    }
    return 1;
}

void*
xmalloc(size_t bytes)
{
    // step -1: determine if the bucket system needs to be instantiated
    // this runs on the very first xmalloc call
    if (bucket_allocator_has_been_allocated == 0)
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
    // the header of the chunk contains the pointer to the beginning of the page
    long* chunk_start = (long*)(ptr - sizeof(long*));
    long* page_header_start = *chunk_start;
    page_header_t page_header = *((page_header_t*)(page_header_start));

    // finds the index of of the chunk on the page
    long chunk_index = ((long)chunk_start - ((long)page_header_start + sizeof(page_header_t))) / page_header.page_chunks_size;
    

    int bitflag_length = 2 ** sizeof(long);
    int bitflag_number = chunk_index / bitflag_length;
    int bitflag_index = chunk_index % bitflag_length;   

    // bitwise and of the bitflag with 1*01*, setting the index bit of this chunk to 0
    pthread_mutex_lock(page_header.page_mutex);
    page_header.bitflags[bitflag_number] &= ~(1 << bitflag_index);
    pthread_mutex_unlock(page_header.page_mutex);
}

void*
xrealloc(void* prev, size_t bytes)
{
    //return opt_realloc(prev, bytes);
    return 0;
}




