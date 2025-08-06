
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

// Circular queue for file streams
int init_queue_fstream(QueueFstream *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid pointer for queue fstream init\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for fstream queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;
    queue->pfstream = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if(!queue->pfstream){
        fprintf(stderr, "CRITICAL ERROR: allocating memory - fstream queue init\n");
    }
    memset(queue->pfstream, 0, sizeof(uintptr_t) * size);
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
       if (!queue->push_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - s_queue frame init\n");
        _aligned_free(queue->pfstream);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
int push_fstream(QueueFstream *queue, const uintptr_t pfstream){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid pointer - fstream queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size - fstream queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: Push -> queue fstream FULL\n");
        return RET_VAL_ERROR;
    }
    // Move the tail index forward    
    queue->pfstream[queue->tail] = pfstream;
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);

    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}
uintptr_t pop_fstream(QueueFstream *queue){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid pointer - fstream queue not initialized\n");
        return 0;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size - fstream queue not initialized\n");
        return 0;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: Pop -> queue fstream EMPTY\n");
        return 0;
    }
    uintptr_t pfstream = queue->pfstream[queue->head];
    queue->pfstream[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return pfstream;
}

// Circular queue for commands (it has only push/pop semaphores)
int s_init_queue_command(QueueCommand *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid command queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for command queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (QueueCommandEntry *)_aligned_malloc(sizeof(QueueCommandEntry) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - command queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(QueueCommandEntry) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, size - 1, LONG_MAX, NULL);
    if (!queue->push_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - s_queue frame init\n");
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!queue->pop_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating pop_semaphore - s_queue frame init\n");
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
int s_push_command(QueueCommand *queue, QueueCommandEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Push - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        fprintf(stdout, "WARNING: - command queue FULL can't push!\n");
    }
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueCommandEntry));
    // Move the tail index forward
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
int s_pop_command(QueueCommand *queue, QueueCommandEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Pop - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    WaitForSingleObject(queue->pop_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        fprintf(stderr, "WARNING: - command queue FULL can't pop!\n");
    }
    // Copy the entry to buffer
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueCommandEntry));
    // Remove the entry
    memset(&queue->entry[queue->head], 0, sizeof(QueueCommandEntry));
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);

    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

// Circular queue for frame pointers (it has only push semaphore)
int init_queue_frame(QueueFrame *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for frame queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - frame queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - frame queue init\n");
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_frame(QueueFrame *queue,  const uintptr_t entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - queue frame not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Push - queue frame size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push queue frame FULL!\n");
        return RET_VAL_ERROR;
    }
    queue->entry[queue->tail] = entry;
    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uintptr_t pop_frame(QueueFrame *queue){       
    // Check if the queue is initialized
    uintptr_t entry = 0;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - queue frame not initialized.\n");
        return entry;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Pop - queue frame size not initialized\n");
        return entry;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: - Pop queue frame EMPTY\n");
        return entry;
    }
    entry = queue->entry[queue->head];
    queue->entry[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);

    LeaveCriticalSection(&queue->lock);
    return entry;
}

// Circular queue for frame pointers (it has push and pop semaphores)
int s_init_queue_frame(s_QueueFrame *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid s_queue frame pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for s_queue frame init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes)
    queue->entry = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - s_queue frame init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, size - 1, LONG_MAX, NULL);
    if (!queue->push_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - s_queue frame init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!queue->pop_semaphore) {
        fprintf(stderr, "CRITICAL ERROR: creating pop_semaphore - s_queue frame init\n");
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int s_push_frame(s_QueueFrame *queue,  const uintptr_t entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - s_queue frame not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Push - s_queue frame invalid queue size\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - s_queue frame FULL (push fail)\n");
        return RET_VAL_ERROR;
    }
    queue->entry[queue->tail] = entry;
    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);

    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
uintptr_t s_pop_frame(s_QueueFrame *queue){       
    // Check if the queue is initialized
    uintptr_t pool_entry = 0;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - s_queue frame not initialized.\n");
        return pool_entry;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Pop - s_queue frame invalid queue size\n");
        return pool_entry;
    }

    WaitForSingleObject(queue->pop_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: - s_queue frame queue empty (pop fail)\n");
        return pool_entry;
    }
    pool_entry = queue->entry[queue->head];
    queue->entry[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);

    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return pool_entry;
}

// Circular queue for pointer entries
int init_queue_ptr(QueuePtr *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid ptr queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for ptr queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->ptr = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->ptr) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - ptr queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->ptr, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - ptr queue init\n");
        _aligned_free(queue->ptr);
    return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_ptr(QueuePtr *queue,  const uintptr_t ptr){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0) {
        fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push ptr queue FULL!\n");
        return RET_VAL_ERROR;
    }
    queue->ptr[queue->tail] = ptr;
    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uintptr_t pop_ptr(QueuePtr *queue){       
    // Check if the queue is initialized
    uintptr_t ptr = 0;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Ptr queue not initialized.\n");
        return ptr;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Pop - Ptr queue size not initialized\n");
        return ptr;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: - Pop ptr queue EMPTY\n");
        return ptr;
    }
    ptr = queue->ptr[queue->head];
    queue->ptr[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);

    LeaveCriticalSection(&queue->lock);
    return ptr;
}

// Circular queue for client slot numbers
int init_queue_slot(QueueClientSlot *queue, size_t size){
    if (!queue){
        fprintf(stderr, "CRITICAL ERROR: Invalid slot queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Invalid size for slot queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->slot = (uint32_t *)_aligned_malloc(sizeof(uint32_t) * size, 64);
    if (!queue->slot) {
        fprintf(stderr, "CRITICAL ERROR: allocating aligned memory - slot queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->slot, 0, sizeof(uint32_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "CRITICAL ERROR: creating push_semaphore - frame queue init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->slot);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_slot(QueueClientSlot *queue,  const uint32_t slot){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "CRITICAL ERROR: Push - Ptr queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push slot queue FULL!\n");
        return RET_VAL_ERROR;
    }
    queue->slot[queue->tail] = slot;
    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uint32_t pop_slot(QueueClientSlot *queue){       
    // Check if the queue is initialized
    uint32_t slot = UINT32_MAX;
    if (!queue) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Slot queue not initialized.\n");
        return slot;
    }
    if(queue->size <= 0) {
        fprintf(stderr, "CRITICAL ERROR: Pop - Slot queue size not initialized\n");
        return slot;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        // fprintf(stderr, "ERROR: - Pop slot queue EMPTY\n");
        return slot;
    }
    slot = queue->slot[queue->head];
    queue->slot[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);

    LeaveCriticalSection(&queue->lock);
    return slot;
}

// Circular queue for frame sequence numbers
int init_queue_seq(QueueSeq *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid seq queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for seq queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->seq = (uint64_t *)_aligned_malloc(sizeof(uint64_t) * size, 64);
    if (!queue->seq) {
        fprintf(stderr, "Error allocating aligned memory - seq queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->seq, 0, sizeof(uint64_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "Error creating push_semaphore - seq queue init\n");
        _aligned_free(queue->seq);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_seq(QueueSeq *queue,  const uint64_t seq){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - Seq queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Push - Seq queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push seq queue FULL!\n");
        return RET_VAL_ERROR;
    }
    queue->seq[queue->tail] = seq;
    // Move the tail index forward    
    queue->tail = next_tail;
    InterlockedIncrement64(&queue->pending);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uint64_t pop_seq(QueueSeq *queue){       
    // Check if the queue is initialized
    uint64_t seq = 0;
    if (!queue) {
        fprintf(stderr, "Pop - Seq queue not initialized.\n");
        return seq;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - Seq queue size not initialized\n");
        return seq;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "ERROR: - Pop seq queue EMPTY\n");
        return seq;
    }
    seq = queue->seq[queue->head];
    queue->seq[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    InterlockedDecrement64(&queue->pending);

    LeaveCriticalSection(&queue->lock);
    return seq;
}

