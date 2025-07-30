
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

void init_queue_frame(QueueFrame *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for frame queue init\n");
        return;
    }
    queue->size = size;

    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (QueueFrameEntry *)_aligned_malloc(sizeof(QueueFrameEntry) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "Error allocating aligned memory - frame queue init\n");
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(QueueFrameEntry) * size);

    // In destroy_queue_command(), use _aligned_free(queue->entry) to avoid memory leaks.

    if(!queue->entry){
        fprintf(stderr, "Error allocating memory - frame queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);    
    return;
}
int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){
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
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - Push frame queue FULL!\n");
        return RET_VAL_ERROR;
    }
    memcpy(&queue->entry[queue->tail], frame_entry, sizeof(QueueFrameEntry));
    // Move the tail index forward    
    queue->tail = next_tail;
    queue->pending++;
    
    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){       
    // Check if the queue is initialized
    if (!queue) {
        fprintf(stderr, "Pop - Frame queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - Frame queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "ERROR: - Pop fream queue EMPTY\n");
        return RET_VAL_ERROR;
    }
    memcpy(frame_entry, &queue->entry[queue->head], sizeof(QueueFrameEntry));
    memset(&queue->entry[queue->head], 0, sizeof(QueueFrameEntry));
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;

    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    if(!entry){
        fprintf(stderr, "Passed invalid ack entry pointer\n");
        return;
    }
    entry->seq = seq;
    entry->sid = sid;
    entry->op_code = op_code;
    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    return;
}
void init_queue_ack(QueueAck *queue, const size_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for ack queue init\n");
        return;
    }
    queue->size = size;
    
    // Allocate memory aligned to cache line (64 bytes typical)
    queue->entry = (QueueAckEntry *)_aligned_malloc(sizeof(QueueAckEntry) * size, 64);
    if (!queue->entry) {
        fprintf(stderr, "Error allocating aligned memory - ack queue init\n");
        return; // early return in case of failure
    }
    // Initialize memory to zero
    memset(queue->entry, 0, sizeof(QueueAckEntry) * size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);
    return;
}
int push_ack(QueueAck *queue, QueueAckEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Push - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Push - Ack queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    long next_tail = (queue->tail + 1) % queue->size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "WARNING - Push Ack queue Full!\n");
        return RET_VAL_ERROR;
    }
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueAckEntry));
    // Move the tail index forward    
    queue->tail = next_tail;
    queue->pending++;
    
    LeaveCriticalSection(&queue->lock);
    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;
}
int pop_ack(QueueAck *queue, QueueAckEntry *entry){       
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Pop - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Pop - Ack queue size not initialized\n");
        return RET_VAL_ERROR;
    }
    //WaitForSingleObject(queue->semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "ERROR: - Pop fream queue EMPTY\n");
        return RET_VAL_ERROR;
    }
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueAckEntry));
    memset(&queue->entry[queue->head], 0, sizeof(QueueAckEntry));
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;

    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

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



int init_queue_ack_frame(QueueAckUpdFrame *queue, const size_t size){
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
int push_ack_frame(QueueAckUpdFrame *queue,  const uintptr_t entry){
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
    long next_tail = (queue->tail + 1) % queue->size;
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
uintptr_t pop_ack_frame(QueueAckUpdFrame *queue){       
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
    queue->entry[queue->head] = (uintptr_t)0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->size;
    queue->pending--;

    LeaveCriticalSection(&queue->lock);
    return entry;
}



int init_queue_tx_frame(QueueTXFrame *queue, const size_t queue_size){
    if (!queue){
        fprintf(stderr, "Invalid queue tx_frame pointer\n");
        return RET_VAL_ERROR;
    }
    if(queue_size <= 0){
        fprintf(stderr, "Invalid size for queue tx_frame init\n");
        return RET_VAL_ERROR;
    }
    queue->queue_size = queue_size;

    // Allocate memory aligned to cache line (64 bytes)
    queue->pool_entry = (uintptr_t *)_aligned_malloc(sizeof(uintptr_t) * queue_size, 64);
    if (!queue->pool_entry) {
        fprintf(stderr, "Error allocating aligned memory - queue tx_frame init\n");
        return RET_VAL_ERROR;
    }
    // Initialize memory to zero
    memset(queue->pool_entry, 0, sizeof(uintptr_t) * queue_size);

    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->push_semaphore = CreateSemaphore(NULL, queue_size - 1, LONG_MAX, NULL);
    queue->pop_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!queue->push_semaphore || !queue->pop_semaphore) {
        fprintf(stderr, "Error creating push_semaphore - queue tx_frame init\n");
        // Clean up already allocated memory and critical section
        _aligned_free(queue->pool_entry);
    return RET_VAL_ERROR;
    }
    InitializeCriticalSection(&queue->lock);    
    return RET_VAL_SUCCESS;
}
int push_tx_frame(QueueTXFrame *queue,  const uintptr_t pool_entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - queue tx_frame not initialized.\n");
        return RET_VAL_ERROR;
    }
    if(queue->queue_size <= 0){
        fprintf(stderr, "Push - queue tx_frame invalid queue size\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    WaitForSingleObject(queue->push_semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    size_t next_tail = (queue->tail + 1) % queue->queue_size;
    if(next_tail == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "WARNING: - tx_frame frame queue FULL (push fail)!\n");
        return RET_VAL_ERROR;
    }
    queue->pool_entry[queue->tail] = pool_entry;
    // Move the tail index forward    
    queue->tail = next_tail;
    queue->pending++;

    ReleaseSemaphore(queue->pop_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
uintptr_t pop_tx_frame(QueueTXFrame *queue){       
    // Check if the queue is initialized
    uintptr_t pool_entry = 0;
    if (!queue) {
        fprintf(stderr, "Pop - queue tx_frame not initialized.\n");
        return pool_entry;
    }
    if(queue->queue_size <= 0){
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
    pool_entry = queue->pool_entry[queue->head];
    queue->pool_entry[queue->head] = 0;
    // Move the head index forward
    queue->head = (queue->head + 1) % queue->queue_size;
    queue->pending--;

    ReleaseSemaphore(queue->push_semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
    return pool_entry;
}