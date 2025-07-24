
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

void init_queue_frame(QueueFrame *queue, const uint32_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for frame queue init\n");
        return;
    }
    queue->size = size;
    queue->entry = calloc(queue->size, sizeof(QueueFrameEntry));
        if(!queue->entry){
        fprintf(stderr, "Error allocating memory - frame queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
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
    if((queue->tail + 1) % queue->size == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stdout, "Frame queue full!\n");
        return RET_VAL_ERROR;
    }
    
    memcpy(&queue->entry[queue->tail], frame_entry, sizeof(QueueFrameEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= queue->size;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);

    LeaveCriticalSection(&queue->lock);
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
        return RET_VAL_ERROR;
    }
    memset(frame_entry, 0, sizeof(QueueFrameEntry));
    memcpy(frame_entry, &queue->entry[queue->head], sizeof(QueueFrameEntry));
    memset(&queue->entry[queue->head], 0, sizeof(QueueFrameEntry));
    // Move the head index forward
    (queue->head)++;
    queue->head %= queue->size;
    InterlockedDecrement(&queue->pending);
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
void init_queue_ack(QueueAck *queue, const uint32_t size){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for ack queue init\n");
        return;
    }
    queue->size = size;
    queue->entry = calloc(queue->size, sizeof(QueueAckEntry));
        if(!queue->entry){
        fprintf(stderr, "Error allocating memory - ack queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
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
    if((queue->tail + 1) % queue->size == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "Push - Ack queue Full!\n");
        return RET_VAL_ERROR;
    }
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueAckEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= queue->size;
    InterlockedIncrement(&queue->pending);
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    LeaveCriticalSection(&queue->lock);
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
        fprintf(stderr, "Pop - Ack queue empty!\n");
        return RET_VAL_ERROR;
    }
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueAckEntry));
    memset(&queue->entry[queue->head], 0, sizeof(QueueAckEntry));
    (queue->head)++;
    queue->head %= queue->size;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

void init_queue_fstream(QueueFstream *queue, const uint32_t size){
    if (!queue){
        fprintf(stderr, "Invalid pointer for queue fstream init\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for fstream queue init\n");
        return;
    }
    queue->size = size;
    queue->pfstream = calloc(queue->size, sizeof(intptr_t));
    if(!queue->pfstream){
        fprintf(stderr, "Error allocating memory - fstream queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);
    return;
}
void clean_queue_fstream(QueueFstream *queue){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Invalid pointer - fstream queue not initialized\n");
        return;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - fstream queue not initialized\n");
        return;
    }
    EnterCriticalSection(&queue->lock);
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->pfstream, 0, queue->size * sizeof(intptr_t));
    CloseHandle(queue->semaphore);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    LeaveCriticalSection(&queue->lock);
    return;
}
int push_fstream(QueueFstream *queue, const intptr_t pfstream){
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
    if((queue->tail + 1) % queue->size == queue->head){
        LeaveCriticalSection(&queue->lock);
        fprintf(stderr, "Push - fream queue Full\n");
        return RET_VAL_ERROR;
    }
    // Add the entry to the fstream queue 
    //memcpy(&queue->entry[queue->tail], entry, sizeof(QueueFstreamEntry));
    // Move the tail index forward    
    queue->pfstream[queue->tail] = pfstream;
    (queue->tail)++;
    queue->tail %= queue->size;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);
    // Release the lock after modifying the queue
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
intptr_t pop_fstream(QueueFstream *queue){
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
        return 0;
    }
    // Copy the entry to buffer
    //memcpy(entry, &queue->entry[queue->head], sizeof(QueueFstreamEntry));
    intptr_t pfstream = queue->pfstream[queue->head];
    // Remove the entry in the ACK queue
    //memset(&queue->entry[queue->head], 0, sizeof(QueueFstreamEntry));
    queue->pfstream[queue->head] = 0;
    // Move the head index forward
    (queue->head)++;
    queue->head %= queue->size;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return pfstream;
}

void init_queue_command(QueueCommand *queue, const uint32_t size){
    if (!queue){
        fprintf(stderr, "Invalid command queue pointer\n");
        return;
    }
    if(size <= 0){
        fprintf(stderr, "Invalid size for command queue init\n");
        return;
    }
    queue->size = size;
    queue->entry = calloc(queue->size, sizeof(QueueCommandEntry));
        if(!queue->entry){
        fprintf(stderr, "Error allocating memory - command queue init\n");
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    InitializeCriticalSection(&queue->lock);
    return;
}
void clean_queue_command(QueueCommand *queue){
    if (!queue){
        fprintf(stderr, "Invalid pinter - command queue not initialized\n");
        return;
    }
    if(queue->size <= 0){
        fprintf(stderr, "Invalid size - command queue not initialized\n");
        return;
    }
    if(!queue->semaphore){
        fprintf(stderr, "Invalid command queue semaphore\n");
        return;
    }
    EnterCriticalSection(&queue->lock);
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->entry, 0, queue->size * sizeof(QueueCommandEntry));
    CloseHandle(queue->semaphore);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    LeaveCriticalSection(&queue->lock);
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
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    if((queue->tail + 1) % queue->size == queue->head){
        fprintf(stderr, "Command queue is full!\n");
        LeaveCriticalSection(&queue->lock);
        return RET_VAL_ERROR;
    }
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueCommandEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= queue->size;
    InterlockedIncrement(&queue->pending);
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    // Release the lock after modifying the queue
    LeaveCriticalSection(&queue->lock);
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
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        return RET_VAL_ERROR;
    }
    // Copy the entry to buffer
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueCommandEntry));
    // Remove the entry
    memset(&queue->entry[queue->head], 0, sizeof(QueueCommandEntry));
    // Move the head index forward
    (queue->head)++;
    queue->head %= queue->size;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

