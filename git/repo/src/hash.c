//  // This file contains the implementation of the hash table for frames and unique identifiers.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "include/protocol_frames.h"
#include "include/netendians.h"
#include "include/mem_pool.h"
#include "include/hash.h"

//--------------------------------------------------------------------------------------------------------------------------
void init_ht_frame(HashTableFramePendingAck *ht, const uint32_t size){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table init\n");
        return;
    }
    ht->size = size;
    ht->entry = calloc(ht->size, sizeof(intptr_t));
    if(!ht->entry){
        fprintf(stderr, "ERROR: Unable to allocate memory for frame hash table\n");
        return;
    }
    InitializeCriticalSection(&ht->mutex);
    ht->count = 0;
    return;
}
uint64_t ht_get_hash_frame(const uint64_t seq_num, const uint32_t size){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table get_seq_num()\n");
        return RET_VAL_ERROR;
    }
    return (seq_num % size);
}
int ht_insert_frame(HashTableFramePendingAck *ht, UdpFrame *frame){
    if(!ht->entry){
        fprintf(stderr, "ERROR: Invalid hash table pointer for insert_frame()\n");
        return RET_VAL_ERROR;
    }
    if(ht->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table insert_frame()\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&ht->mutex);
    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint64_t index = ht_get_hash_frame(seq_num, ht->size);
    //fprintf(stdout, "SeqNum: %llu inserted at index: %llu\n", seq_num, index);
    FramePendingAck *node = (FramePendingAck *)pool_alloc(&ht->pool);
    if(node == NULL){
        return RET_VAL_ERROR;
    }
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->sent_count = 1;

    node->next = (FramePendingAck *)ht->entry[index];  // Insert at the head (linked list)
    ht->entry[index] = node;
    InterlockedIncrement(&ht->count);
    LeaveCriticalSection(&ht->mutex);
    return RET_VAL_SUCCESS;
}
void ht_remove_frame(HashTableFramePendingAck *ht, const uint64_t seq_num){
    if(!ht->entry){
        fprintf(stderr, "ERROR: Invalid hash table pointer for remove_frame()\n");
        return;
    }
    if(ht->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table remove_frame()\n");
        return;
    }
    EnterCriticalSection(&ht->mutex);
    uint64_t index = ht_get_hash_frame(seq_num, ht->size);
    //fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
    FramePendingAck *curr = ht->entry[index];
    FramePendingAck *prev = NULL;
    while (curr) {
        if (_ntohll(curr->frame.header.seq_num) == seq_num) {
            //fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                ht->entry[index] = curr->next;
            }
            pool_free(&ht->pool, curr);
            //free(curr);
            InterlockedDecrement(&ht->count);
            //fprintf(stdout, "Hash count: %d\n", *count);
            LeaveCriticalSection(&ht->mutex);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(&ht->mutex);
    return;
 
}
void ht_clean(HashTableFramePendingAck *ht){
    if(!ht->entry){
        fprintf(stderr, "ERROR: Invalid hash table pointer for remove_frame()\n");
        return;
    }
    if(ht->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table remove_frame()\n");
        return;
    }
    EnterCriticalSection(&ht->mutex);
    FramePendingAck *head = NULL;
    for (int i = 0; i < ht->size; i++) {
        if(ht->entry[i]){       
            FramePendingAck *ptr = ht->entry[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    pool_free(&ht->pool, head);
                    //free(head);
                    InterlockedDecrement(&ht->count);
            }
            if(ptr){
                pool_free(&ht->pool, ptr);
                ptr = NULL;
            }
            //free(ptr);
            ht->entry[i] = NULL;
        }     
    }
//    fprintf(stdout, "Frame hash table clean\n");
    LeaveCriticalSection(&ht->mutex);
    return;
}

//--------------------------------------------------------------------------------------------------------------------------
void htbl_init_txframe(hTbl_txFrame *htable, const size_t size, const size_t max_nodes){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for tx_frame hash table init\n");
        return;
    }
    if(max_nodes <= size){
        fprintf(stderr, "ERROR: Invalid max_nodes for tx_frame hash table init\n");
        return;
    }
    htable->size = size;
    htable->head = (hTblNode_txFrame **)_aligned_malloc(sizeof(hTblNode_txFrame) * size, 64);
    if(!htable->head){
        fprintf(stderr, "ERROR: Unable to allocate memory for tx_frame hash table\n");
        return;
    }
    pool_init(&htable->pool_nodes, sizeof(hTblNode_txFrame), max_nodes);
    memset(htable->head, 0, sizeof(uintptr_t) * size);

    InitializeCriticalSection(&htable->mutex);
    htable->count = 0;
    return;
}
uint64_t htbl_get_hash_txframe(const uint64_t seq_num, const size_t size){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for tx_frame hash table get_seq_num()\n");
        return RET_VAL_ERROR;
    }
    return (seq_num % size);
}
int htbl_insert_txframe(hTbl_txFrame *htable, const uintptr_t pool_frame){
    if(!htable->head){
        fprintf(stderr, "ERROR: Invalid node array pointer for hash table - insert_tx_frame()\n");
        return RET_VAL_ERROR;
    }
    if(!pool_frame){
        fprintf(stderr, "ERROR: Invalid pool_frame pointer for insert_tx_frame() into hash table\n");
        return RET_VAL_ERROR;
    }
    if(htable->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table insert_tx_frame()\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&htable->mutex);
    UdpFrame *frame = (UdpFrame*)pool_frame;    
    
    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint64_t index = htbl_get_hash_txframe(seq_num, htable->size);
    
    //fprintf(stdout, "SeqNum: %llu inserted at index: %llu\n", seq_num, index);
    // Node_HTableTXFrame *node = (Node_HTableTXFrame *)pool_alloc(&htable->pool);
    hTblNode_txFrame *node = (hTblNode_txFrame *)pool_alloc(&htable->pool_nodes);
    if(node == NULL){
        fprintf(stderr, "Failed to allocate memeory for tx_frame hash table node");
        return RET_VAL_ERROR;
    }
    // memcpy(&node->pool_entry, pool_entry, sizeof(uintptr_t));
    node->frame = pool_frame;
    node->sent_time = time(NULL);
    node->count = 1;

    node->next = (hTblNode_txFrame*)htable->head[index];  // Insert at the head (linked list)
    htable->head[index] = node;
    htable->count++;
    LeaveCriticalSection(&htable->mutex);
    return RET_VAL_SUCCESS;
}
uintptr_t htbl_remove_txframe(hTbl_txFrame *htable, const uint64_t seq_num){
    if(!htable->head){
        fprintf(stderr, "ERROR: Invalid node array pointer for hash table - insert_tx_frame()\n");
        return 0;
    }
    if(htable->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table remove_tx_frame()\n");
        return 0;
    }
    EnterCriticalSection(&htable->mutex);
    uint64_t index = htbl_get_hash_txframe(seq_num, htable->size);

    hTblNode_txFrame *curr = htable->head[index];
    hTblNode_txFrame *prev = NULL;
    while (curr) {
        
        UdpFrame *pool_frame = (UdpFrame*)curr->frame;
        uint64_t frame_seq_num = _ntohll(pool_frame->header.seq_num);

        if (frame_seq_num == seq_num) {
            // fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                htable->head[index] = curr->next;
            }
            // free(curr);
            pool_free(&htable->pool_nodes, (void*)curr);
            htable->count--;
            //fprintf(stdout, "Hash count: %d\n", *count);
            LeaveCriticalSection(&htable->mutex);
            return (uintptr_t)pool_frame;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(&htable->mutex);
    return 0;
 
}

//--------------------------------------------------------------------------------------------------------------------------
void init_ht_id(HashTableIdentifierNode *ht){
    for(int i = 0; i < HASH_SIZE_ID; i++){
        ht->entry[i] = NULL;
    }
    InitializeCriticalSection(&ht->mutex); 
    ht->count = 0;
}
uint64_t ht_get_hash_id(uint32_t id) {
    return (id % HASH_SIZE_ID);
}
int ht_insert_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status) {
    EnterCriticalSection(&ht->mutex);
    uint64_t index = ht_get_hash_id(id);   
    IdentifierNode *node = (IdentifierNode *)malloc(sizeof(IdentifierNode));
        if(node == NULL){
        fprintf(stderr, "ERROR: fail to allocate memory for id hash node\n");
        return RET_VAL_ERROR;
    }
    node->sid = sid;
    node->id = id;    
    node->status = status;
    fprintf(stdout, "ADDED to hash table SID: %d - ID: %d\n", node->sid, node->id);
    node->next = (IdentifierNode *)ht->entry[index];  // Insert at the head (linked list)
    ht->entry[index] = node;
    InterlockedIncrement(&ht->count);
    LeaveCriticalSection(&ht->mutex);
    return RET_VAL_SUCCESS;
}
void ht_remove_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id) {
    
    EnterCriticalSection(&ht->mutex);

    uint64_t index = ht_get_hash_id(id); 
    IdentifierNode *curr = ht->entry[index];
    IdentifierNode *prev = NULL;
    while (curr) {     
        if (curr->id == id && curr->sid == sid) {
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                ht->entry[index] = curr->next;
            }
            fprintf(stdout, "REMOVED from hash table SID: %d - ID: %d\n", curr->sid, curr->id);
            free(curr);
            LeaveCriticalSection(&ht->mutex);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(&ht->mutex);
    return;
}
void ht_remove_all_sid(HashTableIdentifierNode *ht, const uint32_t sid) {
    EnterCriticalSection(&ht->mutex);

    for (size_t i = 0; i < HASH_SIZE_ID; ++i) {
        IdentifierNode *curr = ht->entry[i];
        IdentifierNode *prev = NULL;

        while (curr) {
            if (curr->sid == sid) {
                IdentifierNode *to_remove = curr;

                if (prev) {
                    prev->next = curr->next;
                } else {
                    ht->entry[i] = curr->next;
                }
                curr = curr->next;
                fprintf(stdout, "REMOVED from hash table SID: %d - ID: %d\n", to_remove->sid, to_remove->id);
                free(to_remove);
                ht->count--;
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
    }

    LeaveCriticalSection(&ht->mutex);
}
BOOL ht_search_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status) {
    
    EnterCriticalSection(&ht->mutex);
    
    uint64_t index = ht_get_hash_id(id);
    IdentifierNode *node = ht->entry[index];
    while (node) {
        if (node->sid == sid && node->id == id && node->status == status){
            LeaveCriticalSection(&ht->mutex);
            return TRUE;
        }           
        node = node->next;
    }
    LeaveCriticalSection(&ht->mutex);
    return FALSE;
}
int ht_update_id_status(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status) {
    
    EnterCriticalSection(&ht->mutex);
    
    uint64_t index = ht_get_hash_id(id);
    IdentifierNode *node = ht->entry[index];
    while (node) {
        if (node->id == id && node->sid == sid){
            node->status = status;
            LeaveCriticalSection(&ht->mutex);
            return RET_VAL_SUCCESS;
        }           
        node = node->next;
    }
    LeaveCriticalSection(&ht->mutex);
    return RET_VAL_ERROR;

}
void ht_clean_id(HashTableIdentifierNode *ht) {
    
    EnterCriticalSection(&ht->mutex);
    
    IdentifierNode *head = NULL;
    for (int index = 0; index < HASH_SIZE_ID; index++) {
        if(ht->entry[index]){       
            IdentifierNode *node = ht->entry[index];
            while (node) {
                    head = node;                
                    node = node->next;
                    free(head);
            }
            free(node);
            ht->entry[index] = NULL;
        }     
    }
    LeaveCriticalSection(&ht->mutex);
    return;
}
void ht_print_id(HashTableIdentifierNode *ht) {
    
    EnterCriticalSection(&ht->mutex);
    
    for (int index = 0; index < HASH_SIZE_ID; index++) {
        if(ht->entry[index]){
            printf("BUCKET %d: \n", index);           
            IdentifierNode *node = ht->entry[index];
            while (node) {
                    fprintf(stdout, "sID: %d - fID: %d\n", node->sid, node->id);                   
                    node = node->next;
            }
        }     
    }
    LeaveCriticalSection(&ht->mutex);
}

