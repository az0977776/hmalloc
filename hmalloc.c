
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <assert.h>

#include "hmalloc.h"

/*
  typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
  } hm_stats;
*/

// The head of the free list
static long* hmallocFreeListHead;

// The size of one page
const size_t PAGE_SIZE = 4096;
// The minimum allocatable size (this must fit the size of this chunk and the pointer to the next chunk)
const static size_t HMALLOC_MIN_CHUNK_SIZE = sizeof(size_t) + sizeof(long*);
// The threshold after which a hmalloc()/free() call goes straight to m(un)map()
const static size_t HMALLOC_SYSCALL_THRESHOLD = 4096;
static hm_stats stats; // This initializes the stats to 0.

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

// To calculate the length of the free list
long
free_list_length(long* listHead)
{
    // base case: return 0
    if (!listHead)
    {
        return 0;
    }
    // get the data at the next field and recur on that data as a long*
    long* listHeadNext = (long*) ((char*) listHead + sizeof(size_t));
    long dataThere = *listHeadNext;
    return free_list_length((long*)dataThere) + 1;
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length(hmallocFreeListHead);
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length(hmallocFreeListHead);
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

// for some reason, VSCode does not acknowledge the existence of MAP_ANONYMOUS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0xDEDBEEF
#endif

// To determine if the given size is too small for an allocation
int remainingSpaceIsTooSmall(size_t size)
{
    return (size < HMALLOC_MIN_CHUNK_SIZE);
}

// To allocate a chunk of the given size within the given free chunk and to 
// return a pointer to the beginning of the remaining free space within the chunk, or 
// a nullptr if there is no space remaining
long* allocateInFreeChunk(long* originalChunk, size_t sizeOfAllocation)
{ 
    // get the size of the current chunk
    size_t sizeOfFreeSection = *originalChunk;
    // make sure it is within lower bounds
    assert(sizeOfAllocation >= HMALLOC_MIN_CHUNK_SIZE);
    assert(sizeOfFreeSection > sizeOfAllocation);
    // determine the size of the new free section 
    size_t sizeOfNewFreeSection = sizeOfFreeSection - sizeOfAllocation; 
    // find the pointer to that location referenced to the free spot position
    long* pointerToNewFreeSection = (long*) ((char*)originalChunk + sizeOfAllocation); 
    // if this size is below the minimum allocation size, give all of the space to this allocation
    if (sizeOfNewFreeSection < HMALLOC_MIN_CHUNK_SIZE)
    { 
        // write the full size of this free section 
        *pointerToNewFreeSection = sizeOfFreeSection;
        // return a nullptr; there is no next possible allocation within the free chunk
        return 0;
    }
    // otherwise, write the size of the new free section there
    *pointerToNewFreeSection = sizeOfNewFreeSection; 
    // return the pointer to that free section
    return pointerToNewFreeSection;
}

// To find the first free chunk in the free list that can fit the given size
long* findSpaceInFreeList(long* currentNode, long* previousNode, size_t size)
{
    // recur on the pointer, checking if the size field >= the given size
    if (currentNode == 0)
    {
        // base case: return 0
        return 0;
    }
    // otherwise: observe the size of the space at the pointer
    size_t sizeAtFreeListCell = *currentNode;
    // see if it can fit the given size 
    if (sizeAtFreeListCell >= size)
    {
        // a match was found- fix up the free list and return the current node
        // if the previous node is not 0, its next needs to be updated to this node's next
        if (previousNode != 0)
        {
            // get the previous node's next and this node's next, as addresses
            long* previousNodeNext = (long*) ((size_t*)previousNode + 1);
            long* currentNodeNext = (long*) ((size_t*)currentNode + 1);
            // link this node out of the chain by setting the previous node's next to the current node's next
            *previousNodeNext = (long) currentNodeNext;
        }
        else
        {
            // in this case, this was the first node
            // get the pointer to the new free section
            

            // the global free list head must be updated to point to this node's next 
            long* currentNodeNext = (long*) ((size_t*)currentNode + 1);
            // 
            hmallocFreeListHead = (long*) *currentNodeNext;
        }
        // return the current node
        return currentNode;
    }
    else 
    {
        // recur on the 'next' field @ (currentNode + sizeof(size_t))
        long* pointerAfterSize = (long*) ( (size_t*)currentNode + 1); 
        return findSpaceInFreeList((long*)*pointerAfterSize, currentNode, size);
    }
}

// To add the given node to the sorted free list (and to keep it sorted in the process)
void addToFreeList(long* nodeToAdd)
{
    // get a pointer to the (size_t) bytes after the given chunk
    long* nodePostSizeWrite = (long*)((size_t*)nodeToAdd + 1);
    // add this to the sorted free list: 
    long* currentFreeListNode = hmallocFreeListHead; 
    long* previousNode = 0;
    // traverse the free list until the node whose address is greater than that of the node to add is found
    // also break on nullptr: that's the end of the list
    while(currentFreeListNode && nodePostSizeWrite >= currentFreeListNode)
    {
        // store the current node as the previous node
        previousNode = currentFreeListNode;
        // get the next node
        long* ptr = currentFreeListNode + 1;
        currentFreeListNode = (long*)*ptr;
    }
    // the two necessary nodes are found: insert the node to add in between 
    // if the previous node is not nullptr, set its next as this node's address
    if (previousNode)
    {
        long* previousNodeNext = previousNode + 1;
        // set its next as this node's address
        *previousNodeNext = (long)nodeToAdd;
        // set the next field of this node to the address of the next node
        *(nodeToAdd + 1) = (long)currentFreeListNode; 

        // coalesce: compare addresses and sizes 
        int didCoalesceUp = 0;
        long previousNodeNextAddr = (long)previousNode + *previousNode;
        if (previousNodeNextAddr == (long)nodeToAdd)
        { 
            // coalesce up: combine the sizes of the two entries and rearrange pointers
            didCoalesceUp = 1;
            long newSize = *previousNode + *nodeToAdd;
            // write this new size to the previous node
            *previousNode = newSize;
            // set the next of the previous node to the address of this node
            *(previousNode + 1) = (long)currentFreeListNode;
        }
        // coalescing down is dependent upon whether or not this node coalesced up 
        // the node to consider is either the current node (if no coalescing has yet happened) or the previous node (if it has)
        long* nodeToCoalesceDown = didCoalesceUp ? previousNode : nodeToAdd;
        long thisNodeNextAddr = *nodeToCoalesceDown + (long)nodeToCoalesceDown; 
        if (thisNodeNextAddr == (long)currentFreeListNode && currentFreeListNode)
        {
            // coalesce down: combine the sizes of the two entries and rearrange pointers
            long newSize = *nodeToCoalesceDown + *currentFreeListNode;
            // write this size to the coalescing node
            *nodeToCoalesceDown = newSize;
            // set the next of the previous node to the next of this node
            long addr = *(currentFreeListNode + 1);
            *(previousNode + 1) = addr;
        }
    }
    else 
    {
        // otherwise, this is the first node
        // write the address of the current head of the free list to this location
        *nodePostSizeWrite = (long)hmallocFreeListHead;
        // update the head of the free list to the size field of the free space
        hmallocFreeListHead = nodeToAdd;
    } 

}

// To append the new page's free space onto the free list and to return false if 
// there is not enough space remaining to fit the smallest possible allocatable object
int addNewPagesToFreeList(long* newPage, size_t size, int numPagesMapped)
{
    // first: calculate the number of free bytes remaining  
    size_t bytesRemaining = (PAGE_SIZE * numPagesMapped) - size;
    // if this is below the minimum mmap-able size for this free list system, return 1
    if (remainingSpaceIsTooSmall(bytesRemaining))
    {
        // don't add anything to the free list- there will be nothing to add, as the remaining page will be entire filled
        return 0;
    }
    // otherwise, get a pointer to the start of the new space
    long* freeSpace = (long*) ((char*)newPage + size);
    // write the remaining space to this location
    *freeSpace = bytesRemaining;
    // add it to the free list
    addToFreeList(freeSpace);
    // done
    return 1;
}

void*
hmalloc(size_t size)
{ 
    stats.chunks_allocated += 1;
    // add the size field to the total size of this allocation
    size += sizeof(size_t); 

    // then, find the pointer to the first free spot in the free list that can fit this allocation
    long* firstFreeSpot = findSpaceInFreeList(hmallocFreeListHead, 0, size);
    
    // if the size is greater than the threshold, just mmap it
    if (size > HMALLOC_SYSCALL_THRESHOLD)
    { 
        int numPagesMapped = (size / PAGE_SIZE) + 1;
        stats.pages_mapped += numPagesMapped;
        long* newPage = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0); 
        // write the size 
        *newPage = size;
        // return a pointer to after the size
        long* newPagePostSize = (long*)((size_t*)newPage + 1);
        return newPagePostSize;
    }
    // if the size is below the minimum allocatable size, round up
    if (size < HMALLOC_MIN_CHUNK_SIZE)
    {
        size = HMALLOC_MIN_CHUNK_SIZE;
    }
    // if the size is below the length of a page and there is space in the free list that can fit it...
    if (size < PAGE_SIZE && firstFreeSpot)
    {
        // allocate the new free chunk  
        long* pointerToNewFreeSection = allocateInFreeChunk(firstFreeSpot, size);
        // if the pointer returned is not 0, add it to the free list
        if (pointerToNewFreeSection)
        {
            addToFreeList(pointerToNewFreeSection);
        } 
        // allocate the space
        // write the size of the allocation to the pointer
        *firstFreeSpot = size;

        // get the pointer to the data, after the size field
        long* pointerAfterSize = (long*) ( (size_t*)firstFreeSpot + 1); 
        // return that to the user
        return pointerAfterSize;
    }
    else
    {
        // gotta map a new page
        int numPagesMapped = (size / PAGE_SIZE) + 1;
        stats.pages_mapped += numPagesMapped;
        long* newPage = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        check_rv((long)newPage);
        // write the size to the first sizeof(size_t) bytes 
        *newPage = size;
        // return a pointer to 1 size_t AFTER the actual pointer
        // cast to size_t to make sure the proper amount is reserved for the size
        long* pointerAfterSize = (long*) ( (size_t*)newPage + 1); 
        // add the rest to the free list
        if (!addNewPagesToFreeList(newPage, size, numPagesMapped))
        {
            // there is not enough left over to make a valid mmap: reserve this whole page
            *newPage = PAGE_SIZE; 
        }
        return pointerAfterSize;
    }  
}

void
hfree(void* item)
{
    stats.chunks_freed += 1;
    // first, find the size of the item
    size_t* sizePointer = (size_t*)item - 1;
    size_t size = *sizePointer;


    // if the size is too big, just munmap it
    if (size > HMALLOC_SYSCALL_THRESHOLD)
    {
        int numPagesMapped = (size / PAGE_SIZE) + 1;
        stats.pages_unmapped += numPagesMapped;
        munmap(item, size);
        return;
    }


    // free this item by adding its space to the free list
    addToFreeList(sizePointer);
}

void*
hrealloc(void* prev, size_t bytes)
{
    size_t old_size = *((size_t*)(prev - sizeof(size_t)));
    void* out = hmalloc(bytes);
    memcpy(out, prev, old_size);
    hfree(prev);
    return out;
}

