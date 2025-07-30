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
typedef struct FramePendingAck{
    UdpFrame frame;
    time_t time;
    uint16_t sent_count;
    struct FramePendingAck *next;
}FramePendingAck;

typedef struct{
    uint32_t size;
    FramePendingAck **entry;
    CRITICAL_SECTION mutex;
    uint32_t count;
    MemPool pool;
}HashTableFramePendingAck;

void init_ht_frame(HashTableFramePendingAck *ht, const uint32_t size);
uint64_t ht_get_hash_frame(const uint64_t seq_num, const uint32_t size);
int ht_insert_frame(HashTableFramePendingAck *ht, UdpFrame *frame);
void ht_remove_frame(HashTableFramePendingAck *ht, const uint64_t seq_num);
void ht_clean(HashTableFramePendingAck *ht);

//--------------------------------------------------------------------------------------------------------------------------
#define HASH_SIZE_ID                  (65536)

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
}HashTableIdentifierNode;

void init_ht_id(HashTableIdentifierNode *ht);
uint64_t ht_get_hash_id(uint32_t id);
int ht_insert_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_remove_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id);
void ht_remove_all_sid(HashTableIdentifierNode *ht, const uint32_t sid);
BOOL ht_search_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
int ht_update_id_status(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status);
void ht_clean_id(HashTableIdentifierNode *ht);
void ht_print_id(HashTableIdentifierNode *ht);







//--------------------------------------------------------------------------------------------------------------------------

__declspec(align(64))typedef struct hTblNode_txFrame{
    uintptr_t frame;
    time_t sent_time;
    uint16_t count;
    struct hTblNode_txFrame *next;
}hTblNode_txFrame;

__declspec(align(64))typedef struct{
    size_t size;
    hTblNode_txFrame **head;  // array of pointers to hTblNode_txFrame
    CRITICAL_SECTION mutex;
    size_t count;
    MemPool pool_nodes;
}hTbl_txFrame;

void htbl_init_txframe(hTbl_txFrame *htable, const size_t size, const size_t max_nodes);
uint64_t htbl_get_hash_txframe(const uint64_t seq_num, const size_t size);
int htbl_insert_txframe(hTbl_txFrame *htable, const uintptr_t pool_frame);
uintptr_t htbl_remove_txframe(hTbl_txFrame *htable, const uint64_t seq_num);

// void ht_txframe_clean(HTableTXFrame *htable);




 
#endif // FRAMES_HASH_H