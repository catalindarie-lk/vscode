#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include "include/protocol_frames.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

//----------------------------------------------------------------------------------------------------------------
typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t frame_size; // Number of bytes received for this frame
}QueueFrameEntry;

typedef struct {
    uint32_t size; 
    QueueFrameEntry *entry;
    uint32_t head;          
    uint32_t tail;
    uint32_t pending;
    CRITICAL_SECTION lock; // Mutex for thread-safe access to frame_buffer
    HANDLE semaphore;
}QueueFrame;

void init_queue_frame(QueueFrame *queue, const uint32_t size);
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
    uint32_t size;
    QueueAckEntry *entry;
    uint32_t head;          
    uint32_t tail;
    uint32_t pending;
    CRITICAL_SECTION lock; // Mutex for thread-safe access to frame_buffer
    HANDLE semaphore;
}QueueAck;


void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, 
                        const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
void init_queue_ack(QueueAck *queue, const uint32_t size);
int push_ack(QueueAck *queue, QueueAckEntry *entry);
int pop_ack(QueueAck *queue, QueueAckEntry *entry);

//----------------------------------------------------------------------------------------------------------------
#define MAX_ENTRY_SIZE (MAX_PATH + MAX_PATH + 32)
#pragma pack(push, 1)
typedef struct{
    char text[32];
    char fpath[MAX_PATH];
    char fname[MAX_PATH];
}QueueCommandEntrySendFile;

typedef struct{
    char text[32];
    char *message_buffer;
    uint32_t message_len;
}QueueCommandEntrySendMessage;

typedef struct{
    union{
        QueueCommandEntrySendFile send_file;
        QueueCommandEntrySendMessage send_message;
        uint8_t max_bytes[MAX_ENTRY_SIZE];
    } command;
}QueueCommandEntry;
#pragma pack(pop)

typedef struct{
    uint32_t size;
    QueueCommandEntry *entry;
    uint32_t head;
    uint32_t tail;
    uint32_t pending;
    CRITICAL_SECTION lock;
    HANDLE semaphore;
    HANDLE cleared;
}QueueCommand;

void init_queue_command(QueueCommand *queue, const uint32_t size);
void clean_queue_command(QueueCommand *queue);
int push_command(QueueCommand *queue, QueueCommandEntry *entry);
int pop_command(QueueCommand *queue, QueueCommandEntry *entry);
 
//----------------------------------------------------------------------------------------------------------------

typedef struct{
    uint32_t size;
    intptr_t *pfstream;
    uint32_t head;
    uint32_t tail;
    uint32_t pending;
    CRITICAL_SECTION lock;
    HANDLE semaphore;
}QueueFstream;

void init_queue_fstream(QueueFstream *queue, const uint32_t size);
void clean_queue_fstream(QueueFstream *queue);
int push_fstream(QueueFstream *queue, const intptr_t pfstream);
intptr_t pop_fstream(QueueFstream *queue);


#endif