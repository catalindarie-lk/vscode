
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type

#include "include/protocol_frames.h"             // For UdpFrame structure
#include "include/queue.h"

// ---------------------- QUEUE FOR BUFFERING RECEIVED FRAMES ----------------------
// Push frame data to queue - received frames are buffered to a queue before processing; receive thread pushes the frame to the queue
int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
         fprintf(stderr, "Push - Frame queue not initialized.\n");
        return RET_VAL_ERROR;
    }
    // Check if the queue is full
    EnterCriticalSection(&queue->mutex);
    if((queue->tail + 1) % FRAME_QUEUE_SIZE == queue->head){
        LeaveCriticalSection(&queue->mutex);
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
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}
// Pop frame data from queue - frames are poped from the queue by the frame processing thread
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry){       
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL) {
        fprintf(stderr, "Pop - Frame queue not initialized.\n");
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing
    
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        return RET_VAL_ERROR;
    }
    memset(frame_entry, 0, sizeof(QueueFrameEntry)); // Initialize the structure to zero
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(frame_entry, &queue->frame_entry[queue->head], sizeof(QueueFrameEntry)); // Copy the frame from the queue
    memset(&queue->frame_entry[queue->head], 0, sizeof(QueueFrameEntry)); // Clear the frame at the head
    // Move the head index forward
    (queue->head)++;
    queue->head %= FRAME_QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr){
    //memset(entry, 0, sizeof(QueueAckEntry));
    entry->seq = seq;
    entry->sid = sid;
    entry->op_code = op_code;
    entry->src_socket = src_socket;
    memcpy(&entry->dest_addr, dest_addr, sizeof(struct sockaddr_in));
    return;
}

int push_ack(QueueAck *queue, QueueAckEntry *entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL){
        fprintf(stderr, "Push - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is full
    if((queue->tail + 1) % QUEUE_ACK_SIZE == queue->head){
        LeaveCriticalSection(&queue->mutex);
        //fprintf(stderr, "Push - Ack queue Full\n");
        return RET_VAL_ERROR;
    }
    // Add the entry to the ACK queue 
    memcpy(&queue->entry[queue->tail], entry, sizeof(QueueAckEntry));
    // Move the tail index forward    
    (queue->tail)++;
    queue->tail %= QUEUE_ACK_SIZE;
    ReleaseSemaphore(queue->semaphore, 1, NULL);
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

int pop_ack(QueueAck *queue, QueueAckEntry *entry){       
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL){
        //fprintf(stderr, "Pop - Ack queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing an entry
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
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
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}