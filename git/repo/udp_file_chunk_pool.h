#ifndef _UDP_FILE_CHUNK_POOL_H
#define _UDP_FILE_CHUNK_POOL_H

#include "UDP_lib.h"

#define BLOCK_SIZE_CHUNK              (FILE_FRAGMENT_SIZE * 64)
#define BLOCK_COUNT_CHUNK             (4096)

typedef struct {
    char* memory;             // Raw memory buffer
    int free_head;               // Index of the first free block
    int next[BLOCK_COUNT_CHUNK];       // Next free block indices
    BOOL used[BLOCK_COUNT_CHUNK];      // Usage flags (optional, for safety/debugging)
    CRITICAL_SECTION mutex;
} MemPoolFileChunk;

//--------------------------------------------------------------------------------------------------------------------------
void pool_init_chunk(MemPoolFileChunk* pool) {
    pool->memory = (char*)malloc(BLOCK_SIZE_CHUNK * BLOCK_COUNT_CHUNK);
    pool->free_head = 0;

    for (int i = 0; i < BLOCK_COUNT_CHUNK - 1; i++) {
        pool->next[i] = i + 1;
        pool->used[i] = FALSE;
    }
    pool->next[BLOCK_COUNT_CHUNK - 1] = -1; // End of free list
    pool->used[BLOCK_COUNT_CHUNK - 1] = FALSE;
    InitializeCriticalSection(&pool->mutex);
    return;
}

void* pool_alloc_chunk(MemPoolFileChunk* pool) {
    EnterCriticalSection(&pool->mutex);
    if (pool->free_head == -1){
        LeaveCriticalSection(&pool->mutex);
        return NULL; // Pool exhausted
    }        
    int index = pool->free_head;
    pool->free_head = pool->next[index];
    pool->used[index] = TRUE;
    LeaveCriticalSection(&pool->mutex);
    return (void *)(pool->memory + index * BLOCK_SIZE_CHUNK);
}

void pool_free_chunk(MemPoolFileChunk* pool, void* ptr) {
    if(ptr == NULL){
        return;
    }
    EnterCriticalSection(&pool->mutex);
    int index = ((uint8_t*)ptr - pool->memory) / BLOCK_SIZE_CHUNK;
    if (index < 0 || index >= BLOCK_COUNT_CHUNK || !pool->used[index]){
        // Log critical errors with maximum detail
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid or already freed chunk!\n");
        fprintf(stderr, "  Pointer to free: %p\n", ptr);
        fprintf(stderr, "  Calculated index: %d\n", index);
        fprintf(stderr, "  Pool base address: %p\n", pool->memory);
        fprintf(stderr, "  Block size: %d bytes\n", BLOCK_SIZE_CHUNK);
        fprintf(stderr, "  Total block count: %d\n", BLOCK_COUNT_CHUNK);
        fprintf(stderr, "  Is within bounds (0 to %d)? %s\n", BLOCK_COUNT_CHUNK - 1,
                (index >= 0 && index < BLOCK_COUNT_CHUNK) ? "Yes" : "No");
        fprintf(stderr, "  Was block marked as used? %s\n", (index >= 0 && index < BLOCK_COUNT_CHUNK && pool->used[index]) ? "Yes" : "No (Double-Free/Corruption)\n");
        LeaveCriticalSection(&pool->mutex);
        return;
    }
    pool->next[index] = pool->free_head;
    pool->free_head = index;
    pool->used[index] = FALSE;
    LeaveCriticalSection(&pool->mutex);
    return;
}

void pool_destroy_chunk(MemPoolFileChunk* pool) {
    free(pool->memory);
    pool->memory = NULL;
    DeleteCriticalSection(&pool->mutex);
}

#endif