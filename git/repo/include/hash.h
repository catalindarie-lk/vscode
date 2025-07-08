#ifndef FRAMES_HASH_H
#define FRAMES_HASH_H

#include <stdint.h>
#include <windows.h>

#include "include/protocol_frames.h"
#include "include/mem_pool.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

//--------------------------------------------------------------------------------------------------------------------------
#define HASH_SIZE_FRAME                (32768)

typedef struct FramePendingAck{
    UdpFrame frame;
    time_t time;
    uint16_t counter;
    struct FramePendingAck *next;
}FramePendingAck;

uint64_t get_hash_frame(uint64_t key);
int insert_frame(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, UdpFrame *frame, uint32_t *count, MemPool *pool);
void remove_frame(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, uint64_t seq_num, uint32_t *count, MemPool *pool);
void clean_frame_hash_table(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, uint32_t *count, MemPool *pool);

//--------------------------------------------------------------------------------------------------------------------------
#define HASH_SIZE_UID                  (1024)

typedef uint8_t HashMessageStatus;
enum HashMessageStatus{
    UID_WAITING_FRAGMENTS = 1,
    UID_RECV_COMPLETE= 2
};

typedef struct UniqueIdentifierNode{
    uint32_t u_id;
    uint32_t s_id;
    uint8_t status;                         //1 - Pending; 2 - Finished
    struct UniqueIdentifierNode *next;
}UniqueIdentifierNode;

uint64_t get_hash_uid(uint32_t u_id);
void add_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status);
void remove_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id);
BOOL search_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status);
int update_uid_status_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status);
void clean_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex);
void print_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex);

#endif // FRAMES_HASH_H