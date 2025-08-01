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
__declspec(align(64)) typedef struct{
    UdpFrame frame; // The UDP frame to be sent
    struct sockaddr_in src_addr; // Destination address for the frame
    uint32_t frame_size; // Number of bytes received for this frame
}QueueFrameEntry;

typedef struct {
    size_t size; 
    QueueFrameEntry *entry;
    volatile uint32_t head;          
    volatile uint32_t tail;
    volatile uint32_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    HANDLE pop_semaphore;       // not used
}QueueFrame;

void init_queue_frame(QueueFrame *queue, const size_t size);
int push_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);
int pop_frame(QueueFrame *queue, QueueFrameEntry *frame_entry);

//----------------------------------------------------------------------------------------------------------------
__declspec(align(64)) typedef struct{
    uint64_t seq;       // The sequence number of the frame that require ack/nak
    uint8_t op_code;
    uint32_t sid;    // Session ID of the frame (used to identify the connected client)
    SOCKET src_socket;
    struct sockaddr_in dest_addr; // Address of the sender
}QueueAckEntry;

typedef struct {
    size_t size;
    QueueAckEntry *entry;
    volatile uint32_t head;          
    volatile uint32_t tail;
    volatile uint32_t pending;
    CRITICAL_SECTION lock; // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;
}QueueAck;


void new_ack_entry(QueueAckEntry *entry, const uint64_t seq, const uint32_t sid, 
                        const uint8_t op_code, const SOCKET src_socket, const struct sockaddr_in *dest_addr);
void init_queue_ack(QueueAck *queue, const size_t size);
int push_ack(QueueAck *queue, QueueAckEntry *entry);
int pop_ack(QueueAck *queue, QueueAckEntry *entry);

//----------------------------------------------------------------------------------------------------------------
#define MAX_ENTRY_SIZE (MAX_PATH + MAX_PATH + 32)
#pragma pack(push, 1)
__declspec(align(64)) typedef struct{
    char text[32];
    char fpath[MAX_PATH];
    uint32_t fpath_len;
    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];
    uint32_t fname_len;
}QueueCommandEntrySendFile;

__declspec(align(64)) typedef struct{
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
    size_t size;
    QueueCommandEntry *entry;
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t pending;
    CRITICAL_SECTION lock;
    HANDLE push_semaphore;
    HANDLE pop_semaphore;
//    HANDLE cleared;
}QueueCommand;

void init_queue_command(QueueCommand *queue, const size_t size);
int push_command(QueueCommand *queue, QueueCommandEntry *entry);
int pop_command(QueueCommand *queue, QueueCommandEntry *entry);
 
//----------------------------------------------------------------------------------------------------------------

__declspec(align(64)) typedef struct{
    size_t size;
    uintptr_t *pfstream;
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t pending;
    CRITICAL_SECTION lock;
    HANDLE push_semaphore;
    HANDLE pop_semaphore;           // not used
}QueueFstream;

void init_queue_fstream(QueueFstream *queue, const size_t size);
int push_fstream(QueueFstream *queue, const uintptr_t pfstream);
uintptr_t pop_fstream(QueueFstream *queue);




//----------------------------------------------------------------------------------------------------------------
// __declspec(align(64)) typedef struct{
//     AckUdpFrame frame; // The UDP frame to be sent
//     struct sockaddr_in addr; // Destination address for the frame
// }QueueEntryAckUdpFrame;

__declspec(align(64))typedef struct {
    size_t size; 
    uintptr_t *entry;
    volatile uint32_t head;          
    volatile uint32_t tail;
    volatile uint32_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    HANDLE pop_semaphore;       // not used
}QueueAckUpdFrame;

int init_queue_ack_frame(QueueAckUpdFrame *queue, const size_t size);
int push_ack_frame(QueueAckUpdFrame *queue,  const uintptr_t entry);
uintptr_t pop_ack_frame(QueueAckUpdFrame *queue);


//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uintptr_t *entry;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    HANDLE pop_semaphore;       // not used
}QueueSendFrame;

int init_queue_send_frame(QueueSendFrame *queue, const size_t size);
int push_send_frame(QueueSendFrame *queue,  const uintptr_t entry);
uintptr_t pop_send_frame(QueueSendFrame *queue);


#endif 