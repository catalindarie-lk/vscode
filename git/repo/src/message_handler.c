
#include <stdint.h>
#include <stdio.h>
#include <time.h>
//#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "include/message_handler.h"
#include "include/protocol_frames.h"
#include "include/netendians.h"
#include "include/queue.h"
#include "include/hash.h"
#include "include/bitmap.h"
#include "include/checksum.h"
#include "include/mem_pool.h"
#include "include/fileio.h"
#include "include/server.h"

// Handle message fragment helper functions
static int msg_match_fragment(Client *client, UdpFrame *frame){

    uint32_t session_id = _ntohl(frame->header.session_id);    
    uint32_t message_id = _ntohl(frame->payload.text_fragment.message_id);

    for(int i = 0; i < MAX_SERVER_ACTIVE_MSTREAMS; i++){
        EnterCriticalSection(&client->mstream[i].lock);
        if(client->mstream[i].sid == session_id && client->mstream[i].mid == message_id){
            LeaveCriticalSection(&client->mstream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->mstream[i].lock);        
    }
    return RET_VAL_ERROR;
}
static int msg_validate_fragment(Client *client, const int index, UdpFrame *frame) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    MessageStream *mstream = &client->mstream[index];

    EnterCriticalSection(&client->lock);
    EnterCriticalSection(&mstream->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_message_id = _ntohl(frame->payload.text_fragment.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.text_fragment.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.text_fragment.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.text_fragment.fragment_offset);

    uint8_t op_code = 0;
    PoolEntryAckFrame *entry = NULL;

    BOOL is_duplicate_fragment = mstream->bitmap && mstream->buffer &&
                                    check_fragment_received(mstream->bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);

    if (is_duplicate_fragment == TRUE) {
        fprintf(stderr, "ERROR: Received duplicate text message fragment - Session ID: %d, Message ID: %d, Offset: %d,\n", client->sid, recv_message_id, recv_fragment_offset);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;

    }
    if(recv_fragment_offset >= recv_message_len){
        fprintf(stderr, "ERROR: Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", client->sid, recv_message_id, recv_fragment_offset, recv_fragment_len);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }
    if ((recv_fragment_offset + recv_fragment_len) > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        fprintf(stderr, "ERROR: Fragment len past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", client->sid, recv_message_id, recv_fragment_offset, recv_fragment_len);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    LeaveCriticalSection(&mstream->lock);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_SUCCESS;

exit_err:

    entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_udp_frame);
    if(!entry){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment ack error frame\n");
        LeaveCriticalSection(&client->lock);
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_frame(queue_priority_ack_udp_frame, (uintptr_t)entry) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push to queue priority.\n");
        pool_free(pool_queue_ack_udp_frame, entry);
    }
    LeaveCriticalSection(&mstream->lock);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;
}
static int msg_get_available_stream_channel(Client *client){
    for(int i = 0; i < MAX_SERVER_ACTIVE_MSTREAMS; i++){
        EnterCriticalSection(&client->mstream[i].lock);
        if(client->mstream[i].busy == FALSE){
            LeaveCriticalSection(&client->mstream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->mstream[i].lock);
    }
    return RET_VAL_ERROR;
}
static int msg_init_stream(MessageStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len){

    EnterCriticalSection(&mstream->lock);

    mstream->busy = TRUE;

    mstream->sid = session_id;
    mstream->mid = message_id;
    mstream->mlen = message_len;

    // Calculate total fragments
    mstream->fragment_count = (mstream->mlen + (uint64_t)TEXT_FRAGMENT_SIZE - 1ULL) / (uint64_t)TEXT_FRAGMENT_SIZE;

    // Calculate number of 64-bit bitmap entries (chunks)
    mstream->bitmap_entries_count = (mstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;  
    //fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", mstream->bitmap_entries_count);

    mstream->bitmap = malloc(mstream->bitmap_entries_count * sizeof(uint64_t));
    if(mstream->bitmap == NULL){
        fprintf(stderr, "ERROR: Memory allocation fail for file bitmap!!!\n");
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }
    memset(mstream->bitmap, 0, mstream->bitmap_entries_count * sizeof(uint64_t));
    
    //copy the received fragment text to the buffer            
    mstream->mid = message_id;
    mstream->buffer = malloc(message_len);
    if(mstream->buffer == NULL){
        fprintf(stderr, "ERROR: Failed to allocate memory for message buffer - malloc(message_len)\n");
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }
    memset(mstream->buffer, 0, message_len);

    char messageFolder[MAX_PATH];
    snprintf(messageFolder, MAX_PATH, "%s", SERVER_MESSAGE_TEXT_FILES_FOLDER);

    if (CreateAbsoluteFolderRecursive(messageFolder) == FALSE) {
        fprintf(stderr, "ERROR: Failed to create recursive path for message folder: \"%s\". Error code: %lu\n", messageFolder, GetLastError());
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }

    // Constructs a filename for storing the received message, incorporating session and message IDs for uniqueness.
    snprintf(mstream->fnm, MAX_PATH, SERVER_MESSAGE_TEXT_FILES_FOLDER"xmessage_SID_%d_ID%d.txt", session_id, message_id);

    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;


}
static void msg_attach_fragment(MessageStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len){
    EnterCriticalSection(&mstream->lock);
    char *dest = mstream->buffer + fragment_offset;
    char *src = fragment_buffer;                                              
    memcpy(dest, src, fragment_len);
    mstream->chars_received += fragment_len;       
    mark_fragment_received(mstream->bitmap, fragment_offset, TEXT_FRAGMENT_SIZE);
    LeaveCriticalSection(&mstream->lock);
    return;
}
static int msg_check_completion_and_record(MessageStream *mstream) {
    // Check if the message is fully received by verifying total bytes and the fragment bitmap.
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    EnterCriticalSection(&mstream->lock);

    BOOL message_is_complete = (mstream->chars_received == mstream->mlen) && check_bitmap(mstream->bitmap, mstream->fragment_count);

    if (message_is_complete == FALSE) {
        // The message is not yet complete. No action needed for now.
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_SUCCESS;
    }

    // --- Null terminate the message ---
    mstream->buffer[mstream->mlen] = '\0';
    // Attempt to write the in-memory buffer to a file on disk.
    int msg_creation_status = create_output_file(mstream->buffer, mstream->chars_received, mstream->fnm);
    
    ht_update_id_status(ht_mid, mstream->sid, mstream->mid, ID_RECV_COMPLETE);
    
    close_message_stream(mstream);
 
    if (msg_creation_status != RET_VAL_SUCCESS) {
        // If file creation failed, return an error.
        fprintf(stderr, "ERROR: Failed to create output message for file_id %d\n", mstream->mid);
        remove(mstream->fnm);
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }
    // File was successfully created and saved.
    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;
}

// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    int slot;
    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    EnterCriticalSection(&client->lock);

    client->last_activity_time = time(NULL);
    
    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_message_id = _ntohl(frame->payload.text_fragment.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.text_fragment.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.text_fragment.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.text_fragment.fragment_offset);

    uint8_t op_code = 0;
    PoolEntryAckFrame *entry = NULL;

    if(ht_search_id(ht_mid, recv_session_id, recv_message_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received file end frame for completed file Seq: %llu; sID: %u; mID: %u;\n", recv_seq_num, recv_session_id, recv_message_id);
        op_code = ERR_EXISTING_MESSAGE;
        goto exit_err;
    }

    slot = msg_match_fragment(client, frame);
    if (slot != RET_VAL_ERROR) {
        if (msg_validate_fragment(client, slot, frame) == RET_VAL_ERROR) {
            LeaveCriticalSection(&client->lock);
            return RET_VAL_ERROR;
        }
        msg_attach_fragment(&client->mstream[slot], frame->payload.text_fragment.chars, recv_fragment_offset, recv_fragment_len);
        
        if (msg_check_completion_and_record(&client->mstream[slot]) == RET_VAL_ERROR){
            fprintf(stderr, "Final check of the message failed\n");
            op_code = ERR_MESSAGE_FINAL_CHECK;
            goto exit_err;
        }

        entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_udp_frame);
        if(!entry){
            fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment ack frame\n");
            LeaveCriticalSection(&client->lock);
            return RET_VAL_ERROR;
        }
        construct_ack_frame(entry, recv_seq_num, recv_session_id, STS_FRAME_DATA_ACK, server->socket, &client->client_addr);
        if(push_frame(queue_message_ack_udp_frame, (uintptr_t)entry) == RET_VAL_ERROR){
            fprintf(stderr, "ERROR: Failed to push to queue message ack.\n");
            pool_free(pool_queue_ack_udp_frame, entry);
        }
        LeaveCriticalSection(&client->lock);
        return RET_VAL_SUCCESS;

    } else {
        int slot = msg_get_available_stream_channel(client);
        if (slot == RET_VAL_ERROR){
            fprintf(stderr, "Maximum message streams reached for client ID: %d\n", client->cid);
            op_code = ERR_RESOURCE_LIMIT;
            goto exit_err;
        }
        if (msg_validate_fragment(client, slot, frame) == RET_VAL_ERROR){
            LeaveCriticalSection(&client->lock);
            return RET_VAL_ERROR;
        }
        if (msg_init_stream(&client->mstream[slot], recv_session_id, recv_message_id, recv_message_len) == RET_VAL_ERROR){
            op_code = ERR_STREAM_INIT;
            goto exit_err;
        }

        fprintf(stdout, "Received first message fragment Session ID: %u, Message ID: %d, Size: %u\n", recv_session_id, recv_message_id, recv_message_len);
        fprintf(stdout, "Opened message stream: %u\n", slot);

        msg_attach_fragment(&client->mstream[slot], frame->payload.text_fragment.chars, recv_fragment_offset, recv_fragment_len);       

        if(ht_insert_id(ht_mid, recv_session_id, recv_message_id, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
            fprintf(stderr, "Failed to allocate memory for message ID in hash table\n");
            op_code = ERR_MEMORY_ALLOCATION;
            goto exit_err;
        }

        if (msg_check_completion_and_record(&client->mstream[slot]) == RET_VAL_ERROR){
            fprintf(stderr, "Final check of the message failed\n");
            op_code = ERR_MESSAGE_FINAL_CHECK;
            goto exit_err;
        }

        entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_udp_frame);
        if(!entry){
            fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment ack frame\n");
            LeaveCriticalSection(&client->lock);
            return RET_VAL_ERROR;
        }
        construct_ack_frame(entry, recv_seq_num, recv_session_id, STS_FRAME_DATA_ACK, server->socket, &client->client_addr);
        if(push_frame(queue_message_ack_udp_frame, (uintptr_t)entry) == RET_VAL_ERROR){
            fprintf(stderr, "ERROR: Failed to push message ack to queue.\n");
            pool_free(pool_queue_ack_udp_frame, entry);
        }
        LeaveCriticalSection(&client->lock);
        return RET_VAL_SUCCESS;
    }
exit_err:

    entry = (PoolEntryAckFrame*)pool_alloc(pool_queue_ack_udp_frame);
    if(!entry){
        fprintf(stderr, "ERROR: Failed to allocate memory in the pool for message fragment error ack frame\n");
        LeaveCriticalSection(&client->lock);
        return RET_VAL_ERROR;
    }
    construct_ack_frame(entry, recv_seq_num, recv_session_id, op_code, server->socket, &client->client_addr);
    if(push_frame(queue_priority_ack_udp_frame, (uintptr_t)entry) == RET_VAL_ERROR){
        fprintf(stderr, "ERROR: Failed to push message ack error to queue priority.\n");
        pool_free(pool_queue_ack_udp_frame, entry);
    }

    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;
}

