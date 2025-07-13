
#include <stdint.h>
#include <stdio.h>
#include <time.h>
//#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "include/file_handler.h"
#include "include/protocol_frames.h"
#include "include/netendians.h"
#include "include/queue.h"
#include "include/hash.h"
#include "include/bitmap.h"
#include "include/checksum.h"
#include "include/mem_pool.h"
#include "include/fileio.h"
#include "include/server.h"

// Handle file fragment helper functions
static int file_match_frame(Client *client, const uint32_t session_id, const uint32_t file_id){
    // Iterate through all possible file stream slots associated with this client.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        // This prevents other threads from modifying or accessing this specific file stream's data
        // (like its busy status, session ID, or file ID) while this function is checking it.
        EnterCriticalSection(&client->fstream[i].lock);
        // Check if the current file stream slot matches the incoming fragment:
        // 1. client->file_stream[i].busy == TRUE: Ensure the slot is currently active and in use for a transfer.
        // 2. client->file_stream[i].sid == session_id: Match the session ID from the incoming fragment.
        // 3. client->file_stream[i].fid == file_id: Match the file ID from the incoming fragment.
        if(client->fstream[i].busy == TRUE && client->fstream[i].sid == session_id && client->fstream[i].fid == file_id){
            // If a matching file stream slot is found, release its lock.
            LeaveCriticalSection(&client->fstream[i].lock);
            // Return the index of the matching slot.
            return i;
        }
        // If the current file stream slot does not match the fragment, release its lock
        // before proceeding to check the next slot. This is crucial to avoid deadlocks.
        LeaveCriticalSection(&client->fstream[i].lock);
    }
    // If the loop completes without finding any file stream slot that matches the session_id and file_id,
    // it means no active transfer for this client corresponds to the incoming fragment.
    return RET_VAL_ERROR; // Return an error value to indicate no match was found.
}
static int file_check_metadata(Client *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){
    // This function checks if a file stream with the given session_id and file_id already exists for the client. 
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){      
        EnterCriticalSection(&client->fstream[i].lock);       
        if(client->fstream[i].sid == session_id && client->fstream[i].fid == file_id){          
            LeaveCriticalSection(&client->fstream[i].lock);          
            return RET_VAL_ERROR;
        }      
        LeaveCriticalSection(&client->fstream[i].lock);
    }  
    return RET_VAL_SUCCESS; 
}
static int file_get_available_stream_channel(Client *client){
    
    // Iterate through all possible file stream slots allocated for this client.
    // MAX_CLIENT_FILE_STREAMS defines the maximum number of concurrent file transfers a single client can handle.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        EnterCriticalSection(&client->fstream[i].lock);
        // Check if the current file stream slot is not busy.
        // A 'busy' status of FALSE indicates that this slot is currently free and available.
        if(client->fstream[i].busy == FALSE){
            // If an available slot is found, immediately mark it as busy.
            // This claims the slot for the current operation and prevents other threads
            // from simultaneously claiming the same slot.
            client->fstream[i].busy = TRUE;
            // Release the critical section lock for this file stream, as its state has been updated.
            LeaveCriticalSection(&client->fstream[i].lock);
            // Return the index of the newly claimed (and now busy) slot.
            return i;
        }
        // If the current file stream slot is busy, release its lock before checking the next slot.
        // It's important to release the lock for a busy slot so other threads can still access it.
        LeaveCriticalSection(&client->fstream[i].lock);
    }
    // If the loop completes without finding any available (non-busy) file stream slots,
    // it means all slots are currently in use.
    return RET_VAL_ERROR; // Return an error value to indicate no slot was found.
}
static void file_attach_fragment_to_chunk(FileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size, ServerIOManager* io_manager){

    // Acquire the critical section lock for the specific FileStream.
    EnterCriticalSection(&fstream->lock);

    // Calculate the index of the 64-bit bitmap entry (which corresponds to a "chunk" of 64 fragments)
    // to which this incoming fragment belongs.
    // fragment_offset / FILE_FRAGMENT_SIZE gives the fragment number.
    // Dividing by 64ULL (FRAGMENTS_PER_CHUNK) then gives the chunk index.
    uint64_t fstream_index = (fragment_offset / FILE_FRAGMENT_SIZE) / FRAGMENTS_PER_CHUNK;

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
            //fprintf(stdout, "Receiving last chunk bytes: %llu\n", fstream->trailing_chunk_size);
        }
        // Check if all expected bytes for the entire file have been received.
        if(fstream->bytes_received == fstream->fsize){
            // If all bytes are received, mark the trailing chunk as complete.
            // This signals that the last chunk is fully assembled (even if partial in size).
            fstream->trailing_chunk_complete = TRUE;

            update_uid_status_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, fstream->sid, fstream->fid, UID_RECV_COMPLETE);
            fprintf(stdout, "Received complete file: %s, Size: %llu, Last chunk size: %llu\n", fstream->fn, fstream->bytes_received, fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    LeaveCriticalSection(&fstream->lock);
    return;

}
static int file_stream_init(FileStream *fstream, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size, ServerIOManager* io_manager) {

    // Acquire stream channel lock
    EnterCriticalSection(&fstream->lock);

    // Initialize stream channel context data
    fstream->busy = TRUE;

    fstream->file_hash_received = FALSE;
    fstream->file_hash_calculated = FALSE;
    fstream->file_hash_validated = FALSE;
    fstream->file_complete = FALSE;

    fstream->stream_err = STREAM_ERR_NONE;
    fstream->sid = session_id;
    fstream->fid = file_id;
    fstream->fsize = file_size;
    fstream->bytes_received = 0;
    fstream->bytes_written = 0;
    fstream->chunks_hashed = 0;
    memset(fstream->calculated_sha256, 0, 32);
    sha256_init(&fstream->sha256_ctx);

    // Calculate total fragments
    fstream->fragment_count = (fstream->fsize + (uint64_t)FILE_FRAGMENT_SIZE - 1ULL) / (uint64_t)FILE_FRAGMENT_SIZE;
    //fprintf(stdout, "Fragments count: %llu\n", fstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    fstream->bitmap_entries_count = (fstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;
    //fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", fstream->bitmap_entries_count);
 
    //the file size needs to be a multiple of FILE_FRAGMENT_SIZE and nr of fragments needs to be a multiple of 64 due to bitmap entries being 64 bits
    //otherwise the trailing bitmap entry will not be full of fragments (the mask will not be ~0ULL) and this needs to be treated separately 

    // Determine if there's a trailing chunk
    // A trailing chunk exists if:
    // 1. The total file size is not a perfect multiple of FILE_FRAGMENT_SIZE
    // OR
    // 2. The total number of fragments is not a perfect multiple of FRAGMENTS_PER_CHUNK (64)
    fstream->trailing_chunk = ((fstream->fsize % FILE_FRAGMENT_SIZE) != 0) || (((fstream->fsize / FILE_FRAGMENT_SIZE) % 64ULL) != 0);

    // Initialize trailing chunk status
    fstream->trailing_chunk_complete = FALSE;
    fstream->trailing_chunk_size = 0;

    // Allocate memory for bitmap
    fstream->bitmap = calloc(fstream->bitmap_entries_count, sizeof(uint64_t));
    if(fstream->bitmap == NULL){
        fstream->stream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "Memory allocation fail for file bitmap mem!!!\n");
        goto exit_error;
    }
    //memset(fstream->bitmap, 0, fstream->bitmap_entries_count * sizeof(uint64_t));

    // Allocate memory for flags
    fstream->flag = calloc(fstream->bitmap_entries_count, sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->stream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        goto exit_error;
    }
    //memset(fstream->flag, CHUNK_NONE, fstream->bitmap_entries_count * sizeof(uint8_t));

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = calloc(fstream->bitmap_entries_count, sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->stream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        goto exit_error;
    }
    //memset(fstream->pool_block_file_chunk, 0, fstream->bitmap_entries_count * sizeof(char*));

    // Construct file name and open file
    // Using snprintf for buffer overflow safety
    int snprintf_res = snprintf(fstream->fn, PATH_SIZE, "D:\\E\\test_file_SID_%d_UID_%d.txt", session_id, file_id);
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

    SetEvent(fstream->hevent_recv_file);
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
int handle_file_metadata(Client *client, UdpFrame *frame, ServerIOManager* io_manager) {
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

    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);


    if(file_check_metadata(client, frame, recv_session_id, recv_file_id) == RET_VAL_ERROR){
        fprintf(stderr, "Duplicated file metadata received!\n");
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        // If the metadata is invalid or duplicated, register an acknowledgment (ACK)
        goto exit_error;
    }

    // Check if the specific file transfer (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This check prevents redundant processing or re-acknowledgment of already finished transfers.
    if(search_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
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
    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, Size: %llu\n", recv_session_id, recv_file_id, recv_file_size);
    fprintf(stdout, "Opened file stream: %u\n", slot);
    // Initialize the file stream for the new incoming file transfer in the determined `slot`.
    // This function would typically handle opening a file on disk, setting up buffer management,
    // and initializing state for tracking received fragments.
    if(file_stream_init(&client->fstream[slot], recv_session_id, recv_file_id, recv_file_size, io_manager) == RET_VAL_ERROR){
        // If file stream initialization fails, jump to the error handling block.
        goto exit_error;
    }

    // Add an entry to the unique ID hash table for this new file transfer.
    // It's marked with `UID_WAITING_FRAGMENTS` to indicate that the metadata has been received
    // and the server is now awaiting the actual file data fragments.
    add_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_file_id, UID_WAITING_FRAGMENTS);

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
int handle_file_fragment(Client *client, UdpFrame *frame, ServerIOManager* io_manager){
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
    // `_ntohl` and `_ntohll` convert values from network byte order to host byte order
    // to ensure correct interpretation across different system architectures.
    uint32_t recv_session_id = _ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset); // Fragment offset
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size); // Fragment size

    // Check if this file (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This prevents re-processing or re-acknowledging fragments for already completed transfers.
    if(search_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received frame for completed file ID: %u Session ID %u - missing metadata\n", recv_file_id, recv_session_id);
        register_ack(&io_manager->queue_priority_seq_num, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the error handling block to release the lock and exit.
        goto exit_error;
    }

    int slot = file_match_frame(client, recv_session_id, recv_file_id);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Received frame with unknown file ID: %u Session ID %u - missing metadata\n", recv_file_id, recv_session_id);
        // Register an ACK with `ERR_INVALID_FILE_ID` status, informing the sender of the issue.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_INVALID_FILE_ID);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Check if this specific fragment (identified by its offset) has already been received.
    if(check_fragment_received(client->fstream[slot].bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        // If it's a duplicate, register an ACK with `ERR_DUPLICATE_FRAME` status.
        fprintf(stderr, "Received duplicate file fragment - Sesion ID: %u, File ID: %u, Offset: %llu\n", recv_session_id, recv_file_id, recv_fragment_offset);
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_DUPLICATE_FRAME);
        goto exit_error;
    }

    // Validate if the received fragment's offset is within the bounds of the expected file size.
    // A fragment starting beyond the file size indicates a malformed or erroneous frame.
    if(recv_fragment_offset >= client->fstream[slot].fsize){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message providing details about the out-of-bounds offset.
        fprintf(stderr, "Received fragment with offset out of limits. File size: %llu, Received offset: %llu\n", client->fstream[slot].fsize, recv_fragment_offset);
        // Jump to the error handling block.
        goto exit_error;
    }
    // Validate if the fragment, given its offset and size, extends beyond the file's total size.
    // This catches fragments that start within bounds but are too large for the remaining file.
    if (recv_fragment_offset + recv_fragment_size > client->fstream[slot].fsize){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message indicating that the fragment extends past the file boundaries.
        fprintf(stderr, "Fragment extends past file bounds - file ID: %u, offset: %llu, fragment size: %u, file size: %llu\n",
                recv_file_id, recv_fragment_offset, recv_fragment_size, client->fstream[slot].fsize);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Calculate the index of the memory chunk entry in the bitmap based on the fragment offset.
    // Each entry in the bitmap likely corresponds to a larger memory block (chunk) that holds multiple fragments.
    uint64_t entry_index = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL; // Assuming 64 fragments per bitmap entry

    // If the corresponding bitmap entry is 0, it means no memory chunk has been allocated for this section of the file yet.
    if(client->fstream[slot].bitmap[entry_index] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        client->fstream[slot].pool_block_file_chunk[entry_index] = pool_alloc(&io_manager->pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(client->fstream[slot].pool_block_file_chunk[entry_index] == NULL){
            fprintf(stderr, "Error allocating memory chunk for file ID: %u, offset: %llu\n", recv_file_id, recv_fragment_offset);
            goto exit_error;
        }
    }

    // Attach the received fragment's data to the appropriate location within the allocated memory chunk.
    // This function typically involves copying the fragment bytes and updating the fragment's status in the bitmap.
    file_attach_fragment_to_chunk(&client->fstream[slot], frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size, io_manager);

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
// Process received file end frame
int handle_file_end(Client *client, UdpFrame *frame, ServerIOManager* io_manager){
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
    // `_ntohl` and `_ntohll` convert values from network byte order to host byte order
    // to ensure correct interpretation across different system architectures.
    uint32_t recv_session_id = _ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload

    // Check if this file (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This prevents re-processing or re-acknowledging fragments for already completed transfers.
    if(search_uid_hash_table(io_manager->uid_hash_table, &io_manager->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received frame for completed file ID: %u Session ID %u - file end frame\n", recv_file_id, recv_session_id);
        register_ack(&io_manager->queue_priority_seq_num, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the error handling block to release the lock and exit.
        goto exit_error;
    }

    int slot = file_match_frame(client, recv_session_id, recv_file_id);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Received frame with unknown file ID: %u Session ID %u - file end frame\n", recv_file_id, recv_session_id);
        // Register an ACK with `ERR_INVALID_FILE_ID` status, informing the sender of the issue.
        register_ack(&io_manager->queue_priority_seq_num, client, frame, ERR_INVALID_FILE_ID);
        // Jump to the error handling block.
        goto exit_error;
    }

    FileStream *fstream = &client->fstream[slot];
    for(int i = 0; i < 32; i++){   
        fstream->received_sha256[i] = frame->payload.file_end.file_hash[i];
    }    
    fstream->file_hash_received = TRUE;

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

