#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <stdbool.h>            // For BOOL type
#include <windows.h>            // Required for CRITICAL_SECTION and related functions
#include "include/protocol_frames.h"             // For UdpFrame structure

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

#define SEQ_NUM_QUEUE_SIZE                      131072     // Queue buffer size
#define QUEUE_ACK_SIZE                          65536     // Queue buffer size
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

typedef struct{
    uint64_t seq;       // The sequence number of the frame that require ack/nak
    uint8_t op_code;
    uint32_t sid;    // Session ID of the frame (used to identify the connected client)
    SOCKET src_socket;
    struct sockaddr_in dest_addr; // Address of the sender
}QueueAckEntry;
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

typedef struct {
    QueueAckEntry entry[QUEUE_ACK_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
}QueueAck;


int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);

int push_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry);
int pop_seq_num(QueueSeqNum *queue, QueueSeqNumEntry *seq_num_entry);

void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, 
                        const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int push_ack(QueueAck *queue, QueueAckEntry *entry);
int pop_ack(QueueAck *queue, QueueAckEntry *entry);


#endif