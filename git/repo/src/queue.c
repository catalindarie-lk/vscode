
#include <stdio.h>              // For fprintf, NULL checks, etc.
#include <stdlib.h>             // For malloc, free, etc.
#include <stdint.h>             // For uint64_t and uint8_t types
#include <string.h>             // For memset
#include <stdbool.h>            // For BOOL type
#include <windows.h>            // Required for CRITICAL_SECTION and related functions
#include "frames.h"             // For UdpFrame structure
#include "queue.h"

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
    InterlockedIncrement(&queue->tail);
    queue->tail %= FRAME_QUEUE_SIZE;
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
    InterlockedIncrement(&queue->head);
    queue->head %= FRAME_QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}

// ---------------------- QUEUE FOR ACK/NAK FRAMES ----------------------
// Push sequence num data to queue - frames that need ack/nak are buffered in a circular queue
int push_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry){
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL){
        fprintf(stderr, "Push - Seq Num queue not initialized\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is full
    if((queue->tail + 1) % SEQ_NUM_QUEUE_SIZE == queue->head){
        LeaveCriticalSection(&queue->mutex);
        //fprintf(stderr, "Push - Seq Num queue Full\n");
        return RET_VAL_ERROR;
    }
    // Acquire the mutex to ensure thread-safe access to the queue
    // Add the sequence number to the ACK queue 
    memcpy(&queue->seq_num_entry[queue->tail], seq_num_entry, sizeof(QueueSeqNumEntry));
    // Move the tail index forward    
    InterlockedIncrement(&queue->tail);
    queue->tail %= SEQ_NUM_QUEUE_SIZE;
    // Release the mutex after modifying the queue
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}
// Pop sequence num data from queue -> send ack/nak (separate thread)
int pop_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry){       
    // Check if the queue is initialized
    if (queue == NULL || &queue->mutex == NULL){
        //fprintf(stderr, "Pop - Seq Num queue not initialized\n");
        return RET_VAL_ERROR; // Return an empty RecvFrameInfo
    }
    EnterCriticalSection(&queue->mutex);
    // Check if the queue is empty before removing a ACK
    if (queue->head == queue->tail) {
        LeaveCriticalSection(&queue->mutex);
        //fprintf(stderr, "Pop - Seq Num queue empty\n");
        return RET_VAL_ERROR;
    }
    memset(seq_num_entry, 0, sizeof(QueueSeqNumEntry));
    // Acquire the mutex to ensure thread-safe access to the queue
    memcpy(seq_num_entry, &queue->seq_num_entry[queue->head], sizeof(QueueSeqNumEntry));
    memset(&queue->seq_num_entry[queue->head], 0, sizeof(QueueSeqNumEntry));
    // Move the head index forward
    InterlockedIncrement(&queue->head);
    queue->head %= SEQ_NUM_QUEUE_SIZE;
    LeaveCriticalSection(&queue->mutex);
    return RET_VAL_SUCCESS;
}
