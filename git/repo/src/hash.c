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
void init_table_send_frame(TableSendFrame *table, const size_t size, const size_t max_nodes){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for tx_frame hash table init\n");
        return;
    }
    if(max_nodes <= size){
        fprintf(stderr, "ERROR: Invalid max_nodes for tx_frame hash table init\n");
        return;
    }
    table->size = size;
    table->node = (TableNodeSendFrame **)_aligned_malloc(sizeof(TableNodeSendFrame) * size, 64);
    if(!table->node){
        fprintf(stderr, "ERROR: Unable to allocate memory for tx_frame hash table\n");
        return;
    }
    init_pool(&table->pool_nodes, sizeof(TableNodeSendFrame), max_nodes);
    // memset(table->node, 0, sizeof(uintptr_t) * size);
    for(int i = 0; i < size; i++){
        table->node[i] = NULL;
    }

    InitializeCriticalSection(&table->mutex);
    table->count = 0;
    return;
}
uint64_t get_hash_table_send_frame(const uint64_t seq_num, const size_t size){
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for tx_frame hash table get_seq_num()\n");
        return RET_VAL_ERROR;
    }
    return (seq_num % size);
}
int insert_table_send_frame(TableSendFrame *table, const uintptr_t entry){
    if(!table->node){
        fprintf(stderr, "ERROR: Invalid node array pointer for hash table - insert_tx_frame()\n");
        return RET_VAL_ERROR;
    }
    if(!entry){
        fprintf(stderr, "ERROR: Invalid pool_frame pointer for insert_tx_frame() into hash table\n");
        return RET_VAL_ERROR;
    }
    if(table->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table insert_tx_frame()\n");
        return RET_VAL_ERROR;
    }
    EnterCriticalSection(&table->mutex);
    PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)entry;
    
    uint64_t seq_num = _ntohll(pool_entry->frame.header.seq_num);
    uint64_t index = get_hash_table_send_frame(seq_num, table->size);
    
    TableNodeSendFrame *node = (TableNodeSendFrame*)pool_alloc(&table->pool_nodes);
    if(node == NULL){
        fprintf(stderr, "ERROR: Failed to allocate memeory for tx_frame hash table node\n");
        return RET_VAL_ERROR;
    }
    // fprintf(stdout, "DEBUG: Inserting seq num: %llu at index: %llu\n", seq_num, index);
    node->entry = entry;
    node->sent_time = time(NULL);
    node->sent_count = 1;

    node->next = (TableNodeSendFrame*)table->node[index];  // Insert at the head (linked list)
    table->node[index] = node;
    table->count++;
    LeaveCriticalSection(&table->mutex);
    return RET_VAL_SUCCESS;
}
uintptr_t remove_table_send_frame(TableSendFrame *table, const uint64_t seq_num){
    if(!table->node){
        fprintf(stderr, "ERROR: Invalid node array pointer for hash table - htbl_remove_txframe()\n");
        return 0;
    }
    if(table->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table htbl_remove_txframe()\n");
        return 0;
    }
    EnterCriticalSection(&table->mutex);
    uint64_t index = get_hash_table_send_frame(seq_num, table->size);

    TableNodeSendFrame *curr = table->node[index];
    TableNodeSendFrame *prev = NULL;
    while (curr) {
        
        PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)curr->entry;
        uint64_t frame_seq_num = _ntohll(pool_entry->frame.header.seq_num);

        if (frame_seq_num == seq_num) {
            // fprintf(stdout, "DEBUG: Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                table->node[index] = curr->next;
            }
            // free(curr);
            pool_free(&table->pool_nodes, (void*)curr);
            table->count--;
            //fprintf(stdout, "Hash count: %d\n", *count);
            LeaveCriticalSection(&table->mutex);
            return (uintptr_t)pool_entry;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(&table->mutex);
    return 0;
 
}
uintptr_t search_table_send_frame(TableSendFrame *table, const uint64_t seq_num){
    if(!table->node){
        fprintf(stderr, "ERROR: Invalid node array pointer for hash table - search_tx_frame()\n");
        return 0;
    }
    if(table->size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table search_tx_frame()\n");
        return 0;
    }
    EnterCriticalSection(&table->mutex);
    uint64_t index = get_hash_table_send_frame(seq_num, table->size);

    TableNodeSendFrame *curr = table->node[index];
    TableNodeSendFrame *prev = NULL;
    while (curr) {
        
        PoolEntrySendFrame *pool_entry = (PoolEntrySendFrame*)curr->entry;
        uint64_t frame_seq_num = _ntohll(pool_entry->frame.header.seq_num);

        if (frame_seq_num == seq_num) {
            LeaveCriticalSection(&table->mutex);
            fprintf(stdout, "DEBUG: Found frame node in hash table with seq_num: %llu at index: %llu\n", seq_num, index);
            return (uintptr_t)pool_entry;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(&table->mutex);
    fprintf(stdout, "DEBUG: Node frame not found in hash table with seq_num: %llu\n", seq_num);
    return 0;
 
}


//--------------------------------------------------------------------------------------------------------------------------
void init_table_id(HashTableIdentifierNode *ht, size_t size, const size_t max_nodes){
    
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table ID's init\n");
        return;
    }
    if(max_nodes <= size){
        fprintf(stderr, "ERROR: Invalid max_nodes for hash table ID's init\n");
        return;
    }
    ht->size = size;
    ht->entry = (IdentifierNode **)_aligned_malloc(sizeof(IdentifierNode) * size, 64);
    if(!ht->entry){
        fprintf(stderr, "ERROR: Unable to allocate memory for hash table ID's init\n");
        return;
    }   
    
    init_pool(&ht->pool_nodes, sizeof(IdentifierNode), max_nodes);
    // memset(ht->entry, 0, sizeof(uintptr_t) * size);
    for(int i = 0; i < size; i++){
        ht->entry[i] = NULL;
    }
    InitializeCriticalSection(&ht->mutex); 
    ht->count = 0;
}
uint64_t ht_get_hash_id(uint32_t id, const size_t size) {
    if(size <= 0){
        fprintf(stderr, "ERROR: Invalid size for hash table ID's\n");
        return RET_VAL_ERROR;
    }
    return (id % (uint32_t)size);
}
int ht_insert_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id, const uint8_t status) {

    EnterCriticalSection(&ht->mutex);
    uint64_t index = ht_get_hash_id(id, ht->size);

    IdentifierNode *node = (IdentifierNode*)pool_alloc(&ht->pool_nodes);
    if(node == NULL){
        fprintf(stderr, "ERROR: fail to allocate memory for hash table ID's node\n");
        return RET_VAL_ERROR;
    }

    node->sid = sid;
    node->id = id;    
    node->status = status;
    node->next = (IdentifierNode *)ht->entry[index];  // Insert at the head (linked list)
    ht->entry[index] = node;
    ht->count++;
    LeaveCriticalSection(&ht->mutex);
    return RET_VAL_SUCCESS;
}
void ht_remove_id(HashTableIdentifierNode *ht, const uint32_t sid, const uint32_t id) {
    
    EnterCriticalSection(&ht->mutex);
    uint64_t index = ht_get_hash_id(id, ht->size);
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
            pool_free(&ht->pool_nodes, (void*)curr);
            ht->count--;
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
                pool_free(&ht->pool_nodes, (void*)to_remove);
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
    uint64_t index = ht_get_hash_id(id, ht->size);
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
    uint64_t index = ht_get_hash_id(id, ht->size);
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
            // free(node);
            pool_free(&ht->pool_nodes, (void*)node);
            ht->count--;
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

