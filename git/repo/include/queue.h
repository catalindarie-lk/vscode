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
#define MAX_ENTRY_SIZE (MAX_PATH + MAX_PATH + 32)
#pragma pack(push, 1)
typedef struct{
    char text[32];
    char fpath[MAX_PATH];
    uint32_t fpath_len;
    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];
    uint32_t fname_len;
}QueueCommandEntrySendFile;

typedef struct{
    char text[32];
    char *message_buffer;
    uint32_t message_len;
}QueueCommandEntrySendMessage;

__declspec(align(64))typedef struct{
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
    volatile size_t head;
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;
    HANDLE push_semaphore;
    HANDLE pop_semaphore;
}QueueCommand;

int s_init_queue_command(QueueCommand *queue, const size_t size);
int s_push_command(QueueCommand *queue, QueueCommandEntry *entry);
int s_pop_command(QueueCommand *queue, QueueCommandEntry *entry);
 
//----------------------------------------------------------------------------------------------------------------

__declspec(align(64)) typedef struct{
    size_t size;
    uintptr_t *pfstream;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;
    HANDLE push_semaphore;
}QueueFstream;

int init_queue_fstream(QueueFstream *queue, const size_t size);
int push_fstream(QueueFstream *queue, const uintptr_t pfstream);
uintptr_t pop_fstream(QueueFstream *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size; 
    uintptr_t *entry;
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue    
}QueueFrame;

int init_queue_frame(QueueFrame *queue, const size_t size);
int push_frame(QueueFrame *queue,  const uintptr_t entry);
uintptr_t pop_frame(QueueFrame *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uintptr_t *entry;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    HANDLE pop_semaphore;       
}s_QueueFrame;

int s_init_queue_frame(s_QueueFrame *queue, const size_t size);
int s_push_frame(s_QueueFrame *queue,  const uintptr_t entry);
uintptr_t s_pop_frame(s_QueueFrame *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uintptr_t *ptr;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
}QueuePtr;

int init_queue_ptr(QueuePtr *queue, const size_t size);
int push_ptr(QueuePtr *queue,  const uintptr_t ptr);
uintptr_t pop_ptr(QueuePtr *queue);

//----------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct {
    size_t size;
    uint32_t *slot;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
}QueueClientSlot;

//----------------------------------------------------------------------------------------------------------------

int init_queue_slot(QueueClientSlot *queue, size_t slot);
int push_slot(QueueClientSlot *queue,  const uint32_t slot);
uint32_t pop_slot(QueueClientSlot *queue);

__declspec(align(64))typedef struct {
    size_t size;
    uint64_t *seq;      // pointer to an array of uintptr_t
    volatile size_t head;          
    volatile size_t tail;
    volatile size_t pending;
    CRITICAL_SECTION lock;      // Mutex for thread-safe access to frame_buffer
    HANDLE push_semaphore;      // this semaphore is released when frame is pushed on the queue
}QueueSeq;


int init_queue_seq(QueueSeq *queue, const size_t size);
int push_seq(QueueSeq *queue,  const uint64_t seq);
uint64_t pop_seq(QueueSeq *queue);

#endif 