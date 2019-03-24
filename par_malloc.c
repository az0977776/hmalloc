#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <math.h>

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

// The number of bits in a long
#define NUM_BITS_PER_LONG (8 * sizeof(long))

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
    struct page_header_t* next_page;
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
    struct page_header_t* page_header_address;
} data_chunk_header_t;                                          //                  8 bytes

// The bucket system consists of an array of long pointers
typedef struct bucket_allocator_t {
    // the array of pointers to linked lists of pages for each size
    page_header_t* buckets[BUCKET_NUM_BUCKETS]; 
} bucket_allocator_t;

// A header for directly mapped pages
typedef struct direct_map_page_t {
    // the size of the page
    size_t size;
    // some thing to distinguish the directly mapped page from the allocated chunks
    long key;
} direct_map_page_t;

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

// To determine the index of the appropriate bucket for the given allocation
int sizeToBucketIndex(size_t size)
{ 
    // could binary search this if we need to since it's sorted
    for (int i = 0; i < BUCKET_NUM_BUCKETS; i++)
    {
        if (BUCKET_THRESHOLD_ARRAY[i] == size)
        {
            return i;
        }
    }
    assert(0); 
}

// To determine the index of the appropriate bucket for the given allocation
int allocationToBucketIndex(long* data)
{ 
    // Get the size: 8 bytes behind the pointer
    size_t size = *(data - sizeof(size_t));
    // return the index
    return sizeToBucketIndex(size);
}

// To determine if an allocation of the given size is large enough to be passed directly to mmap
int largerThanPage(size_t size)
{
    return (size >= THRESHOLD_MMAP_SIZE);
}

// To initialize a new page with chunks of the given size
page_header_t* makeNewPage(size_t size)
{
    // step 1: compute all required values for the page header  
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
    header.page_mutex = mutex;
    // step 2: allocate the page
    long* pagePtr = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    check_rv((long)pagePtr);
    // step 3: write the header to the page
    memcpy(pagePtr, &header, sizeof(page_header_t));
    // return the page pointer
    return (page_header_t*)pagePtr;
}

// To initialize the bucket allocator upon the first xmalloc call
int initBucketAllocator()
{
    // init the bucket allocator itself 
    bucket_allocator_has_been_allocated = 1;

    // init pages for each pointer: favor one-time overhead; instantiate many pages at first? 
    for (int i = 0; i < BUCKET_NUM_BUCKETS; i++)
    {
        // get the size for this bucket
        size_t size = BUCKET_THRESHOLD_ARRAY[i];
        // make the page
        page_header_t* pagePtr = makeNewPage(size);
        // store the pointer to this page in the bucket table
        bucket_allocator.buckets[i] = pagePtr;
        // done!
    }
    return 1;
}

// To determine the index of the first free chunk in memory represented by the given bitflags
long findFirstFreeIndexInBitflags(long* flags)
{
    long bitsPerLong = sizeof(long) * 8;
    // the number of longs per bitflag field is constant; iterate over each
    for (int i = 0; i < PAGE_HEADER_NUM_BITFLAG_LONGS; i++)
    { 
        // get the long to consider
        long toConsider = flags[i];
        // proceed with this long iff it's not -1 (every bit set to 1)
        if (toConsider != -1)
        {
            long ans = 0;
            // find the first zero entry 
            // this is NOT the most efficient way to do this but idk anything about bit twiddling
            while (ans < bitsPerLong)
            {
                if ((toConsider >> ans) % 2 == 0)
                {  
                    // add in the indices from the longs behind this one 
                    return (ans + (i * sizeof(long) * 8));
                }
                ans++;
            }
        }
    }
    // bad!
    assert(0);
}

// To calculate the address of the chunk to allocate within the given page at the given address
long* calculateAddressToAlloc(page_header_t* page, long firstFreeIndex)
{
    long offset = 0;
    // there are (firstFreeIndex) chunks of size (size) before this allocation
    size_t size = page->page_chunks_size;
    offset += (firstFreeIndex * size);
    // add the header size
    offset += sizeof(page_header_t);
    // return the offset plus the page base address
    long pageAddress = (long)page;
    long returnAddress = pageAddress + offset;
    return (long*)returnAddress;
}

// To toggle the bitflag status of the given bitflags at the given index
void toggleBitflags(page_header_t* page_header, long index)
{
    long bitflag_length = NUM_BITS_PER_LONG;
    long bitflag_number = index / bitflag_length;
    long bitflag_index = index % bitflag_length; 
    // xor-ing the bitflag with 
    pthread_mutex_lock(&page_header->page_mutex);  
    page_header->bitflags[bitflag_number] ^= (long)1 << bitflag_index;
    pthread_mutex_unlock(&page_header->page_mutex);   
}

// To allocate a chunk in the given page, or to create a new page of the same allocation size if this page is full
void* allocInPage(page_header_t* page, size_t sizeOfAllocation)
{  
    // step 1: get the bitflags
    long* flags = page->bitflags;
    // step 2: find the index of the first free chunk
    long firstFreeIndex = findFirstFreeIndexInBitflags(flags);
    if (firstFreeIndex > (4016 / page->page_chunks_size))
    {
        assert(0);
    }
    
    // step 3: calculate the address of the memory to allocate
    long* addressToAlloc = calculateAddressToAlloc(page, firstFreeIndex);
    if ((long)addressToAlloc > (long)page + 0x1000)
    {
        assert(0);
    }
    
    // step 4: write the header to the chunk
    data_chunk_header_t* chunkHeader = (data_chunk_header_t*)addressToAlloc;
    // write the page addres at the chunk
    chunkHeader->page_header_address = page;
    // step 5: change the bitflags 
    toggleBitflags(page, firstFreeIndex);  
    // return the chunk header pointer
    return chunkHeader;
}

// To determine if there is any free space in the given page
int isSpaceInPage(page_header_t* pageHeader)
{
    // step 1: get the page size
    size_t chunkSize = pageHeader->page_chunks_size;
    // step 2: determine the number of chunks in the page
    long usablePageSpace = 4096 - sizeof(page_header_t);
    long numChunks = usablePageSpace / chunkSize;  // round down- int division is good
    // step 3: check that number of bits
    for (int i = 0; i < numChunks; i++)
    { 
        // get the long to check 
        long bitfieldLongToCheck = i / NUM_BITS_PER_LONG;
        // get the index within the long to check 
        long bitfieldIndex = i % NUM_BITS_PER_LONG;
        // check if the bit at that location is 0 
        long longToCheck = pageHeader->bitflags[bitfieldLongToCheck];
        long is0 = (longToCheck >> bitfieldIndex) & 1; 
        // if this is 0, the bit at that location is 0; there is space
        if (!is0)
        {
            if (pageHeader->bitflags[0] == 2147483647 && pageHeader->page_chunks_size == 72)
            {
                long adsf = 1;
                (void*)adsf;
            }
            if (pageHeader->page_chunks_size == 72)
            {
                long adsf = 1;
                (void*)adsf;
            }
            return 1;
        }
    }
    // no space; return 0
    return 0;
}

// To return the first free page in the allocator of the given size
page_header_t* findFirstFreePageOfSize(size_t size)
{
    // step 1: obtain the index of the bucket to use in the bucket list
    int bucketIndex = sizeToBucketIndex(size);
    // step 2: get the first page of that size
    page_header_t* pageHeader = bucket_allocator.buckets[bucketIndex]; 
    // step 3: iterate over the linked list of pages until one with free space is found
    while(!isSpaceInPage(pageHeader))
    {
        page_header_t* previousHeader = pageHeader;
        pageHeader = pageHeader->next_page;
        // if this is null, make a new page
        // must lock here!
        pthread_mutex_lock(&previousHeader->page_mutex);
        if (!pageHeader)
        {
            page_header_t* newPage = makeNewPage(previousHeader->page_chunks_size);
            // link this page to the previous page 
            previousHeader->next_page = newPage;
            // free the lock
            pthread_mutex_unlock(&previousHeader->page_mutex);
            // return the new page
            pageHeader = newPage;
            break;
        }
        pthread_mutex_unlock(&previousHeader->page_mutex);
    }
    // return that page
    return pageHeader;
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
    int mmapDirectly = largerThanPage(bytes);
    // if so, do it
    if (mmapDirectly)
    {
        // retrieve the pointer to memory, adding sizeof(long) to the mapping because of the key
        direct_map_page_t* direct_page = mmap(0, bytes + sizeof(long), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // write the size at the beginning
        direct_page->size = bytes;
        direct_page->key = 1234567;
        // return a pointer to the memory after the size field
        return ((void*)direct_page + sizeof(direct_map_page_t));
    }
    
    // step 2: get the pointer to the first free for this size allocation
    page_header_t* firstFreePage = findFirstFreePageOfSize(bytes);  
    // step 3: allocate the data 
    long* ptrToHeaderOfAllocData = allocInPage(firstFreePage, bytes);
    // step 4: return a pointer to the data after the header 
    long* ptrToData = (long*) ((long)ptrToHeaderOfAllocData + sizeof(data_chunk_header_t));
    return ptrToData;
}



void
xfree(void* ptr)
{
    direct_map_page_t direct_map = *((direct_map_page_t*)(ptr - sizeof(direct_map_page_t)));
    // check if the allocated memory is directly mapped 
    if (direct_map.key == 1234567) {
        munmap(&direct_map, direct_map.size);
    }

    // the header of the chunk contains the pointer to the beginning of the page
    long* chunk_start = (long*)(ptr - sizeof(long*));
    long* page_header_start = (long*)*chunk_start;
    page_header_t page_header = *((page_header_t*)(page_header_start));
    page_header_t* ph = (page_header_t*) page_header_start;
    // finds the index of of the chunk on the page
    long chunk_index = ((long)chunk_start - ((long)page_header_start + sizeof(page_header_t))) / page_header.page_chunks_size;

    int bitflag_length = 1 << sizeof(long);
    int bitflag_number = chunk_index / bitflag_length;
    int bitflag_index = chunk_index % bitflag_length;   

    // bitwise and of the bitflag with 1*01*, setting the index bit of this chunk to 0
    pthread_mutex_lock(&page_header.page_mutex);
    page_header.bitflags[bitflag_number] &= ~((long)1 << bitflag_index);
    pthread_mutex_unlock(&page_header.page_mutex);
}

void*
xrealloc(void* prev, size_t bytes)
{
    data_chunk_header_t old_chunk_header = *((data_chunk_header_t*)(prev - sizeof(data_chunk_header_t)));
    page_header_t old_page_header = *(old_chunk_header.page_header_address);
    int old_size = old_page_header.page_chunks_size; 
    void* out = xmalloc(bytes);
    memcpy(out, prev, old_size);
    xfree(prev);
    return out;
}




