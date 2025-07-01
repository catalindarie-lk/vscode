#ifndef _UDP_HASH_H
#define _UDP_HASH_H

#include "UDP_lib.h"

#define HASH_SIZE               65536
#define HASH_SIZE_UID           1024
#define HASH_HIGH_WATERMARK     65536
#define HASH_LOW_WATERMARK      32768

#define BLOCK_SIZE              (sizeof(AckHashNode))
#define BLOCK_COUNT             (HASH_HIGH_WATERMARK * 2)

typedef struct {
    uint8_t* memory;             // Raw memory buffer
    int free_head;               // Index of the first free block
    int next[BLOCK_COUNT];       // Next free block indices
    BOOL used[BLOCK_COUNT];      // Usage flags (optional, for safety/debugging)
} MemPool;

typedef uint8_t HashMessageStatus;
enum HashMessageStatus{
    UID_WAITING_FRAGMENTS = 1,
    UID_RECV_COMPLETE= 2
};

typedef struct AckHashNode{
    UdpFrame frame;
    time_t time;
    uint16_t counter;
    struct AckHashNode *next;
}AckHashNode;

typedef struct SeqNumNode{
    uint64_t seq_num;
    uint32_t id;
    struct SeqNumNode *next;
}SeqNumNode;

typedef struct UniqueIdentifierNode{
    uint32_t uid;
    uint32_t session_id;
    uint8_t status;                         //1 - Pending; 2 - Finished
    struct UniqueIdentifierNode *next;
}UniqueIdentifierNode;

//--------------------------------------------------------------------------------------------------------------------------
void pool_init(MemPool* pool) {
    pool->memory = malloc(BLOCK_SIZE * BLOCK_COUNT);
    pool->free_head = 0;

    for (int i = 0; i < BLOCK_COUNT - 1; i++) {
        pool->next[i] = i + 1;
        pool->used[i] = FALSE;
    }
    pool->next[BLOCK_COUNT - 1] = -1; // End of free list
    pool->used[BLOCK_COUNT - 1] = FALSE;
}
void* pool_alloc(MemPool* pool) {
    if (pool->free_head == -1) return NULL; // Pool exhausted

    int index = pool->free_head;
    pool->free_head = pool->next[index];
    pool->used[index] = TRUE;

    return (void *)(pool->memory + index * BLOCK_SIZE);
}
void pool_free(MemPool* pool, void* ptr) {
    int index = ((uint8_t*)ptr - pool->memory) / BLOCK_SIZE;
    if (index < 0 || index >= BLOCK_COUNT || !pool->used[index]) return;

    pool->next[index] = pool->free_head;
    pool->free_head = index;
    pool->used[index] = FALSE;
}
void pool_destroy(MemPool* pool) {
    free(pool->memory);
    pool->memory = NULL;
}
//--------------------------------------------------------------------------------------------------------------------------
uint16_t get_hash(uint64_t seq_num){
    return (seq_num % HASH_SIZE);
}
uint16_t get_hash_uid(uint32_t uid){
    return (uid % HASH_SIZE_UID);
}
int insert_frame(AckHashNode *hash_table[], UdpFrame *frame, uint32_t *count, MemPool *pool) {
    uint64_t seq_num = ntohll(frame->header.seq_num);
    uint16_t index = get_hash(seq_num);
//    fprintf(stdout, "SeqNum: %d inserted at index: %d\n", seq_num, index);
    AckHashNode *node = (AckHashNode *)pool_alloc(pool);//         malloc(sizeof(AckHashNode));
    if(node == NULL){
        return RET_VAL_ERROR;
    }
    memcpy(&node->frame, frame, sizeof(UdpFrame));
    node->time = time(NULL);
    node->counter = 1;

    node->next = (AckHashNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    (*count)++;
    return RET_VAL_SUCCESS;
}
void remove_frame(AckHashNode *hash_table[], uint64_t seq_num, uint32_t *count, MemPool *pool) {
    uint16_t index = get_hash(seq_num);
    AckHashNode *curr = hash_table[index];
    AckHashNode *prev = NULL;
    while (curr) {      
        if (ntohll(curr->frame.header.seq_num) == seq_num) {
            //fprintf(stdout, "Removing frame with seq num: %zu from index: %d\n", seq_num, index);
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
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}
void clean_frame_hash_table(AckHashNode *hash_table[], uint32_t *count){
    AckHashNode *head = NULL;
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){       
            AckHashNode *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    free(head);
                    (*count)--;
            }
            free(ptr);
            hash_table[i] = NULL;
        }     
    }
//    fprintf(stdout, "Frame hash table clean\n");
    return;
}
//--------------------------------------------------------------------------------------------------------------------------
void insert_seq_num(SeqNumNode *hash_table[], uint64_t seq_num, uint32_t id) {
    uint16_t index = get_hash(seq_num);
    //fprintf(stdout, "SeqNum: %d inserted at index: %d\n", seq_num, index);
    SeqNumNode *node = (SeqNumNode *)malloc(sizeof(SeqNumNode));
    node->seq_num = seq_num;
    node->id = id;
 
    node->next = (SeqNumNode *)hash_table[index];  // Insert at the head (linked list)
    hash_table[index] = node;
    return;
}
void remove_seq_num(SeqNumNode *hash_table[], uint64_t seq_num) {
    uint16_t index = get_hash(seq_num); 
    SeqNumNode *curr = hash_table[index];
    SeqNumNode *prev = NULL;
    while (curr) {     
        if (curr->seq_num == seq_num) {
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
SeqNumNode *search_seq_num(SeqNumNode *hash_table[], uint64_t seq_num, uint32_t id) {
    uint16_t index = get_hash(seq_num);
    SeqNumNode *ptr = hash_table[index];
    while (ptr) {
        if (ptr->seq_num == seq_num && ptr->id == id){
            fprintf(stdout, "Received double SeqNum: %llu for ID: %d\n", ptr->seq_num, ptr->id);
            return ptr;
        }           
        ptr = ptr->next;
    }
    return NULL;
}
void print_seq_num_table(SeqNumNode *hash_table[]) {
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){
            printf("BUCKET %d: \n", i);           
            SeqNumNode *ptr = hash_table[i];
            while (ptr) {     
                    fprintf(stdout, "Bucket: %d - SeqNum: %llu\n", i, ptr->seq_num);                   
                    ptr = ptr->next;
            }
        }     
    }
    return;
}
void clean_seq_num_hash_table(SeqNumNode *hash_table[]){
    SeqNumNode *head = NULL;
    for (int i = 0; i < HASH_SIZE; i++) {
        if(hash_table[i]){       
            SeqNumNode *ptr = hash_table[i];
            while (ptr) {
                    head = ptr;
                    //fprintf(stdout, "Bucket: %d - Freeing SeqNum: %d\n", i, head->seq_num);                   
                    ptr = ptr->next;
                    free(head);
            }
            free(ptr);
            hash_table[i] = NULL;
        }     
    }
    return;
}
//--------------------------------------------------------------------------------------------------------------------------
void add_uid_hash_table(UniqueIdentifierNode *hash_table[], uint32_t uid, uint32_t session_id, uint8_t status){
    uint16_t index = get_hash_uid(uid);
    
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
    uint16_t index = get_hash_uid(uid); 
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
    uint16_t index = get_hash_uid(uid);
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
    uint16_t index = get_hash_uid(uid);
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

#endif