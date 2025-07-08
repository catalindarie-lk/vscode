
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset

#include "include/mem_pool.h"

//--------------------------------------------------------------------------------------------------------------------------
void pool_init(MemPool* pool) {
    // Allocate memory for 'next' array
    pool->next = (uint64_t*)calloc(pool->block_count, sizeof(uint64_t));
    if (pool->next == NULL) {
        fprintf(stderr, "Memory allocation failed for next indices in pool_init.\n");
        return;
    }
    //memset(pool->next, 0, pool->block_count * sizeof(uint64_t)); // Initialize to 0

    // Allocate memory for 'used' array
    pool->used = (uint8_t*)calloc(pool->block_count, sizeof(uint8_t));
    if (pool->used == NULL) {
        fprintf(stderr, "Memory allocation failed for used flags in pool_init.\n");
        free(pool->next); // Clean up previously allocated memory
        return;
    }
    //memset(pool->used, 0, pool->block_count * sizeof(uint8_t)); // Initialize to 0 (unused)
    // Allocate the main memory buffer for the pool
    pool->memory = (char*)calloc(pool->block_count, pool->block_size);
    if (pool->memory == NULL) {
        fprintf(stderr, "Memory allocation failed for pool memory in pool_init.\n");
        free(pool->next); // Clean up previously allocated memory
        free(pool->used);
        return;
    }
    //memset(pool->memory, 0, pool->block_count * pool->block_size); // Initialize memory to 0
    // Initialize the free list: all blocks are initially free
    pool->free_head = 0; // The first block is the head of the free list
    // Link all blocks together and mark them as unused
    for (uint64_t i = 0; i < pool->block_count - 1; i++) {
        pool->next[i] = i + 1; // Link to the next block
        pool->used[i] = 0;     // Mark as unused
    }
    // The last block points to -1, indicating the end of the free list
    pool->next[pool->block_count - 1] = (uint64_t)-1; // Use -1 to indicate end of list
    pool->used[pool->block_count - 1] = 0;           // Last block is also unused

    // Initialize the critical section for thread safety
    InitializeCriticalSection(&pool->mutex);
    return;
}
//--------------------------------------------------------------------------------------------------------------------------
void* pool_alloc(MemPool* pool) {
    // Enter critical section to protect shared pool data
    EnterCriticalSection(&pool->mutex);
    // Check if the pool is exhausted
    if (pool->free_head == (uint64_t)-1) { // Check against -1
        LeaveCriticalSection(&pool->mutex);
        return NULL; // Pool exhausted
    }
    // Get the index of the first free block
    uint64_t index = pool->free_head;
    // Update the free head to the next free block
    pool->free_head = pool->next[index];
    // Mark the allocated block as used
    pool->used[index] = 1;
    // Leave critical section
    LeaveCriticalSection(&pool->mutex);
    // Return the memory address of the allocated block
    memset(pool->memory + index * pool->block_size, 0, pool->block_size); // Optional: clear memory for safety
    return (void *)(pool->memory + index * pool->block_size);
}
//--------------------------------------------------------------------------------------------------------------------------
void pool_free(MemPool* pool, void* ptr) {
    // Handle NULL pointer case
    if (ptr == NULL) {
        fprintf(stderr, "ERROR: Attempt to free a NULL block in pool!\n");
        return;
    }
    // Enter critical section
    EnterCriticalSection(&pool->mutex);
    // Calculate the index of the block to be freed
    uint64_t index = ((char*)ptr - pool->memory) / pool->block_size;
    // Validate the index and usage flag for safety and debugging
    if (index >= pool->block_count || !pool->used[index]) { // Corrected bounds check
        // Log critical errors with maximum detail
        fprintf(stderr, "CRITICAL ERROR: Attempt to free invalid or already freed chunk!\n");
        fprintf(stderr, "   Pointer to free: %p\n", ptr);
        fprintf(stderr, "   Calculated index: %llu\n", index);
        fprintf(stderr, "   Pool base address: %p\n", pool->memory);
        fprintf(stderr, "   Block size: %llu bytes\n", pool->block_size);
        fprintf(stderr, "   Total block count: %llu\n", pool->block_count);
        fprintf(stderr, "   Is within bounds (0 to %llu)? %s\n", pool->block_count - 1,
                        (index < pool->block_count) ? "Yes" : "No");
        fprintf(stderr, "   Was block marked as used? %s\n", (index < pool->block_count && pool->used[index]) ? "Yes" : "No (Double-Free/Corruption)\n");
        LeaveCriticalSection(&pool->mutex);
        return;
    }
    // Add the freed block back to the head of the free list
    pool->next[index] = pool->free_head;
    pool->free_head = index;
    // Mark the block as unused
    pool->used[index] = 0;
    // memset(pool->memory + index * pool->block_size, 0, pool->block_size);
    LeaveCriticalSection(&pool->mutex);
    return;
}
//--------------------------------------------------------------------------------------------------------------------------
void pool_destroy(MemPool* pool) {
    // Check for NULL pool pointer
    if (pool == NULL) {
        fprintf(stderr, "ERROR: Attempt to destroy a unallocated pool!\n");
        return;
    }

    // Free allocated memory for 'next' array
    if (pool->next != NULL) {
        free(pool->next);
        pool->next = NULL;
    }
    // Free allocated memory for 'used' array
    if (pool->used != NULL) {
        free(pool->used);
        pool->used = NULL;
    }
    // Free the main memory buffer
    if (pool->memory != NULL) {
        free(pool->memory);
        pool->memory = NULL;
    }
    DeleteCriticalSection(&pool->mutex);
}
//--------------------------------------------------------------------------------------------------------------------------