
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"


void init_queue_fstream(QueueFstream *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid pointer for queue fstream init\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for fstream queue init\n");
        return;
    }
    queue->size = size;
    queue->pfstream = calloc(queue->size, sizeof(uintptr_t));
    if(!queue->pfstream){
        fprintf(stderr, "Error allocating memory - fstream queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);
    return;
}
int push_fstream(QueueFstream *queue, const uintptr_t pfstream){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Invalid pointer - fstream queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - fstream queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: Push -> fream queue FULL\n");
        return RET_VAL_ERROR;
    }
    // Move the tail index forward    
    queue->pfstream[queue->tail] = pfstream;
    queue->tail = next_tail;
    queue->pending++;

    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}
uintptr_t pop_fstream(QueueFstream *queue){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Invalid pointer - fstream queue not initialized\n");
        return 0;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - fstream queue not initialized\n");
        return 0;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "ERROR: Pop -> fream queue EMPTY\n");
        return 0;
    }
    uintptr_t pfstream = queue->pfstream[queue->head];
    queue->pfstream[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;
    LeaveCriticalSection(&queue->lock);
    return pfstream;
}

void init_queue_command(QueueCommand *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid command queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for command queue init\n");
        return;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (QueueCommandEntry *)_aligned_malloc(sizeof(QueueCommandEntry) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "Error allocating aligned memory - command queue init\n");
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(QueueCommandEntry) * size);

    // In destroy_queue_command(), use _aligned_free(queue->entry) to avoid memory leaks.

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, size - 1, LONG_MAX, NULL);
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);
    return;
}
int push_command(QueueCommand *queue, QueueCommandEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Push - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        fprintf(stderr, "ERROR SHOULD NOT HAPPEN - Command queue is full -> head=%u tail=%u pending=%u\n", queue->head, queue->tail, queue->pending);
    }
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueCommandEntry));
    // Move the tail index forward
    queue->tail = next_tail;
    queue->pending++;
    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}
int pop_command(QueueCommand *queue, QueueCommandEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Pop - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    WaitForSingleObject(queue->pop_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        fprintf(stderr, "ERROR SHOULD NOT HAPPEN - Command queue is empty -> head=%u tail=%u pending=%u\n", queue->head, queue->tail, queue->pending);
    }
    // Copy the entry to buffer
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueCommandEntry));
    // Remove the entry
    memset(&queue->entry[queue->head], 0, sizeof(QueueCommandEntry));
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;
    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}



int init_queue_frame(QueueAckUpdFrame *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for frame queue init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "Error allocating aligned memory - frame queue init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(uintptr_t) * size);

    // In destroy_queue_command(), use _aligned_free(queue->entry) to avoid memory leaks.

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (queue->push_semaphore == NULL) {
        fprintf(stderr, "Error creating push_semaphore - frame queue init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->entry);
    return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_frame(QueueAckUpdFrame *queue,  const uintptr_t entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - Frame queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Push - Frame queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push frame queue FULL!\n");
        return RET_VAL_ERROR;
    }
    queue->entry[queue->tail] = entry;
    // Move the tail index forward    
    queue->tail = next_tail;
    queue->pending++;
    
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
uintptr_t pop_frame(QueueAckUpdFrame *queue){       
    // Check if the queue is initialized
    uintptr_t entry = 0;
    if (!queue) {
        fprintf(stderr, "Pop - Frame queue not initialized.\n");
        return entry;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - Frame queue size not initialized\n");
        return entry;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "ERROR: - Pop fream queue EMPTY\n");
        return entry;
    }
    entry = queue->entry[queue->head];
    queue->entry[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;

    LeaveCriticalSection(&queue->lock);
    return entry;
}



int init_queue_send_frame(QueueSendFrame *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue tx_frame pointer\n");
        return RET_VAL_ERROR;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for queue tx_frame init\n");
        return RET_VAL_ERROR;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes)
    queue->entry = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "Error allocating aligned memory - queue tx_frame init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(uintptr_t) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, size - 1, LONG_MAX, NULL);
    if (!queue->push_semaphore) {
        fprintf(stderr, "Error creating push_semaphore - queue tx_frame init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!queue->pop_semaphore) {
        fprintf(stderr, "Error creating pop_semaphore - queue tx_frame init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->entry);
        return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_send_frame(QueueSendFrame *queue,  const uintptr_t entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - queue tx_frame not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Push - queue tx_frame invalid queue size\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - tx_frame frame queue FULL (push fail)!\n");
        return RET_VAL_ERROR;
    }
    queue->entry[queue->tail] = entry;
    // Move the tail index forward    
    queue->tail = next_tail;
    queue->pending++;

    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
uintptr_t pop_send_frame(QueueSendFrame *queue){       
    // Check if the queue is initialized
    uintptr_t pool_entry = 0;
    if (!queue) {
        fprintf(stderr, "Pop - queue tx_frame not initialized.\n");
        return pool_entry;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - queue tx_frame invalid queue size\n");
        return pool_entry;
    }

    WaitForSingleObject(queue->pop_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING: - tx_frame frame queue empty (pop fail)!\n");
        return pool_entry;
    }
    pool_entry = queue->entry[queue->head];
    queue->entry[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;

    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return pool_entry;
}