
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


ServerFileStream *get_free_file_stream(ServerBuffers *buffers){
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

ServerFileStream *search_file_stream(ServerBuffers *buffers, const uint32_t session_id, const uint32_t file_id){
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




// Original recursive directory creation logic (slightly refined for this context)
// This function takes a FULL, ABSOLUTE path and ensures all its parent directories exist.
static bool CreateAbsoluteFolderRecursive(const char *absolutePathToCreate) {
    char tempPath[MAX_PATH];
    char *p;
    DWORD dwError;

    // Make a copy of the path as strtok_s modifies the string.
    // Ensure it's safe and null-terminated.
    if (strncpy_s(tempPath, sizeof(tempPath), absolutePathToCreate, sizeof(tempPath) - 1) != 0) {
        fprintf(stderr, "Error: Failed to copy path '%s' for recursive creation.\n", absolutePathToCreate);
        return FALSE;
    }
    tempPath[sizeof(tempPath) - 1] = '\0'; // Ensure null termination

    // Handle potential trailing backslash for consistent tokenizing, but only if not root (e.g., "C:\")
    size_t path_len = strlen(tempPath);
    if (path_len > 0 && (tempPath[path_len - 1] == '\\' || tempPath[path_len - 1] == '/')) {
        // If it's just "C:\" or "C:/", don't remove the backslash.
        // Otherwise, remove it.
        if (path_len > 3 || (path_len == 3 && tempPath[1] != ':')) { // Heuristic for non-root paths like "C:\"
             tempPath[path_len - 1] = '\0';
        }
    }

    // Iterate through the path components
    char currentPath[MAX_PATH] = {0};
    char *next_token;
    char *token = strtok_s(tempPath, "\\/", &next_token); // Use both \ and / as delimiters

    // Special handling for drive letters (e.g., "C:", "D:")
    if (token != NULL && strlen(token) == 2 && token[1] == ':') {
        // This is a drive letter, copy it as the first part of currentPath
        if (strcpy_s(currentPath, sizeof(currentPath), token) != 0) {
            fprintf(stderr, "Error: Failed to copy drive letter '%s'.\n", token);
            return FALSE;
        }
        token = strtok_s(NULL, "\\/", &next_token); // Move to the next token
    } else if (token != NULL && (token[0] == '\\' || token[0] == '/') && strlen(token) == 1) {
        // This handles UNC paths like "\\server\share" by ignoring initial empty token
        // if path started with \\ or //, but only if it's not a single backslash
        // for relative path from current directory.
        // For simplicity with drive letters, we assume absolute paths here for this logic.
    }


    while (token != NULL) {
        // Append current token to currentPath with a backslash
        if (strlen(currentPath) > 0) { // Add backslash if not the very first component
            if (strcat_s(currentPath, sizeof(currentPath), "\\") != 0) {
                fprintf(stderr, "Error: Failed to append backslash to '%s'.\n", currentPath);
                return FALSE;
            }
        }
        if (strcat_s(currentPath, sizeof(currentPath), token) != 0) {
            fprintf(stderr, "Error: Failed to append token '%s' to '%s'.\n", token, currentPath);
            return FALSE;
        }

        // Try to create the directory
        if (!CreateDirectoryA(currentPath, NULL)) {
            dwError = GetLastError();
            if (dwError != ERROR_ALREADY_EXISTS) {
                fprintf(stderr, "Failed to create directory '%s'. Error code: %lu\n", currentPath, dwError);
                return FALSE;
            }
        }
        token = strtok_s(NULL, "\\/", &next_token);
    }
    // printf("Folder structure '%s' created or already exists.\n", absolutePathToCreate);
    return TRUE;
}


// New wrapper function to create a relative path within a given root
// rootDirectory: The full absolute path of the starting root directory (e.g., "C:\MyLogs")
// relativePath: The path to create relative to the root (e.g., "AppErrors\2025\July")
bool CreateRelativeFolderRecursive(const char *rootDirectory, const char *relativePath) {
    char fullPathToCreate[MAX_PATH];
    int result;

    if (rootDirectory == NULL || relativePath == NULL) {
        fprintf(stderr, "Error: rootDirectory or relativePath is NULL.\n");
        return FALSE;
    }

    // Determine if rootDirectory already has a trailing backslash
    size_t root_len = strlen(rootDirectory);
    if (root_len == 0) {
        fprintf(stderr, "Error: rootDirectory is empty.\n");
        return FALSE;
    }

    // Construct the full absolute path
    if (root_len + strlen(relativePath) + 2 > MAX_PATH) { // +1 for potential backslash, +1 for null terminator
        fprintf(stderr, "Error: Combined path '%s\\%s' exceeds MAX_PATH (%d).\n", rootDirectory, relativePath, MAX_PATH);
        return FALSE;
    }

    // Copy root directory
    result = strcpy_s(fullPathToCreate, sizeof(fullPathToCreate), rootDirectory);
    if (result != 0) {
        fprintf(stderr, "Error: Failed to copy rootDirectory '%s'. errno: %d\n", rootDirectory, errno);
        return FALSE;
    }

    // Append a backslash if rootDirectory doesn't already have one and it's not a bare drive letter (e.g., "C:")
    if (fullPathToCreate[root_len - 1] != '\\' && fullPathToCreate[root_len - 1] != '/') {
         // Special handling for drive roots like "C:" - ensure it becomes "C:\"
        if (!(root_len == 2 && fullPathToCreate[1] == ':')) {
            result = strcat_s(fullPathToCreate, sizeof(fullPathToCreate), "\\");
            if (result != 0) {
                fprintf(stderr, "Error: Failed to append backslash to rootDirectory. errno: %d\n", errno);
                return FALSE;
            }
        }
    }

    // Append relative path
    result = strcat_s(fullPathToCreate, sizeof(fullPathToCreate), relativePath);
    if (result != 0) {
        fprintf(stderr, "Error: Failed to append relativePath '%s'. errno: %d\n", relativePath, errno);
        return FALSE;
    }

    // Now call the core recursive creation function with the combined path
    //printf("Attempting to create full path: '%s'\n", fullPathToCreate);
    return CreateAbsoluteFolderRecursive(fullPathToCreate);
}



/**
 * @brief Normalizes a Windows-style file path by:
 * 1. Replacing consecutive backslashes with a single one.
 * 2. Handling trailing backslashes for consistency (adds one for drive roots,
 * removes for others unless specified).
 * 3. Handles UNC paths (\\server\share) correctly by preserving leading \\.
 *
 * @param path The input path string (will be modified in place).
 * @param add_trailing_backslash If true, ensures a single trailing backslash for directories
 * (unless it's a file path). If false, removes trailing backslash
 * unless it's a drive root (e.g., "C:\").
 * @return True on success, False if the path is too long after normalization or input is invalid.
 */
bool normalize_paths(char *path, bool add_trailing_backslash) {
    if (path == NULL || strlen(path) == 0) {
        // Empty or NULL path is considered an invalid input for normalization
        fprintf(stderr, "Error: Input path is NULL or empty.\n");
        return false;
    }

    char normalized_path[MAX_PATH];
    int write_idx = 0;
    int read_idx = 0;
    bool is_unc_path = false;

    // Handle leading double backslashes for UNC paths (e.g., \\server\share)
    if (path[0] == '\\' && path[1] == '\\') {
        normalized_path[write_idx++] = '\\';
        normalized_path[write_idx++] = '\\';
        read_idx = 2;
        is_unc_path = true;
    } else if (path[0] == '/' && path[1] == '/') { // Also support forward slashes for UNC-like paths
        normalized_path[write_idx++] = '\\'; // Normalize to backslash for Windows
        normalized_path[write_idx++] = '\\';
        read_idx = 2;
        is_unc_path = true;
    }

    // Handle drive letter root (e.g., "C:")
    // This is distinct from UNC paths or relative paths
    if (!is_unc_path && strlen(path) >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
        normalized_path[write_idx++] = path[0];
        normalized_path[write_idx++] = ':';
        // If there's no backslash after the drive letter, add one to read_idx if needed
        if (strlen(path) > 2 && (path[2] == '\\' || path[2] == '/')) {
            read_idx = 3; // Skip C:\ or C:/
        } else {
            // Path is just "C:", we'll add the '\' later if needed
            read_idx = 2;
        }
    }


    // Process the rest of the path
    bool last_char_was_separator = false;
    for (; read_idx < strlen(path); ++read_idx) {
        if (path[read_idx] == '\\' || path[read_idx] == '/') {
            if (!last_char_was_separator) { // Only add one separator if multiple found
                if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow during normalization.\n"); return false; }
                normalized_path[write_idx++] = '\\'; // Normalize all to backslashes
            }
            last_char_was_separator = true;
        } else {
            if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow during normalization.\n"); return false; }
            normalized_path[write_idx++] = path[read_idx];
            last_char_was_separator = false;
        }
    }

    // Handle trailing backslashes
    if (write_idx > 0) {
        // If the path ends with a separator and it's not a bare drive root (e.g., "C:\")
        // and it's not a bare UNC root (e.g., "\\server\share")
        bool is_drive_root = (write_idx == 3 && normalized_path[1] == ':' && normalized_path[2] == '\\');
        bool is_unc_root = (is_unc_path && write_idx == 2); // after \\

        // Remove trailing backslash if not desired and not a drive root or UNC root
        if (!add_trailing_backslash && normalized_path[write_idx - 1] == '\\' && !is_drive_root && !is_unc_root) {
            write_idx--;
        }
        // Add trailing backslash if desired and not already present, and not ending with a drive letter (e.g., "C:")
        else if (add_trailing_backslash && normalized_path[write_idx - 1] != '\\') {
            // If the path ends with a drive letter (like "C:"), add a backslash to make it "C:\"
            if (!(write_idx == 2 && normalized_path[1] == ':')) {
                if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow when adding trailing backslash.\n"); return false; }
                normalized_path[write_idx++] = '\\';
            }
        }
    }

    // Null-terminate the normalized path
    if (write_idx >= MAX_PATH) { // Final check after possible trailing backslash addition
        fprintf(stderr, "Error: Path buffer overflow after normalization and termination.\n");
        return false;
    }
    normalized_path[write_idx] = '\0';

    // Copy the normalized path back to the original buffer
    if (strcpy_s(path, MAX_PATH, normalized_path) != 0) {
        fprintf(stderr, "Error: Failed to copy normalized path back to original buffer.\n");
        return false;
    }

    return true;
}



static int init_file_stream(ServerFileStream *fstream, UdpFrame *frame, ServerBuffers* buffers) {

    EnterCriticalSection(&fstream->lock);

    uint64_t recv_seq_num = _ntohll(frame->header.seq_num);
    fstream->sid = _ntohl(frame->header.session_id);
    fstream->fid = _ntohl(frame->payload.file_metadata.file_id);
    fstream->fsize = _ntohll(frame->payload.file_metadata.file_size);
    fstream->rpath_len = _ntohl(frame->payload.file_metadata.rpath_len);
    memcpy(fstream->rpath, frame->payload.file_metadata.rpath, fstream->rpath_len);
    fstream->fname_len = _ntohl(frame->payload.file_metadata.fname_len);
    memcpy(fstream->fname, frame->payload.file_metadata.fname, fstream->fname_len);

    fstream->fstream_err = STREAM_ERR_NONE;

    // fstream->sid = session_id;
    // fstream->fid = file_id;
    // fstream->fsize = file_size;

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

    if (CreateRelativeFolderRecursive(rootFolder, fstream->rpath) == false) {
        fprintf(stderr, "Failed to create recursive path for root folder: \"%s\", relative path \"%s\". Error code: %lu\n", rootFolder, fstream->rpath, GetLastError());
        goto exit_error;
    }

    snprintf(fstream->fpath, MAX_PATH, "%s%s%s", rootFolder, fstream->rpath, fstream->fname);
    normalize_paths(fstream->fpath, false);
    
    fprintf(stdout, "Received file: %s\n", fstream->fpath);

    fstream->fp = fopen(fstream->fpath, "wb+"); // "wb+" allows writing and reading, creates or truncates
    if(fstream->fp == NULL){
        fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fname, errno);
        fstream->fstream_err = STREAM_ERR_FP;
        goto exit_error;
    }

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
        fprintf(stderr, "Received metadata frame Seq: %llu for old completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
        op_code = ERR_DUPLICATE_FRAME;
        goto exit_err;
    }

    if(ht_search_id(&buffers->ht_fid, recv_session_id, recv_file_id, ID_RECV_COMPLETE) == TRUE){
        fprintf(stderr, "Received metadata frame Seq: %llu for old completed fID: %u sID %u\n", recv_seq_num, recv_file_id, recv_session_id);
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

