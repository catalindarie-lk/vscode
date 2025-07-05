#ifndef MEM_POOL_H
#define MEM_POOL_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <windows.h>           // For CRITICAL_SECTION and related functions

typedef struct {
    char* memory;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)

    uint64_t block_size;        // Size of each block in bytes
    uint64_t block_count;       // Total number of blocks in the pool

    CRITICAL_SECTION mutex;     // Mutex for thread safety
} MemPool;

//--------------------------------------------------------------------------------------------------------------------------
void pool_init(MemPool* pool);

void* pool_alloc(MemPool* pool);

void pool_free(MemPool* pool, void* ptr);

void pool_destroy(MemPool* pool);

#endif // MEM_POOL_H