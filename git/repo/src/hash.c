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

uint64_t get_hash_frame(const uint64_t key) {
    return (key % HASH_SIZE_FRAME);
}
int insert_frame(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, UdpFrame *frame, uint32_t *count, MemPool *pool) {
    
    EnterCriticalSection(mutex);
    
    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint64_t index = get_hash_frame(seq_num);
    //fprintf(stdout, "SeqNum: %llu inserted at index: %llu\n", seq_num, index);
    FramePendingAck *node = (FramePendingAck *)pool_alloc(pool);
    if(node == NULL){
        return RET_VAL_ERROR;
    }
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->counter = 1;

    node->next = (FramePendingAck *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    (*count)++;

    LeaveCriticalSection(mutex);
    return RET_VAL_SUCCESS;
}
void remove_frame(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, uint64_t seq_num, uint32_t *count, MemPool *pool) {

    EnterCriticalSection(mutex);

    uint64_t index = get_hash_frame(seq_num);
    //fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
    FramePendingAck *curr = hash_table[index];
    FramePendingAck *prev = NULL;
    while (curr) {
        if (_ntohll(curr->frame.header.seq_num) == seq_num) {
            //fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
            // Found it
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;
            }
            pool_free(pool, curr);
            //free(curr);
            (*count)--;
            //fprintf(stdout, "Hash count: %d\n", *count);
            LeaveCriticalSection(mutex);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(mutex);
}
void clean_frame_hash_table(FramePendingAck *hash_table[], CRITICAL_SECTION *mutex, uint32_t *count, MemPool *pool) {
    
    EnterCriticalSection(mutex);
    
    FramePendingAck *head = NULL;
    for (int i = 0; i < HASH_SIZE_FRAME; i++) {
        if(hash_table[i]){       
            FramePendingAck *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    pool_free(pool, head);
                    //free(head);
                    (*count)--;
            }
            pool_free(pool, ptr);
            //free(ptr);
            hash_table[i] = NULL;
        }     
    }
//    fprintf(stdout, "Frame hash table clean\n");
    LeaveCriticalSection(mutex);
    return;
}

//--------------------------------------------------------------------------------------------------------------------------

uint64_t get_hash_uid(uint32_t u_id) {
    return (u_id % HASH_SIZE_UID);
}
void add_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status) {
    
    EnterCriticalSection(mutex);
    
    uint64_t index = get_hash_uid(u_id);
    
    UniqueIdentifierNode *head = (UniqueIdentifierNode *)malloc(sizeof(UniqueIdentifierNode));
    head->u_id = u_id;
    head->s_id = s_id;
    head->status = UID_WAITING_FRAGMENTS;
    fprintf(stdout, "Added to hash table SID: %d - UID: %d\n", head->s_id, head->u_id);
    head->next = (UniqueIdentifierNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = head;
    LeaveCriticalSection(mutex);
    return;
}
void remove_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id) {
    
    EnterCriticalSection(mutex);

    uint64_t index = get_hash_uid(u_id); 
    UniqueIdentifierNode *curr = hash_table[index];
    UniqueIdentifierNode *prev = NULL;
    while (curr) {     
        if (curr->u_id == u_id && curr->s_id == s_id) {
            // Found it
            //fprintf(stdout, "Removing UID: %d, SID: %d from hash table\n", curr->u_id, curr->s_id);
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;
            }
            free(curr);
            LeaveCriticalSection(mutex);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(mutex);
    return;
}
BOOL search_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status) {
    
    EnterCriticalSection(mutex);
    
    uint64_t index = get_hash_uid(u_id);
    UniqueIdentifierNode *node = hash_table[index];
    while (node) {
        if (node->s_id == s_id && node->u_id == u_id && node->status == status){
            //fprintf(stdout, "Found in hash table UID: %d, session ID: %d, status %d\n", node->u_id, node->s_id, node->status);
            LeaveCriticalSection(mutex);
            return TRUE;
        }           
        node = node->next;
    }
    LeaveCriticalSection(mutex);
    return FALSE;
}
int update_uid_status_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex, const uint32_t s_id, const uint32_t u_id, const uint8_t status) {
    
    EnterCriticalSection(mutex);
    
    uint64_t index = get_hash_uid(u_id);
    UniqueIdentifierNode *node = hash_table[index];
    while (node) {
        if (node->u_id == u_id && node->s_id == s_id){
            node->status = status;
            //fprintf(stdout, "Updated in hash table SID: %d UID: %d, new status %d\n", node->session_id, node->u_id, node->status);
            LeaveCriticalSection(mutex);
            return RET_VAL_SUCCESS;
        }           
        node = node->next;
    }
    LeaveCriticalSection(mutex);
    return RET_VAL_ERROR;

}
void clean_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex) {
    
    EnterCriticalSection(mutex);
    
    UniqueIdentifierNode *head = NULL;
    for (int i = 0; i < HASH_SIZE_UID; i++) {
        if(hash_table[i]){       
            UniqueIdentifierNode *node = hash_table[i];
            while (node) {
                    head = node;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    node = node->next;
                    free(head);
            }
            free(node);
            hash_table[i] = NULL;
        }     
    }
    LeaveCriticalSection(mutex);
    return;
}
void print_uid_hash_table(UniqueIdentifierNode *hash_table[], CRITICAL_SECTION *mutex) {
    
    EnterCriticalSection(mutex);
    
    for (int i = 0; i < HASH_SIZE_UID; i++) {
        if(hash_table[i]){
            printf("BUCKET %d: \n", i);           
            UniqueIdentifierNode *node = hash_table[i];
            while (node) {
                    fprintf(stdout, "SID: %d - UID: %d\n", node->s_id, node->u_id);                   
                    node = node->next;
            }
        }     
    }
    LeaveCriticalSection(mutex);
}
