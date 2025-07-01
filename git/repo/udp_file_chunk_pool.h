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
}
void* pool_alloc_chunk(MemPoolFileChunk* pool) {
    if (pool->free_head == -1) return NULL; // Pool exhausted

    int index = pool->free_head;
    pool->free_head = pool->next[index];
    pool->used[index] = TRUE;

    return (void *)(pool->memory + index * BLOCK_SIZE_CHUNK);
}
void pool_free_chunk(MemPoolFileChunk* pool, void* ptr) {
    int index = ((uint8_t*)ptr - pool->memory) / BLOCK_SIZE_CHUNK;
    if (index < 0 || index >= BLOCK_COUNT_CHUNK || !pool->used[index]) return;

    pool->next[index] = pool->free_head;
    pool->free_head = index;
    pool->used[index] = FALSE;
}
void pool_destroy_chunk(MemPoolFileChunk* pool) {
    free(pool->memory);
    pool->memory = NULL;
}


#endif