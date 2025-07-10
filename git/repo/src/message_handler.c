
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
    uint32_t message_id = _ntohl(frame->payload.long_text_msg.message_id);

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].s_id == session_id && client->msg_stream[i].m_id == message_id){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);        
    }
    return RET_VAL_ERROR;
}
static int msg_validate_fragment(Client *client, const int index, UdpFrame *frame, ServerIOManager* io_manager) {

    EnterCriticalSection(&client->msg_stream[index].lock);

    uint32_t recv_message_id = _ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.long_text_msg.fragment_offset);

    BOOL is_duplicate_fragment = client->msg_stream[index].bitmap && client->msg_stream[index].buffer &&
                                    check_fragment_received(client->msg_stream[index].bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);
    if (is_duplicate_fragment == TRUE) {
        //Client already has bitmap and message buffer allocated so fragment can be processed
        //if the message was already received send duplicate frame ack op_code
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate text message fragment - Session ID: %d, Message ID: %d, Offset: %d,\n", client->session_id, recv_message_id, recv_fragment_offset);
        goto exit_error;
    }
    if(recv_fragment_offset >= recv_message_len){
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    if ((recv_fragment_offset + recv_fragment_len) > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code 
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment len past message bounds! - Session ID: %d, Message ID: %d, Offset: %d, Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    //Success path
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_ERROR;

}
static int msg_get_available_stream_channel(Client *client){
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].busy == FALSE){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);
    }
    return RET_VAL_ERROR;
}
static int msg_init_stream(MsgStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len){

    EnterCriticalSection(&mstream->lock);

    mstream->busy = TRUE;

    mstream->s_id = session_id;
    mstream->m_id = message_id;
    mstream->m_len = message_len;

    // Calculate total fragments
    mstream->fragment_count = (mstream->m_len + (uint64_t)TEXT_FRAGMENT_SIZE - 1ULL) / (uint64_t)TEXT_FRAGMENT_SIZE;
    //fprintf(stdout, "Fragments count: %llu\n", mstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    mstream->bitmap_entries_count = (mstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;  
    //fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", mstream->bitmap_entries_count);

    mstream->bitmap = malloc(mstream->bitmap_entries_count * sizeof(uint64_t));
    if(mstream->bitmap == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        goto exit_error;
    }
    memset(mstream->bitmap, 0, mstream->bitmap_entries_count * sizeof(uint64_t));
    
    //copy the received fragment text to the buffer            
    mstream->m_id = message_id;
    mstream->buffer = malloc(message_len);
    if(mstream->buffer == NULL){
        fprintf(stdout, "Error allocating memory!!!\n");
        goto exit_error;
    }
    memset(mstream->buffer, 0, message_len);

    // Constructs a filename for storing the received message, incorporating session and message IDs for uniqueness.
    snprintf(mstream->file_name, PATH_SIZE, "D:\\E\\msg_SID_%d_UID%d.txt", session_id, message_id);

    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_ERROR;

}
static void msg_attach_fragment(MsgStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len){
    EnterCriticalSection(&mstream->lock);
    char *dest = mstream->buffer + fragment_offset;
    char *src = fragment_buffer;                                              
    memcpy(dest, src, fragment_len);
    mstream->chars_received += fragment_len;       
    mark_fragment_received(mstream->bitmap, fragment_offset, TEXT_FRAGMENT_SIZE);
    LeaveCriticalSection(&mstream->lock);
}
static int msg_check_completion_and_record(MsgStream *mstream, ServerIOManager* io_manager) {
    // Check if the message is fully received by verifying total bytes and the fragment bitmap.
    EnterCriticalSection(&mstream->lock);

    BOOL message_is_complete = (mstream->chars_received == mstream->m_len) && check_bitmap(mstream->bitmap, mstream->fragment_count);

    if (message_is_complete == FALSE) {
        // The message is not yet complete. No action needed for now.
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_SUCCESS;
    }

    // --- Null terminate the message ---
    mstream->buffer[mstream->m_len] = '\0';
    // Attempt to write the in-memory buffer to a file on disk.
    int msg_creation_status = create_output_file(mstream->buffer, mstream->chars_received, mstream->file_name);
    
    // Update the file status in the hash table to mark it as complete.
    // This is done regardless of the file save success, as we won't be receiving more fragments.
    update_uid_status_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, mstream->s_id, mstream->m_id, UID_RECV_COMPLETE);

    // Clean up all dynamically allocated resources for the transfer entry.
    // This block is executed in both success and failure cases of file creation.
    
    message_cleanup_stream(mstream, io_manager);
 
    if (msg_creation_status != RET_VAL_SUCCESS) {
        // If file creation failed, return an error.
        fprintf(stderr, "Error: Failed to create output message for file_id %d\n", mstream->m_id);
        remove(mstream->file_name);
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }
    // File was successfully created and saved.
    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;
}

// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame, ServerIOManager* io_manager){

    int slot; // Declares an integer variable 'slot' to store the index of the message handling slot.
    if(client == NULL){ // Checks if the 'client' pointer is NULL, indicating an invalid or non-existent client context.
        fprintf(stdout, "Received frame for non existing client context!\n"); // Prints an informational message to standard output.
        goto exit_error; // Jumps to the 'exit_error' label for centralized error handling and cleanup.
    }

    EnterCriticalSection(&client->lock); // Acquires a critical section lock associated with the 'client' object. This ensures thread-safe access to 'client' data.

    client->last_activity_time = time(NULL); // Updates the 'last_activity_time' field of the client, typically used for session timeout management.

    // Extracts and converts message-specific fields from the network byte order to host byte order.
    // '_ntohl' converts a 32-bit unsigned integer from network byte order to host byte order.
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_message_id = _ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = _ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = _ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = _ntohl(frame->payload.long_text_msg.fragment_offset);

    // Guard against fragments for already completed messages.
    // Checks if the message, identified by its ID and the client's session ID, has already been marked as fully received in the unique ID hash table.
    if(search_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_message_id, UID_RECV_COMPLETE) == TRUE){
        // If the message is already complete, registers an acknowledgment with a 'STS_TRANSFER_COMPLETE' status.
        // This informs the sender that the message is fully received and no further retransmissions are needed.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, STS_TRANSFER_COMPLETE);
        goto exit_error; // Jumps to the 'exit_error' label.
    }

    // Handle either an existing or a new message stream.
    // Attempts to find an existing message handling slot that matches the incoming fragment.
    slot = msg_match_fragment(client, frame);
    if (slot != RET_VAL_ERROR) {
        // This block is executed if the 'slot' is found, meaning this is a fragment for an existing message.
        // Validates the incoming fragment against the state of the message stream in the identified slot.
        if (msg_validate_fragment(client, slot, frame, io_manager) == RET_VAL_ERROR) {
            goto exit_error; // If fragment validation fails, jumps to the 'exit_error' label.
        }
        // Attaches the fragment's data to the appropriate position within the message buffer managed by the 'message_stream' structure.
        msg_attach_fragment(&client->msg_stream[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        // Acknowledges the successful receipt of the fragment.
        register_ack(&io_manager->queue_seq_num, client, frame, STS_ACK);
        // Checks if all fragments for the message have been received and, if so, finalizes the message (e.g., writes to disk).
        if (msg_check_completion_and_record(&client->msg_stream[slot], io_manager) == RET_VAL_ERROR){
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
        }
    } else {
        // This block is executed if no matching slot is found, suggesting this is the first fragment of a new message.
        // Attempts to obtain an available slot for a new message stream.
        int slot = msg_get_available_stream_channel(client);
        if (slot == RET_VAL_ERROR){ // Checks if an available slot could not be obtained (e.g., maximum streams reached).
            fprintf(stderr, "Maximum message streams reached for client ID: %d\n", client->client_id); // Logs an error message.
            // Registers an ACK with 'ERR_RESOURCE_LIMIT' status, informing the sender of the resource constraint.
            register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_RESOURCE_LIMIT);
            goto exit_error; // Jumps to the 'exit_error' label.
        }
        // Validates the fragment. This is particularly important for the first fragment, which often contains total message length.
        if (msg_validate_fragment(client, slot, frame, io_manager) == RET_VAL_ERROR){
            goto exit_error; // If validation fails, jumps to the 'exit_error' label.
        }
        // Initializes the message receiving slot with the message ID and its total expected length.
        if (msg_init_stream(&client->msg_stream[slot], recv_session_id, recv_message_id, recv_message_len) == RET_VAL_ERROR){
            goto exit_error; // If slot initialization fails, jumps to the 'exit_error' label.
        }

        // Attaches the first fragment's data to the newly initialized message buffer.     
        fprintf(stdout, "Received first message fragment Session ID: %u, Message ID: %d, Size: %u\n", recv_session_id, recv_message_id, recv_message_len);

        fprintf(stdout, "Opened message stream: %u\n", slot);

        msg_attach_fragment(&client->msg_stream[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        
        // Adds an entry to the unique ID hash table, marking the message as awaiting further fragments.
        add_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_message_id, UID_WAITING_FRAGMENTS);
        // Acknowledges the successful receipt of this initial fragment.
        register_ack(&io_manager->queue_seq_num, client, frame, STS_ACK);
        // Checks if the message is now complete (e.g., if it was a single-fragment message) and finalizes it.
        if (msg_check_completion_and_record(&client->msg_stream[slot], io_manager) == RET_VAL_ERROR){
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
        }
        LeaveCriticalSection(&client->lock); // Releases the critical section lock.
        return RET_VAL_SUCCESS; // Returns success, indicating the message fragment was handled.
    }

exit_error: // Label for centralized error handling.
    LeaveCriticalSection(&client->lock); // Ensures the critical section lock is released before exiting, preventing deadlocks.
    return RET_VAL_ERROR; // Returns an error status.
}


