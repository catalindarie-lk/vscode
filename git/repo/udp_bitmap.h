#ifndef _UDP_BITMAP_H
#define _UDP_BITMAP_H


#include "UDP_lib.h"

// To mark a fragment as received
void mark_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
    uint64_t bitmap_index = fragment_offset / fragment_size;
    bitmap[bitmap_index / 64ULL] |= (1ULL << (bitmap_index % 64ULL));
    //fprintf(stdout, "Marking fragment received %llu, bitmap index: %llu\n", fragment_offset, bitmap_index);
    //fprintf(stdout, "bitmap[%llu] = %llX\n", bitmap_index / 64ULL, bitmap[bitmap_index / 64ULL]);
    return;
 
}

// To check if already received
BOOL check_fragment_received(uint64_t bitmap[], uint64_t fragment_offset, uint32_t fragment_size) {
  
    uint64_t bitmap_index = fragment_offset / fragment_size;  
    //fprintf(stdout, "Check fragment received %llu, bitmap index: %llu\n", fragment_offset, bitmap_index);
    return (bitmap[bitmap_index / 64ULL] & (1ULL << (bitmap_index % 64ULL))) != 0ULL;
}

// Check if bitmap is full (all fragments received)
BOOL check_bitmap(uint64_t bitmap[], uint32_t fragment_count){

    uint64_t bitmap_index;
    bitmap_index = fragment_count / 64ULL;
    if((uint64_t)fragment_count % 64ULL > 0ULL){
        bitmap_index++;
    }
    uint64_t mask = 0xFFFFFFFFFFFFFFFF;
    for(uint64_t i = 0; i < bitmap_index - 1ULL; i++){
        if(bitmap[i] != mask){
            fprintf(stdout, "Bitmap NOK\n");
            return FALSE;
        }             
    }

    mask = 0x0000000000000000;
    if (fragment_count % 64ULL == 0ULL) {
        return TRUE;
    } else {
        mask = (1ULL << fragment_count % 64ULL) - 1ULL;
        if(bitmap[bitmap_index - 1ULL] != mask){
            fprintf(stdout, "Bitmap NOK\n");
            return FALSE;
        } 
    }
    return(TRUE);    
}

#endif