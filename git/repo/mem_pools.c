#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define BLOCK_SIZE 100
#define BLOCK_COUNT 1024

typedef struct {
    uint8_t* memory;             // Raw memory buffer
    int free_head;               // Index of the first free block
    int next[BLOCK_COUNT];       // Next free block indices
    bool used[BLOCK_COUNT];      // Usage flags (optional, for safety/debugging)
} MemPool;

// Initialize the pool
void pool_init(MemPool* pool) {
    pool->memory = malloc(BLOCK_SIZE * BLOCK_COUNT);
    pool->free_head = 0;

    for (int i = 0; i < BLOCK_COUNT - 1; i++) {
        pool->next[i] = i + 1;
        pool->used[i] = false;
    }
    pool->next[BLOCK_COUNT - 1] = -1; // End of free list
    pool->used[BLOCK_COUNT - 1] = false;
}

// Allocate a block
void* pool_alloc(MemPool* pool) {
    if (pool->free_head == -1) return NULL; // Pool exhausted

    int index = pool->free_head;
    pool->free_head = pool->next[index];
    pool->used[index] = true;

    return pool->memory + index * BLOCK_SIZE;
}

// Free a block
void pool_free(MemPool* pool, void* ptr) {
    int index = ((uint8_t*)ptr - pool->memory) / BLOCK_SIZE;
    if (index < 0 || index >= BLOCK_COUNT || !pool->used[index]) return;

    pool->next[index] = pool->free_head;
    pool->free_head = index;
    pool->used[index] = false;
}

// Destroy the pool
void pool_destroy(MemPool* pool) {
    free(pool->memory);
    pool->memory = NULL;
}

void main(){
    
}