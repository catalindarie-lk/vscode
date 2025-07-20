#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>             // For uint64_t and uint8_t types
#include <stdbool.h>            // For BOOL type
#include <windows.h>            // Required for CRITICAL_SECTION and related functions
#include "include/protocol_frames.h"             // For UdpFrame structure
//#include "include/server.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif


#define FRAME_QUEUE_SIZE                        32768
#define QUEUE_ACK_SIZE                          131072     // Queue buffer size
#define QUEUE_FSTREAM_SIZE                      100


//----------------------------------------------------------------------------------------------------------------
typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t frame_size; // Number of bytes received for this frame
}QueueFrameEntry;

typedef struct {
    QueueFrameEntry frame_entry[FRAME_QUEUE_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
    HANDLE semaphore;
}QueueFrame;



int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);
//----------------------------------------------------------------------------------------------------------------
typedef struct{
    uint64_t seq;       // The sequence number of the frame that require ack/nak
    uint8_t op_code;
    uint32_t sid;    // Session ID of the frame (used to identify the connected client)
    SOCKET src_socket;
    struct sockaddr_in dest_addr; // Address of the sender
}QueueAckEntry;

typedef struct {
    QueueAckEntry entry[QUEUE_ACK_SIZE];
    uint32_t head;          
    uint32_t tail;
    CRITICAL_SECTION mutex; // Mutex for thread-safe access to frame_buffer
    HANDLE semaphore;
}QueueAck;


void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, 
                        const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
int push_ack(QueueAck *queue, QueueAckEntry *entry);
int pop_ack(QueueAck *queue, QueueAckEntry *entry);
//----------------------------------------------------------------------------------------------------------------
typedef struct{
    intptr_t fstream_ptr;
}QueueFstreamEntry;

typedef struct{
    QueueFstreamEntry entry[QUEUE_FSTREAM_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t pending;
    CRITICAL_SECTION lock;
    HANDLE semaphore;
}QueueFstream;

int push_fstream(QueueFstream *queue, QueueFstreamEntry *entry);
int pop_fstream(QueueFstream *queue, QueueFstreamEntry *entry);
 
#endif