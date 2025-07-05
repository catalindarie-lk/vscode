
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "netendians.h"
#include "frames.h"
#include "mem_pool.h"
#include "hash.h"

//--------------------------------------------------------------------------------------------------------------------------

uint64_t get_hash_frame(const uint64_t key){
    return (key % HASH_SIZE_FRAME);
}
int insert_frame(AckHashNode *hash_table[], CRITICAL_SECTION *mutex, UdpFrame *frame, uint32_t *count, MemPool *pool) {
    
    EnterCriticalSection(mutex);
    
    uint64_t seq_num = _ntohll(frame->header.seq_num);
    uint64_t index = get_hash_frame(seq_num);
    //fprintf(stdout, "SeqNum: %llu inserted at index: %llu\n", seq_num, index);
    AckHashNode *node = (AckHashNode *)pool_alloc(pool);
    if(node == NULL){
        return RET_VAL_ERROR;
    }
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->counter = 1;

    node->next = (AckHashNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    InterlockedIncrement(count);

    LeaveCriticalSection(mutex);
    return RET_VAL_SUCCESS;
}
void remove_frame(AckHashNode *hash_table[], CRITICAL_SECTION *mutex, uint64_t seq_num, uint32_t *count, MemPool *pool) {

    EnterCriticalSection(mutex);

    uint64_t index = get_hash_frame(seq_num);
    //fprintf(stdout, "Removing frame with seq num: %llu from index: %llu\n", seq_num, index);
    AckHashNode *curr = hash_table[index];
    AckHashNode *prev = NULL;
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
            InterlockedDecrement(count);
            //fprintf(stdout, "Hash count: %d\n", *count);
            LeaveCriticalSection(mutex);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    LeaveCriticalSection(mutex);
}
void clean_frame_hash_table(AckHashNode *hash_table[], CRITICAL_SECTION *mutex, uint32_t *count, MemPool *pool){
    
    EnterCriticalSection(mutex);
    
    AckHashNode *head = NULL;
    for (int i = 0; i < HASH_SIZE_FRAME; i++) {
        if(hash_table[i]){       
            AckHashNode *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    pool_free(pool, head);
                    //free(head);
                    InterlockedDecrement(count);
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

uint64_t get_hash_uid(uint32_t uid){
    return (uid % HASH_SIZE_UID);
}
void add_uid_hash_table(UniqueIdentifierNode *hash_table[], uint32_t uid, uint32_t session_id, uint8_t status){
    uint64_t index = get_hash_uid(uid);
    
    UniqueIdentifierNode *head = (UniqueIdentifierNode *)malloc(sizeof(UniqueIdentifierNode));
    head->uid = uid;
    head->session_id = session_id;
    head->status = UID_WAITING_FRAGMENTS;
    fprintf(stdout, "Added to hash table SID: %d - UID: %d\n", head->session_id, head->uid);
    head->next = (UniqueIdentifierNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = head;
    return;
}
void remove_uid_hash_table(UniqueIdentifierNode *hash_table[], uint32_t uid) {
    uint64_t index = get_hash_uid(uid); 
    UniqueIdentifierNode *curr = hash_table[index];
    UniqueIdentifierNode *prev = NULL;
    while (curr) {     
        if (curr->uid == uid) {
//            fprintf(stdout, "Removing seq num: %d from index: %d\n", seq_num, index);
            if (prev) {
                prev->next = curr->next;
            } else {
                hash_table[index] = curr->next;
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    return;
}
BOOL search_uid_hash_table(UniqueIdentifierNode *hash_table[], uint32_t uid, uint32_t session_id, uint8_t status) {
    uint64_t index = get_hash_uid(uid);
    UniqueIdentifierNode *node = hash_table[index];
    while (node) {
        if (node->uid == uid && node->session_id == session_id && node->status == status){
            //fprintf(stdout, "Found in hash table UID: %d, session ID: %d, status %d\n", node->uid, node->session_id, node->status);
            return TRUE;
        }           
        node = node->next;
    }
    return FALSE;
}
int update_uid_status_hash_table(UniqueIdentifierNode *hash_table[], const uint32_t session_id, uint32_t uid, uint8_t status){
    uint64_t index = get_hash_uid(uid);
    UniqueIdentifierNode *node = hash_table[index];
    while (node) {
        if (node->uid == uid && node->session_id == session_id){
            node->status = status;
            //fprintf(stdout, "Updated in hash table SID: %d UID: %d, new status %d\n", node->session_id, node->uid, node->status);
            return RET_VAL_SUCCESS;
        }           
        node = node->next;
    }
    return RET_VAL_ERROR;

}
void clean_uid_hash_table(UniqueIdentifierNode *hash_table[]){
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
    return;
}
void print_uid_hash_table(UniqueIdentifierNode *hash_table[]) {
    for (int i = 0; i < HASH_SIZE_UID; i++) {
        if(hash_table[i]){
            printf("BUCKET %d: \n", i);           
            UniqueIdentifierNode *node = hash_table[i];
            while (node) {
                    fprintf(stdout, "SID: %d - UID: %d\n", node->session_id, node->uid);                   
                    node = node->next;
            }
        }     
    }
}
