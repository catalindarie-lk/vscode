
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

void init_queue_frame(QueueFrame *queue){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->frame_entry, 0, FRAME_QUEUE_SIZE * sizeof(QueueFrameEntry));
    InitializeCriticalSection(&queue->lock);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    return;
}
int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){
    // Check if the queue is initialized
    if (!queue) {
         fprintf(stderr, "Push - Frame queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->lock);
    if((queue->tail + 1) % FRAME_QUEUE_SIZE == queue->head){
        LeaveCriticalSection(&queue->lock);
        //fprintf(stdout, "Frame queue full\n");
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    
    // Add the sequence number to the ACK queue 
    memcpy(&queue->frame_entry[queue->tail], frame_entry, sizeof(QueueFrameEntry)); // Copy the frame to the queue
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= FRAME_QUEUE_SIZE;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){       
    // Check if the queue is initialized
    if (!queue) {
        fprintf(stderr, "Pop - Frame queue not initialized.\n");
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        return RET_VAL_ERROR;
    }
    memset(frame_entry, 0, sizeof(QueueFrameEntry)); // Initialize the structure to zero
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(frame_entry, &queue->frame_entry[queue->head], sizeof(QueueFrameEntry)); // Copy the frame from the queue
    memset(&queue->frame_entry[queue->head], 0, sizeof(QueueFrameEntry)); // Clear the frame at the head
    // Move the head index forward
    (queue->head)++;
    queue->head %= FRAME_QUEUE_SIZE;
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
void init_queue_ack(QueueAck *queue){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->entry, 0, QUEUE_ACK_SIZE * sizeof(QueueAckEntry));
    InitializeCriticalSection(&queue->lock);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    return;
}
int push_ack(QueueAck *queue, QueueAckEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Push - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_ACK_SIZE == queue->head){
        LeaveCriticalSection(&queue->lock);
        //fprintf(stderr, "Push - Ack queue Full\n");
        return RET_VAL_ERROR;
    }
    // Add the entry to the ACK queue 
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueAckEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= QUEUE_ACK_SIZE;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
int pop_ack(QueueAck *queue, QueueAckEntry *entry){       
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Pop - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    //WaitForSingleObject(queue->semaphore, INFINITE);
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        //fprintf(stderr, "Pop - Ack queue empty\n");
        return RET_VAL_ERROR;
    }
    // Copy the entry to buffer
    memcpy(entry, &queue->entry[queue->head], sizeof(QueueAckEntry));
    // Remove the entry in the ACK queue
    memset(&queue->entry[queue->head], 0, sizeof(QueueAckEntry));
    // Move the head index forward
    (queue->head)++;
    queue->head %= QUEUE_ACK_SIZE;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

void init_queue_command(QueueCommand *queue){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->entry, 0, QUEUE_COMMAND_SIZE * sizeof(QueueCommandEntry));
    InitializeCriticalSection(&queue->lock);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    return;
}
int push_command(QueueCommand *queue, QueueCommandEntry *entry){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Push - command queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_COMMAND_SIZE == queue->head){
        LeaveCriticalSection(&queue->lock);
        return RET_VAL_ERROR;
    }
    // Add the entry to the fstream queue 
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueCommandEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= QUEUE_COMMAND_SIZE;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);
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
    queue->head %= QUEUE_COMMAND_SIZE;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}

void init_queue_fstream(QueueFstream *queue){
    if (!queue){
        fprintf(stderr, "Invalid queue pointer\n");
        return;
    }
    queue->head = 0;
    queue->tail = 0;
    queue->pending = 0;
    memset(&queue->pfstream, 0, QUEUE_FSTREAM_SIZE * sizeof(intptr_t));
    InitializeCriticalSection(&queue->lock);
    queue->semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    return;
}
int push_fstream(QueueFstream *queue, const intptr_t pfstream){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Push - fstream queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_FSTREAM_SIZE == queue->head){
        LeaveCriticalSection(&queue->lock);
        //fprintf(stderr, "Push - fream queue Full\n");
        return RET_VAL_ERROR;
    }
    // Add the entry to the fstream queue 
    //memcpy(&queue->entry[queue->tail], entry, sizeof(QueueFstreamEntry));
    // Move the tail index forward    
    queue->pfstream[queue->tail] = pfstream;
    (queue->tail)++;
    queue->tail %= QUEUE_FSTREAM_SIZE;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    InterlockedIncrement(&queue->pending);
    // Release the lock after modifying the queue
    LeaveCriticalSection(&queue->lock);
    return RET_VAL_SUCCESS;
}
intptr_t pop_fstream(QueueFstream *queue){
    // Check if the queue is initialized
    if (!queue){
        fprintf(stderr, "Pop - fstream queue not initialized\n");
        return 0;
    }
    EnterCriticalSection(&queue->lock);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->lock);
        //fprintf(stderr, "Pop - fstream queue empty\n");
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
    queue->head %= QUEUE_FSTREAM_SIZE;
    InterlockedDecrement(&queue->pending);
    LeaveCriticalSection(&queue->lock);
    return pfstream;
}
