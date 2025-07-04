#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <stdbool.h>            // For BOOL type
#include <windows.h>            // Required for CRITICAL_SECTION and related functions
#include "frames.h"             // For UdpFrame structure

#define SEQ_NUM_QUEUE_SIZE                      131072     // Queue buffer size
#define FRAME_QUEUE_SIZE                        65536

#pragma pack(push, 1)
typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t frame_size; // Number of bytes received for this frame
}QueueFrameEntry;

typedef struct{
    uint64_t seq_num;       // The sequence number of the frame that require ack/nak
    uint8_t op_code;
    uint32_t session_id;    // Session ID of the frame (used to identify the connected client)
    struct sockaddr_in addr; // Address of the sender
}QueueSeqNumEntry;
#pragma pack(pop)

typedef struct {
    QueueFrameEntry frame_entry[FRAME_QUEUE_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
}QueueFrame;

typedef struct {
    QueueSeqNumEntry seq_num_entry[SEQ_NUM_QUEUE_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
}QueueSeqNum;


int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);

int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);

int push_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry);

int pop_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry);

#endif