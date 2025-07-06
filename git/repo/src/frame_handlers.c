
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <winsock2.h>
#include <windows.h>
#include "frame_handlers.h"
#include "frames.h"
#include "hash.h"
#include "queue.h"
#include "server.h"




// Handle message fragment helper functions
static int msg_match_fragment(ClientData *client, UdpFrame *frame){
    
    uint32_t message_id = ntohl(frame->payload.long_text_msg.message_id);

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].m_id == message_id && client->msg_stream[i].buffer != NULL && client->msg_stream[i].bitmap != NULL){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);        
    }
    return RET_VAL_ERROR;
}
static int msg_validate_fragment(ClientData *client, const int index, UdpFrame *frame, ServerIOManager* io_manager) {

    EnterCriticalSection(&client->msg_stream[index].lock);

    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    BOOL is_duplicate_fragment = client->msg_stream[index].bitmap && client->msg_stream[index].buffer &&
                                    check_fragment_received(client->msg_stream[index].bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);
    if (is_duplicate_fragment == TRUE) {
        //Client already has bitmap and message buffer allocated so fragment can be processed
        //if the message was already received send duplicate frame ack op_code
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate text message fragment! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    if(recv_fragment_offset >= recv_message_len){
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    if ((recv_fragment_offset + recv_fragment_len) > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code 
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment len past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    //Success path
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_ERROR;

}
static int msg_get_available_stream_channel(ClientData *client){
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].m_id == 0 && client->msg_stream[i].buffer == NULL && client->msg_stream[i].bitmap == NULL){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);
    }
    return RET_VAL_ERROR;
}
static int msg_init_stream(MsgStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len){

    EnterCriticalSection(&mstream->lock);

    mstream->s_id = session_id;
    mstream->m_id = message_id;
    mstream->m_len = message_len;

    // Calculate total fragments
    mstream->fragment_count = (mstream->m_len + (uint64_t)TEXT_FRAGMENT_SIZE - 1ULL) / (uint64_t)TEXT_FRAGMENT_SIZE;
    fprintf(stdout, "Fragments count for message: %llu\n", mstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    mstream->bitmap_entries_count = (mstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;  
    fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", mstream->bitmap_entries_count);

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
    snprintf(mstream->file_name, PATH_SIZE, "E:\\msg_SID_%d_UID%d.txt", session_id, message_id);

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
    update_uid_status_hash_table(io_manager->uid_hash_table, mstream->s_id, mstream->m_id, UID_RECV_COMPLETE);

    // Clean up all dynamically allocated resources for the transfer entry.
    // This block is executed in both success and failure cases of file creation.
    mstream->s_id = 0;
    mstream->m_id = 0;
    mstream->chars_received = 0;
    mstream->fragment_count = 0;
    mstream->bitmap_entries_count = 0;
    
    if(mstream->buffer != NULL){
        free(mstream->buffer);
        mstream->buffer = NULL;
    }
    if(mstream->bitmap != NULL){
        free(mstream->bitmap);
        mstream->bitmap = NULL;
    }
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

// Handle file fragment helper functions
static int file_match_fragment(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){
    // Iterate through all possible file stream slots associated with this client.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        // This prevents other threads from modifying or accessing this specific file stream's data
        // (like its busy status, session ID, or file ID) while this function is checking it.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Check if the current file stream slot matches the incoming fragment:
        // 1. client->file_stream[i].busy == TRUE: Ensure the slot is currently active and in use for a transfer.
        // 2. client->file_stream[i].s_id == session_id: Match the session ID from the incoming fragment.
        // 3. client->file_stream[i].fid == file_id: Match the file ID from the incoming fragment.
        if(client->file_stream[i].busy == TRUE && client->file_stream[i].s_id == session_id && client->file_stream[i].f_id == file_id){
            // If a matching file stream slot is found, release its lock.
            LeaveCriticalSection(&client->file_stream[i].lock);
            // Return the index of the matching slot.
            return i;
        }
        // If the current file stream slot does not match the fragment, release its lock
        // before proceeding to check the next slot. This is crucial to avoid deadlocks.
        LeaveCriticalSection(&client->file_stream[i].lock);
    }
    // If the loop completes without finding any file stream slot that matches the session_id and file_id,
    // it means no active transfer for this client corresponds to the incoming fragment.
    return RET_VAL_ERROR; // Return an error value to indicate no match was found.
}
static int file_check_metadata(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){
    // This function checks if a file stream with the given session_id and file_id already exists for the client. 
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){      
        EnterCriticalSection(&client->file_stream[i].lock);       
        if(client->file_stream[i].s_id == session_id && client->file_stream[i].f_id == file_id){          
            LeaveCriticalSection(&client->file_stream[i].lock);          
            return RET_VAL_ERROR;
        }      
        LeaveCriticalSection(&client->file_stream[i].lock);
    }  
    return RET_VAL_SUCCESS; 
}
static int file_get_available_stream_channel(ClientData *client){
    // Iterate through all possible file stream slots allocated for this client.
    // MAX_CLIENT_FILE_STREAMS defines the maximum number of concurrent file transfers a single client can handle.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Check if the current file stream slot is not busy.
        // A 'busy' status of FALSE indicates that this slot is currently free and available.
        if(client->file_stream[i].busy == FALSE){
            // If an available slot is found, immediately mark it as busy.
            // This claims the slot for the current operation and prevents other threads
            // from simultaneously claiming the same slot.
            client->file_stream[i].busy = TRUE;
            // Release the critical section lock for this file stream, as its state has been updated.
            LeaveCriticalSection(&client->file_stream[i].lock);
            // Return the index of the newly claimed (and now busy) slot.
            return i;
        }
        // If the current file stream slot is busy, release its lock before checking the next slot.
        // It's important to release the lock for a busy slot so other threads can still access it.
        LeaveCriticalSection(&client->file_stream[i].lock);
    }
    // If the loop completes without finding any available (non-busy) file stream slots,
    // it means all slots are currently in use.
    return RET_VAL_ERROR; // Return an error value to indicate no slot was found.
}
static void file_attach_fragment_to_chunk(FileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size){
    // Calculate the index of the 64-bit bitmap entry (which corresponds to a "chunk" of 64 fragments)
    // to which this incoming fragment belongs.
    // fragment_offset / FILE_FRAGMENT_SIZE gives the fragment number.
    // Dividing by 64ULL (FRAGMENTS_PER_CHUNK) then gives the chunk index.
    uint64_t fstream_index = (fragment_offset / FILE_FRAGMENT_SIZE) / FRAGMENTS_PER_CHUNK;

    // Acquire the critical section lock for the specific FileStream.
    // This ensures thread-safe access to the fstream's data members (bitmap, flags, counters, pool_block_file_chunk)
    // while this fragment is being processed and attached.
    EnterCriticalSection(&fstream->lock);

    // Calculate the destination address within the target memory chunk where the fragment data will be copied.
    // fstream->pool_block_file_chunk[fstream_index] gives the base address of the chunk.
    // (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)) calculates the offset *within that chunk*.
    // This accounts for multiple fragments being part of one chunk.
    char *dest = fstream->pool_block_file_chunk[fstream_index] + (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK));
    char *src = fragment_buffer; // The source buffer containing the incoming fragment data.

    // Copy the fragment data from the source buffer to its calculated destination within the chunk's memory block.
    memcpy(dest, src, fragment_size);
    // Mark the flag for this chunk as CHUNK_BODY.
    // This assumes that by default, chunks are considered 'body' chunks unless specifically identified as 'trailing'.
    fstream->flag[fstream_index] = CHUNK_BODY;
    // fprintf(stdout, "Attaching fragment with offset: %llu to chunk: %llu at offset: %llu\n", fragment_offset, fstream_index, (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)));
    // Increment the total number of bytes received for this file stream.
    fstream->bytes_received += fragment_size;

    // Check if this file stream is expected to have a trailing (potentially partial) chunk.
    if(fstream->trailing_chunk == TRUE){
        // Special handling for the last chunk, which might be the trailing chunk.
        // Check if the current fragment belongs to the last bitmap entry (chunk).
        if(fstream_index == fstream->bitmap_entries_count - 1ULL){
            // If it's the last chunk, accumulate the size of the received fragments for this chunk.
            fstream->trailing_chunk_size += fragment_size;
            // Mark the flag for this last chunk specifically as CHUNK_TRAILING.
            // This flag differentiates it from regular CHUNK_BODY entries for writing logic.
            fstream->flag[fstream_index] = CHUNK_TRAILING;
            // Note: The `CHUNK_BODY` set above will be immediately overwritten if this condition is true.
            //fprintf(stdout, "Receiving last chunk bytes: %llu\n", entry->last_chunk_size); // Original comment used 'entry', should be 'fstream'
        }
        // Check if all expected bytes for the entire file have been received.
        if(fstream->bytes_received == fstream->f_size){
            // If all bytes are received, mark the trailing chunk as complete.
            // This signals that the last chunk is fully assembled (even if partial in size).
            fstream->trailing_chunk_complete = TRUE;
            fprintf(stdout, "\nReceived all bytes. Last chunk size is: %llu\n", fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    // This function manipulates the bits within the uint64_t bitmap entry.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    LeaveCriticalSection(&fstream->lock);

}
static int file_stream_init(FileStream *fstream, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size, ServerIOManager* io_manager) {

    // Acquire stream channel lock
    EnterCriticalSection(&fstream->lock);

    // Initialize stream channel context data
    fstream->busy = TRUE;
    fstream->file_complete = FALSE;
    fstream->stream_err = 0;

    fstream->s_id = session_id;
    fstream->f_id = file_id;
    fstream->f_size = file_size;
    fstream->bytes_received = 0;
    fstream->bytes_written = 0;

    // Calculate total fragments
    fstream->fragment_count = (fstream->f_size + (uint64_t)FILE_FRAGMENT_SIZE - 1ULL) / (uint64_t)FILE_FRAGMENT_SIZE;
    fprintf(stdout, "Fragments count: %llu\n", fstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    fstream->bitmap_entries_count = (fstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;
    fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", fstream->bitmap_entries_count);

 
    //the file size needs to be a multiple of FILE_FRAGMENT_SIZE and nr of fragments needs to be a multiple of 64 due to bitmap entries being 64 bits
    //otherwise the trailing bitmap entry will not be full of fragments (the mask will not be ~0ULL) and this needs to be treated separately 

    // Determine if there's a trailing chunk
    // A trailing chunk exists if:
    // 1. The total file size is not a perfect multiple of FILE_FRAGMENT_SIZE
    // OR
    // 2. The total number of fragments is not a perfect multiple of FRAGMENTS_PER_CHUNK (64)
    fstream->trailing_chunk = ((fstream->f_size % FILE_FRAGMENT_SIZE) != 0) || (((fstream->f_size / FILE_FRAGMENT_SIZE) % 64ULL) != 0);

    // Initialize trailing chunk status
    fstream->trailing_chunk_complete = FALSE;
    fstream->trailing_chunk_size = 0;

    // Allocate memory for bitmap
    fstream->bitmap = malloc(fstream->bitmap_entries_count * sizeof(uint64_t));
    if(fstream->bitmap == NULL){
        fstream->stream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "Memory allocation fail for file bitmap mem!!!\n");
        goto exit_error;
    }
    memset(fstream->bitmap, 0, fstream->bitmap_entries_count * sizeof(uint64_t));

    // Allocate memory for flags
    fstream->flag = malloc(fstream->bitmap_entries_count * sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->stream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        goto exit_error;
    }
    memset(fstream->flag, CHUNK_NONE, fstream->bitmap_entries_count * sizeof(uint8_t));

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = malloc(fstream->bitmap_entries_count * sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->stream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        goto exit_error;
    }   
    memset(fstream->pool_block_file_chunk, 0, fstream->bitmap_entries_count * sizeof(char*));

    // Construct file name and open file
    // Using snprintf for buffer overflow safety
    int snprintf_res = snprintf(fstream->fn, PATH_SIZE, "E:\\test_file_SID_%d_UID_%d.txt", session_id, file_id);
    if (snprintf_res < 0 || snprintf_res >= PATH_SIZE) {
        fprintf(stderr, "Error: File name construction failed or buffer too small.\n");
        fstream->stream_err = STREAM_ERR_FILENAME; // Suggest adding this error code
        goto exit_error;
    }
    //creating output file
    fstream->fp = fopen(fstream->fn, "wb+"); // "wb+" allows writing and reading, creates or truncates
    if(fstream->fp == NULL){
        fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fn, errno);
        fstream->stream_err = STREAM_ERR_FP;
        goto exit_error;
    }

    // Success path
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    //Call file_cleanup_stream to free any partially allocated resources
    file_cleanup_stream(fstream, io_manager);
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_ERROR;

}




// Process received file metadata frame
int handle_file_metadata(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager) {
    // Check if the provided client context is NULL.
    // Receiving a frame for a non-existent client is a critical error,
    // as all operations on the frame depend on a valid client session.
    if(client == NULL){
        // Log an error message indicating that the frame was received for an unknown client.
        fprintf(stdout, "Received frame for non existing client context!\n");
        // Jump to the `exit_error` label to handle cleanup and return an error status.
        goto exit_error;
    }

    // Enter a critical section. This mutex protects the `client` data structure
    EnterCriticalSection(&client->lock);

    // Update the `last_activity_time` for the client.
    client->last_activity_time = time(NULL);

    uint32_t recv_session_id = ntohl(frame->header.session_id);
    uint32_t recv_file_id = ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = ntohll(frame->payload.file_metadata.file_size);


    if(file_check_metadata(client, frame, recv_session_id, recv_file_id) == RET_VAL_ERROR){
        fprintf(stderr, "Duplicated file metadata received!\n");
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        // If the metadata is invalid or duplicated, register an acknowledgment (ACK)
        goto exit_error;
    }

    // Check if the specific file transfer (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This check prevents redundant processing or re-acknowledgment of already finished transfers.
    if(search_uid_hash_table(io_manager->uid_hash_table, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        // If the transfer is already complete, register an acknowledgment (ACK)
        // with `STS_TRANSFER_COMPLETE` status. This informs the sender that
        // the file is already received and no further retransmissions of metadata are needed.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the `exit_error` label, as no further processing is needed for this file metadata.
        goto exit_error;
    }

    // Attempt to get an available slot within the client's file stream management structure.
    // This is typically used when a client can handle multiple concurrent file transfers.
    int slot = file_get_available_stream_channel(client);
    // If `file_get_available_stream_channel` returns `RET_VAL_ERROR`, it means no free slot is available.
    if(slot == RET_VAL_ERROR){
        // Log an error message indicating that the maximum number of concurrent file transfers has been reached.
        fprintf(stderr, "Maximum file transfers reached!\n");
        // Register an ACK with `ERR_RESOURCE_LIMIT` status. This informs the sender
        // that the server is currently unable to accept more file transfers due to resource constraints.
        // `queue_priority_seq_num` is used for high-priority control messages, such as error notifications.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_RESOURCE_LIMIT);
        // Jump to the `exit_error` label to clean up and return.
        goto exit_error;
    }

    // Log the details of the successfully received file metadata to standard output.
    // This provides valuable debugging and monitoring information.
    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, File Size: %llu, Fragment Size: %d\n", recv_session_id, recv_file_id, recv_file_size, FILE_FRAGMENT_SIZE);

    // Initialize the file stream for the new incoming file transfer in the determined `slot`.
    // This function would typically handle opening a file on disk, setting up buffer management,
    // and initializing state for tracking received fragments.
    if(file_stream_init(&client->file_stream[slot], recv_session_id, recv_file_id, recv_file_size, io_manager) == RET_VAL_ERROR){
        // If file stream initialization fails, jump to the error handling block.
        goto exit_error;
    }

    // Add an entry to the unique ID hash table for this new file transfer.
    // It's marked with `UID_WAITING_FRAGMENTS` to indicate that the metadata has been received
    // and the server is now awaiting the actual file data fragments.
    add_uid_hash_table(io_manager->uid_hash_table, recv_session_id, recv_file_id, UID_WAITING_FRAGMENTS);

    // Register a general acknowledgment (ACK) to be sent back to the client,
    // confirming successful receipt and processing of the file metadata frame.
    // `queue_seq_num` is typically for data-related ACKs, but here used for a positive response.
    register_ack(&io_manager->queue_seq_num, client, frame, STS_ACK);

    // Leave the critical section. This releases the lock on the `client` data structure,
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_SUCCESS` to indicate that the file metadata was handled successfully.
    return RET_VAL_SUCCESS;

exit_error:
    // This label serves as a unified exit point for all error conditions within the function.
    // It ensures that the critical section is always exited, preventing potential deadlocks
    // if an error occurs before the normal `LeaveCriticalSection` call.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_ERROR` to signal that an error occurred during the processing of file metadata.
    return RET_VAL_ERROR;
}
// Process received file fragment frame
int handle_file_fragment(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager){
    // Check if the client context is NULL. This is a fundamental check, as all
    // fragment handling logic depends on a valid client session.
    if(client == NULL){
        // Log an informational message indicating a frame was received for an unknown client.
        fprintf(stdout, "Received frame for non existing client context!\n");
        // Jump to the error handling block to release any potential resources and return.
        goto exit_error;
    }

    // Enter a critical section to protect the `client` data structure.
    EnterCriticalSection(&client->lock);

    // Update the client's `last_activity_time`. This timestamp is used for
    // session management and timeout detection, indicating recent communication.
    client->last_activity_time = time(NULL);

    // Extract necessary information from the UDP frame's header and payload.
    // `ntohl` and `ntohll` convert values from network byte order to host byte order
    // to ensure correct interpretation across different system architectures.
    uint32_t recv_session_id = ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload
    uint64_t recv_fragment_offset = ntohll(frame->payload.file_fragment.offset); // Fragment offset
    uint32_t recv_fragment_size = ntohl(frame->payload.file_fragment.size); // Fragment size

    // Check if this file (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This prevents re-processing or re-acknowledging fragments for already completed transfers.
    if(search_uid_hash_table(io_manager->uid_hash_table, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        // If the transfer is complete, register an ACK with `STS_TRANSFER_COMPLETE` status.
        // This signals to the sender that the file is fully received and no more fragments are needed.
        // Use `queue_priority_seq_num` for high-priority control messages, such as transfer
        register_ack(&io_manager->queue_priority_seq_num, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the error handling block to release the lock and exit.
        goto exit_error;
    }

    // Attempt to match the incoming file fragment to an existing file transfer session
    // within the client's context. This function typically searches through `client->file_stream` array.
    int slot = file_match_fragment(client, frame, recv_session_id, recv_file_id);
    // If `file_match_fragment` returns `RET_VAL_ERROR`, it means no active file transfer
    // corresponding to the received file ID and session ID was found. This usually implies
    // that the metadata for this file was not received or processed correctly.
    if(slot == RET_VAL_ERROR){
        // Log an error indicating an unknown file ID or missing metadata for the received fragment.
        fprintf(stderr, "Received frame with unknown file ID: %u Session ID %u - missing metadata\n", recv_file_id, client->session_id);
        // Register an ACK with `ERR_INVALID_FILE_ID` status, informing the sender of the issue.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_INVALID_FILE_ID);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Additional check to ensure the file ID in the selected slot matches the received file ID.
    // This acts as a double-check against `file_match_fragment` and potential race conditions.
    if (client->file_stream[slot].f_id != recv_file_id){
        // This scenario should ideally be caught by `file_match_fragment`, but serves as a safeguard.
        // It indicates an inconsistency or a frame for a file not properly set up in the slot.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_INVALID_FILE_ID);
        fprintf(stderr, "No file transfer in progress (no file buffer allocated) for file ID %u !!!\n", recv_file_id);
        goto exit_error;
    }

    // Check if this specific fragment (identified by its offset) has already been received.
    // This uses a bitmap or similar mechanism to track received fragments and detect duplicates.
    if(check_fragment_received(client->file_stream[slot].bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        // If it's a duplicate, register an ACK with `ERR_DUPLICATE_FRAME` status.
        // This informs the sender not to retransmit this specific fragment.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        // Log a message indicating the receipt of a duplicate fragment.
        fprintf(stderr, "Received duplicate frame (sesion ID: %u, file ID: %u, offset: %llu)!!!\n", recv_session_id, recv_file_id, recv_fragment_offset);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Validate if the received fragment's offset is within the bounds of the expected file size.
    // A fragment starting beyond the file size indicates a malformed or erroneous frame.
    if(recv_fragment_offset >= client->file_stream[slot].f_size){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message providing details about the out-of-bounds offset.
        fprintf(stderr, "Received fragment with offset out of limits. File size: %llu, Received offset: %llu\n", client->file_stream[slot].f_size, recv_fragment_offset);
        // Jump to the error handling block.
        goto exit_error;
    }
    // Validate if the fragment, given its offset and size, extends beyond the file's total size.
    // This catches fragments that start within bounds but are too large for the remaining file.
    if (recv_fragment_offset + recv_fragment_size > client->file_stream[slot].f_size){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message indicating that the fragment extends past the file boundaries.
        fprintf(stderr, "Fragment extends past file bounds (file ID: %u, offset: %llu, size: %u). File size: %llu\n",
                recv_file_id, recv_fragment_offset, recv_fragment_size, client->file_stream[slot].f_size);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Calculate the index of the memory chunk entry in the bitmap based on the fragment offset.
    // Each entry in the bitmap likely corresponds to a larger memory block (chunk) that holds multiple fragments.
    uint64_t entry_index = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL; // Assuming 64 fragments per bitmap entry

    // If the corresponding bitmap entry is 0, it means no memory chunk has been allocated for this section of the file yet.
    if(client->file_stream[slot].bitmap[entry_index] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        client->file_stream[slot].pool_block_file_chunk[entry_index] = pool_alloc(&io_manager->pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(client->file_stream[slot].pool_block_file_chunk[entry_index] == NULL){
            // If the memory pool is full and allocation fails, log a message (commented out in original).
            // This suggests a resource limitation that might need to be addressed.
            // fprintf(stdout, "Pool is full! Waiting for free block!\n");
            // Jump to the error handling block, as the fragment cannot be stored.
            goto exit_error;
        }
    }

    // Attach the received fragment's data to the appropriate location within the allocated memory chunk.
    // This function typically involves copying the fragment bytes and updating the fragment's status in the bitmap.
    file_attach_fragment_to_chunk(&client->file_stream[slot], frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size);

    // Register a general acknowledgment (ACK) to be sent back to the client,
    // confirming successful receipt and processing of this file fragment.
    // `queue_seq_num` is typically for data-related ACKs.
    register_ack(&io_manager->queue_seq_num, client, frame, STS_ACK);

    // Leave the critical section, releasing the client's lock.
    // This must be done before returning from the function to prevent deadlocks.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_SUCCESS` to indicate that the file fragment was handled successfully.
    return RET_VAL_SUCCESS;

exit_error:
    // This label serves as a common exit point for all error conditions within the function.
    // It ensures that the critical section is always exited, regardless of where an error occurred,
    // thereby preventing deadlocks and resource leaks.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_ERROR` to signal that an error occurred during the processing of the file fragment.
    return RET_VAL_ERROR;
}
// HANDLE received message fragment frame
int handle_message_fragment(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager){

    int slot; // Declares an integer variable 'slot' to store the index of the message handling slot.
    if(client == NULL){ // Checks if the 'client' pointer is NULL, indicating an invalid or non-existent client context.
        fprintf(stdout, "Received frame for non existing client context!\n"); // Prints an informational message to standard output.
        goto exit_error; // Jumps to the 'exit_error' label for centralized error handling and cleanup.
    }

    EnterCriticalSection(&client->lock); // Acquires a critical section lock associated with the 'client' object. This ensures thread-safe access to 'client' data.

    client->last_activity_time = time(NULL); // Updates the 'last_activity_time' field of the client, typically used for session timeout management.

    // Extracts and converts message-specific fields from the network byte order to host byte order.
    // 'ntohl' converts a 32-bit unsigned integer from network byte order to host byte order.
    uint32_t recv_session_id = ntohl(frame->header.session_id);
    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    // Guard against fragments for already completed messages.
    // Checks if the message, identified by its ID and the client's session ID, has already been marked as fully received in the unique ID hash table.
    if(search_uid_hash_table(io_manager->uid_hash_table, recv_session_id, recv_message_id, UID_RECV_COMPLETE) == TRUE){
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
        if (msg_check_completion_and_record(&client->msg_stream[slot], io_manager) == RET_VAL_ERROR)
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
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
        if (msg_validate_fragment(client, slot, frame, io_manager) == RET_VAL_ERROR)
            goto exit_error; // If validation fails, jumps to the 'exit_error' label.
        // Initializes the message receiving slot with the message ID and its total expected length.
        if (msg_init_stream(&client->msg_stream[slot], recv_session_id, recv_message_id, recv_message_len) == RET_VAL_ERROR)
            goto exit_error; // If slot initialization fails, jumps to the 'exit_error' label.
        // Attaches the first fragment's data to the newly initialized message buffer.
        
        msg_attach_fragment(&client->msg_stream[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        
        // Adds an entry to the unique ID hash table, marking the message as awaiting further fragments.
        add_uid_hash_table(io_manager->uid_hash_table, recv_session_id, recv_message_id, UID_WAITING_FRAGMENTS);
        // Acknowledges the successful receipt of this initial fragment.
        register_ack(&io_manager->queue_seq_num, client, frame, STS_ACK);
        // Checks if the message is now complete (e.g., if it was a single-fragment message) and finalizes it.
        if (msg_check_completion_and_record(&client->msg_stream[slot], io_manager) == RET_VAL_ERROR)
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
        LeaveCriticalSection(&client->lock); // Releases the critical section lock.
        return RET_VAL_SUCCESS; // Returns success, indicating the message fragment was handled.
    }

exit_error: // Label for centralized error handling.
    LeaveCriticalSection(&client->lock); // Ensures the critical section lock is released before exiting, preventing deadlocks.
    return RET_VAL_ERROR; // Returns an error status.
}


