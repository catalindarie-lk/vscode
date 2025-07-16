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

typedef struct{
    FramePendingAck *entry[HASH_SIZE_FRAME];
    CRITICAL_SECTION mutex;
    uint32_t count;
    MemPool pool;
}HashTableFramePendingAck;

uint64_t ht_get_hash_frame(const uint64_t seq_num);
int ht_insert_frame(HashTableFramePendingAck *ht, UdpFrame *frame);
void ht_remove_frame(HashTableFramePendingAck *ht, const uint64_t seq_num);
void ht_clean(HashTableFramePendingAck *ht);

//--------------------------------------------------------------------------------------------------------------------------
#define HASH_SIZE_ID                  (512)

typedef uint8_t HashTableStatus;
enum HashTableStatus{
    ID_STATUS_NONE = 0,
    ID_WAITING_FRAGMENTS = 1,
    ID_RECV_COMPLETE= 2
};

typedef struct IdentifierNode{
    uint32_t id;
    uint32_t sid;
    uint8_t status;                         //1 - Pending; 2 - Finished
    struct IdentifierNode *next;
}IdentifierNode;

typedef struct{
    IdentifierNode *entry[HASH_SIZE_ID];
    CRITICAL_SECTION mutex;
    uint32_t count;
    MemPool pool;
}HashTableIdentifierNode;

uint64_t ht_get_hash_id(uint32_t id);
int ht_insert_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_remove_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id);
BOOL ht_search_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
int ht_update_id_status(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_clean_id(HashTableIdentifierNode *ht);
void ht_print_id(HashTableIdentifierNode *ht);

#endif // FRAMES_HASH_H