
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
#include "include/folders.h"


static ServerFileStream *get_free_file_stream(ServerBuffers *buffers){
    EnterCriticalSection(&buffers->fstreams_lock);
    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &buffers->fstream[i];
        if(!fstream->fstream_busy){
            fstream->fstream_busy = TRUE;
            LeaveCriticalSection(&buffers->fstreams_lock);
            return fstream;
        }
    }
    LeaveCriticalSection(&buffers->fstreams_lock);
    return NULL;
}
static ServerFileStream *search_file_stream(ServerBuffers *buffers, const uint32_t session_id, const uint32_t file_id){
    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &buffers->fstream[i];
        // EnterCriticalSection(&fstream->lock);
        if(fstream->fstream_busy == TRUE && fstream->sid == session_id && fstream->fid == file_id){
            // LeaveCriticalSection(&fstream->lock);
            return fstream;
        }
        // LeaveCriticalSection(&fstream->lock);
    }
    return NULL;
}
static void file_attach_fragment_to_chunk(ServerFileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size, ServerBuffers* buffers){

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
        }
        // Check if all expected bytes for the entire file have been received.
        if(fstream->recv_bytes_count == fstream->fsize){
            fstream->trailing_chunk_complete = TRUE;
            // fprintf(stdout, "Received complete file: %s, Size: %llu, Last chunk size: %llu\n", fstream->fname, fstream->recv_bytes_count, fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    LeaveCriticalSection(&fstream->lock);
    return;

}
static int init_file_stream(ServerFileStream *fstream, UdpFrame *frame, ServerBuffers* buffers) {

    EnterCriticalSection(&fstream->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    fstream->sid = _ntohl(frame->header.session_id);
    fstream->fid = _ntohl(frame->payload.file_metadata.file_id);
    fstream->fsize = _ntohll(frame->payload.file_metadata.file_size);

    // --- Proper string copy and validation for rpath ---
    uint32_t received_rpath_len = _ntohl(frame->payload.file_metadata.rpath_len);

    // Validate the received length against the destination buffer's capacity (MAX_PATH - 1 for content + null)
    if (received_rpath_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_file_stream - Received rpath length (%u) is too large for buffer (max %d).\n",
                received_rpath_len, MAX_PATH - 1);
        fstream->fstream_err = STREAM_ERR_MALFORMED_DATA; // Define a new error for this
        fstream->rpath_len = 0;
        fstream->rpath[0] = '\0'; // Ensure it's null-terminated even on error
        goto exit_error; // Exit function on critical error
    } else {
        // Use snprintf with precision to copy exactly 'received_rpath_len' characters.
        // snprintf will null-terminate the buffer as long as `received_rpath_len < MAX_PATH`.
        int result = snprintf(fstream->rpath, sizeof(fstream->rpath),
                              "%.*s", (int)received_rpath_len, frame->payload.file_metadata.rpath);

        // Verify snprintf's return value. It should equal the number of characters copied.
        if (result < 0 || (size_t)result != received_rpath_len) {
            fprintf(stderr, "ERROR: init_file_stream - Failed to copy rpath: snprintf returned %d, expected %u.\n",
                    result, received_rpath_len);
            fstream->fstream_err = STREAM_ERR_INTERNAL_COPY; // Define a new error
            fstream->rpath_len = 0;
            fstream->rpath[0] = '\0';
            goto exit_error;
        } else {
            // Copy successful, store the actual content length
            fstream->rpath_len = received_rpath_len;
        }
    }

    // --- Proper string copy and validation for fname ---
    uint32_t received_fname_len = _ntohl(frame->payload.file_metadata.fname_len);

    if (received_fname_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: init_file_stream - Received fname length (%u) is too large for buffer (max %d).\n",
                received_fname_len, MAX_PATH - 1);
        fstream->fstream_err = STREAM_ERR_MALFORMED_DATA;
        fstream->fname_len = 0;
        fstream->fname[0] = '\0';
        goto exit_error;
    } else {
        int result = snprintf(fstream->fname, sizeof(fstream->fname),
                              "%.*s", (int)received_fname_len, frame->payload.file_metadata.fname);

        if (result < 0 || (size_t)result != received_fname_len) {
            fprintf(stderr, "ERROR: init_file_stream - Failed to copy fname: snprintf returned %d, expected %u.\n",
                    result, received_fname_len);
            fstream->fstream_err = STREAM_ERR_INTERNAL_COPY;
            fstream->fname_len = 0;
            fstream->fname[0] = '\0';
            goto exit_error;
        } else {
            fstream->fname_len = received_fname_len;
        }
    }
    // --- End of string copy and validation ---

    fstream->fstream_err = STREAM_ERR_NONE;

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

    // Allocate memory for flags
    fstream->flag = calloc(fstream->bitmap_entries_count, sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->fstream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        goto exit_error;
    }

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = calloc(fstream->bitmap_entries_count, sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->fstream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        goto exit_error;
    }

    // creating root folder (based on session ID of the client)
    char rootFolder[MAX_PATH];
    snprintf(rootFolder, MAX_PATH, "%s""Client_SID_%d", DEST_FPATH, fstream->sid);

    if (CreateDirectory(rootFolder, NULL)) {
        // printf("Root folder created successfully.\n");
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
            // printf("Folder already exists.\n");
        } else {
            printf("Failed to create root folder \"%s\". Error code: %lu\n", rootFolder, error);
            goto exit_error;
        }
    }

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == FALSE) {
        fprintf(stderr, "Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        goto exit_error;
    }

    snprintf(fstream->fpath, MAX_PATH, "%s%s%s", rootFolder, fstream->rpath, fstream->fname);
    NormalizePaths(fstream->fpath, false);
    
    char fpath[MAX_PATH] = {0};

    // fstream->fp = fopen(fstream->fpath, "wb+"); // "wb+" allows writing and reading, creates or truncates
    fstream->fp = FopenRename(fstream->fpath, fpath, MAX_PATH, "wb+");
    if(fstream->fp == NULL){
        fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fname, errno);
        fstream->fstream_err = STREAM_ERR_FP;
        goto exit_error;
    }
    fprintf(stdout, "Received file: %s\n", fpath);

    if(push_fstream(&buffers->queue_fstream, (uintptr_t)fstream) == RET_VAL_ERROR){
        goto exit_error;
    }

    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    //Call close_file_stream to free any partially allocated resources
    close_file_stream(fstream, buffers);
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

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = _ntohll(frame->payload.file_metadata.file_size);

    QueueAckEntry ack_entry = {0};
    uint8_t op_code = 0;

    if(ht_search_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_WAITING_FRAGMENTS) == TRUE){
        fprintf(stderr, "Received duplicated metadata frame Seq: %llu for fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(ht_search_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    ServerFileStream *fstream = get_free_file_stream(buffers);
    if(fstream == NULL){
        // fprintf(stderr, "All new file transfers streams are in use!\n");
        op_code = ERR_RESOURCE_LIMIT;
        goto exit_err; 
    }

    memcpy(&fstream->client_addr, &client->client_addr, sizeof(struct sockaddr_in));

    if(init_file_stream(fstream, frame, buffers) == RET_VAL_ERROR){
        fprintf(stderr, "Error initializing file stream\n");
        op_code = ERR_STREAM_INIT;
        goto exit_err;
    }

    if(ht_insert_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_WAITING_FRAGMENTS) == RET_VAL_ERROR){
        fprintf(stderr, "Failed to allocate memory for file ID in hash table\n");
        op_code = ERR_MEMORY_ALLOCATION;
        goto exit_err;
    }

    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_CONFIRM_FILE_METADATA, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_SUCCESS;

exit_err:
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, op_code, client->srv_socket, &client->client_addr);
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

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id);
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = _ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = _ntohl(frame->payload.file_fragment.size);

    QueueAckEntry ack_entry = {0};
    uint8_t op_code = 0;

    if(ht_search_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received fragment frame Seq: %llu; for old completed fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    ServerFileStream *fstream = search_file_stream(buffers, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received fragment frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MISSING_METADATA;
        goto exit_err;
    }

    if(check_fragment_received(fstream->bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        fprintf(stderr, "Received duplicate file fragment Seq: %llu; fID: %u; sID: %u; \n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(recv_fragment_offset >= fstream->fsize){
       fprintf(stderr, "Fragment offset past limits - offset: %llu, fragment size: %u, file size: %llu\n",
                                        recv_fragment_offset, recv_fragment_size, fstream->fsize);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    if (recv_fragment_offset + recv_fragment_size > fstream->fsize){
        fprintf(stderr, "Fragment extends past file bounds - offset: %llu, fragment size: %u, file size: %llu\n",
                                        recv_fragment_offset, recv_fragment_size, fstream->fsize);
        op_code = ERR_MALFORMED_FRAME;
        goto exit_err;
    }

    uint64_t i = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL; // Assuming 64 fragments per bitmap entry
    if(fstream->bitmap[i] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        fstream->pool_block_file_chunk[i] = pool_alloc(&buffers->pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(fstream->pool_block_file_chunk[i] == NULL){
            fprintf(stderr, "Error allocating memory chunk for sID: %u; fID: %u;\n", recv_session_id, recv_file_id);
            op_code = ERR_RESOURCE_LIMIT;
            goto exit_err;
        }
    }

    file_attach_fragment_to_chunk(fstream, frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size, buffers);

    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_FRAME_DATA_ACK, client->srv_socket, &client->client_addr);
    push_ack(&buffers->fqueue_ack, &ack_entry);

    LeaveCriticalSection(&client->lock);
    ReleaseSemaphore(buffers->test_semaphore, 1, NULL);
    return RET_VAL_SUCCESS;

exit_err:
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, op_code, client->srv_socket, &client->client_addr);
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

    EnterCriticalSection(&client->lock);

    client->last_activity_time = time(NULL);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    uint32_t recv_session_id = _ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = _ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload

    QueueAckEntry ack_entry = {0};
    uint8_t op_code = 0;

    if(ht_search_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received file end frame for completed file Seq: %llu; sID: %u; fID: %u;\n", recv_seq_num, recv_session_id, recv_file_id);
        op_code = ERR_EXISTING_FILE;
        goto exit_err;
    }

    ServerFileStream *fstream = search_file_stream(buffers, recv_session_id, recv_file_id);
    if(!fstream){
        fprintf(stderr, "Received end frame Seq: %llu for unknown fID: %u; sID %u;\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_MISSING_METADATA;
        goto exit_err;
    }

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
    new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, op_code, client->srv_socket, &client->client_addr);
    push_ack(&buffers->queue_priority_ack, &ack_entry);
    LeaveCriticalSection(&client->lock);
    return RET_VAL_ERROR;
}

