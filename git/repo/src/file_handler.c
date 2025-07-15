
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
#include "include/server_frames.h"
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
        if(client->fstream[i].fstream_busy == TRUE && client->fstream[i].sid == session_id && client->fstream[i].fid == file_id){
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
        if(client->fstream[i].fstream_busy == FALSE){
            // If an available slot is found, immediately mark it as busy.
            // This claims the slot for the current operation and prevents other threads
            // from simultaneously claiming the same slot.
            client->fstream[i].fstream_busy = TRUE;
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
static void file_attach_fragment_to_chunk(FileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size, ServerBuffers* buffers){

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
    fstream->recv_bytes_count += fragment_size;

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
        if(fstream->recv_bytes_count == fstream->fsize){
            // If all bytes are received, mark the trailing chunk as complete.
            // This signals that the last chunk is fully assembled (even if partial in size).
            fstream->trailing_chunk_complete = TRUE;

            //update_uid_status_hash_table(buffers->uid_hash_table, &buffers->uid_ht_mutex, fstream->sid, fstream->fid, UID_RECV_COMPLETE);
            fprintf(stdout, "Received complete file: %s, Size: %llu, Last chunk size: %llu\n", fstream->fn, fstream->recv_bytes_count, fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    LeaveCriticalSection(&fstream->lock);
    return;

}
static int file_stream_init(FileStream *fstream, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size, ServerBuffers* buffers) {

    // Acquire stream channel lock
    EnterCriticalSection(&fstream->lock);

    // Initialize stream channel context data
    //file_cleanup_stream(fstream, buffers);
    fstream->fstream_busy = TRUE;
    fstream->fstream_err = STREAM_ERR_NONE;
    fstream->sid = session_id;
    fstream->fid = file_id;
    fstream->fsize = file_size;

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
        fstream->fstream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "Memory allocation fail for file bitmap mem!!!\n");
        goto exit_error;
    }
    //memset(fstream->bitmap, 0, fstream->bitmap_entries_count * sizeof(uint64_t));

    // Allocate memory for flags
    fstream->flag = calloc(fstream->bitmap_entries_count, sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->fstream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        goto exit_error;
    }
    //memset(fstream->flag, CHUNK_NONE, fstream->bitmap_entries_count * sizeof(uint8_t));

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = calloc(fstream->bitmap_entries_count, sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->fstream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        goto exit_error;
    }
    //memset(fstream->pool_block_file_chunk, 0, fstream->bitmap_entries_count * sizeof(char*));

    // Construct file name and open file
    // Using snprintf for buffer overflow safety
    int snprintf_res = snprintf(fstream->fn, PATH_SIZE, "D:\\E\\test_file_SID_%d_UID_%d.txt", session_id, file_id);
    if (snprintf_res < 0 || snprintf_res >= PATH_SIZE) {
        fprintf(stderr, "Error: File name construction failed or buffer too small.\n");
        fstream->fstream_err = STREAM_ERR_FILENAME; // Suggest adding this error code
        goto exit_error;
    }
    //creating output file
    fstream->fp = fopen(fstream->fn, "wb+"); // "wb+" allows writing and reading, creates or truncates
    if(fstream->fp == NULL){
        fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fn, errno);
        fstream->fstream_err = STREAM_ERR_FP;
        goto exit_error;
    }

    SetEvent(fstream->hevent_recv_file);
    // Success path
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    //Call file_cleanup_stream to free any partially allocated resources
    file_cleanup_stream(fstream, buffers);
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_ERROR;

}



// Process received file metadata frame
int handle_file_metadata(Client *client, UdpFrame *frame, ServerBuffers* buffers) {

    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    EnterCriticalSection(&client->lock);

    QueueAckEntry ack_entry;
    uint8_t err;

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);

    if(file_check_metadata(client, frame, recv_session_id, recv_file_id) == RET_VAL_ERROR){
        fprintf(stderr, "Duplicated file metadata received!\n");
        err = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(search_uid_hash_table(buffers->uid_hash_table, &buffers->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for old completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        err = ERR_EXISTING_FILE;
        goto exit_err;
    }

    int slot = file_get_available_stream_channel(client);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "All file transfers streams are in use!\n");
        err = ERR_RESOURCE_LIMIT;
        goto exit_err;
    }

    fprintf(stdout, "Received metadata Seq: %llu; sID: %d; fID: %d; fsize: %llu;\n", recv_seq_num, recv_session_id, recv_file_id, recv_file_size);
    fprintf(stdout, "Opened file stream: %u\n", slot);

    if(file_stream_init(&client->fstream[slot], recv_session_id, recv_file_id, recv_file_size, buffers) == RET_VAL_ERROR){
        fprintf(stderr, "Error initializing file stream\n");
        err = ERR_TRANSFER_INIT;
        goto exit_err;
    }

    add_uid_hash_table(buffers->uid_hash_table, &buffers->uid_ht_mutex, recv_session_id, recv_file_id, UID_WAITING_FRAGMENTS);

    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_CONFIRM_FILE_METADATA, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    
    LeaveCriticalSection(&client->lock);
    return RET_VAL_SUCCESS;

exit_err:
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, err, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;
}
// Process received file fragment frame
int handle_file_fragment(Client *client, UdpFrame *frame, ServerBuffers* buffers){

    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    EnterCriticalSection(&client->lock);

    QueueAckEntry ack_entry;
    uint8_t err;

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    if(search_uid_hash_table(buffers->uid_hash_table, &buffers->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        err = ERR_EXISTING_FILE;
        goto exit_err;
    }

    int slot = file_match_frame(client, recv_session_id, recv_file_id);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        err = ERR_MISSING_METADATA;
        goto exit_err;
    }

    if(check_fragment_received(client->fstream[slot].bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        fprintf(stderr, "Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        err = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(recv_fragment_offset >= client->fstream[slot].fsize){
       fprintf(stderr, "Fragment offset past limits - offset: %llu, fragment size: %u, file size: %llu\n",
                                        recv_fragment_offset, recv_fragment_size, client->fstream[slot].fsize);
        err = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if (recv_fragment_offset + recv_fragment_size > client->fstream[slot].fsize){
        fprintf(stderr, "Fragment extends past file bounds - offset: %llu, fragment size: %u, file size: %llu\n",
                                        recv_fragment_offset, recv_fragment_size, client->fstream[slot].fsize);
        err = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    uint64_t i = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL; // Assuming 64 fragments per bitmap entry
    if(client->fstream[slot].bitmap[i] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        client->fstream[slot].pool_block_file_chunk[i] = pool_alloc(&buffers->pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(client->fstream[slot].pool_block_file_chunk[i] == NULL){
            fprintf(stderr, "Error allocating memory chunk for sID: %u; fID: %u;\n", recv_session_id, recv_file_id);
            return RET_VAL_ERROR;
        }
    }

    file_attach_fragment_to_chunk(&client->fstream[slot], frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size, buffers);

    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_ACK, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_ack, &ack_entry);

    LeaveCriticalSection(&client->lock);
    return RET_VAL_SUCCESS;

exit_err:
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, err, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;

}
// Process received file end frame
int handle_file_end(Client *client, UdpFrame *frame, ServerBuffers* buffers){

    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        return RET_VAL_ERROR;
    }

    QueueAckEntry ack_entry;
    uint8_t err;

    EnterCriticalSection(&client->lock);

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload

    if(search_uid_hash_table(buffers->uid_hash_table, &buffers->uid_ht_mutex, recv_session_id, recv_file_id, UID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received file end frame for completed file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        err = ERR_EXISTING_FILE;
        goto exit_err;
    }

    int slot = file_match_frame(client, recv_session_id, recv_file_id);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Received end frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        err = ERR_MISSING_METADATA;
        goto exit_err;
    }

    FileStream *fstream = &client->fstream[slot];
    for(int i = 0; i < 32; i++){   
        fstream->received_sha256[i] = frame->payload.file_end.file_hash[i];
    }
    fstream->file_hash_received = TRUE;
    fstream->file_end_frame_seq_num = recv_seq_num;

    if(fstream->file_complete){
        new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_CONFIRM_FILE_END, client->srv_socket, &client->client_addr);
        push_ack(&buffers->queue_priority_ack, &ack_entry);
    }
 
    LeaveCriticalSection(&client->lock);
    return RET_VAL_SUCCESS;

exit_err:
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, err, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;
}

