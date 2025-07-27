
#include <stdint.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "include/protocol_frames.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/client.h"

// Custom strnlen (for platforms that might not have it or if you use a custom one)
size_t s_strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len] != '\0') {
        len++;
    }
    return len;
}

int send_connect_request(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t client_id, 
                    const uint32_t flags, 
                    const char *client_name, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                ){
    // Create a connect request frame
    UdpFrame frame;
    // Initialize the connect request frame    
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_REQUEST;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    frame.payload.connection_request.client_id = _htonl(client_id);
    frame.payload.connection_request.flags = flags;

    snprintf(frame.payload.connection_request.client_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, client_name);

    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent; 
}

int send_keep_alive(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                ){
    UdpFrame frame;

    // Initialize the frame
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_KEEP_ALIVE;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id); // Use the session ID provided  
    // Calculate CRC32 for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_keep_alive() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_file_metadata(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *rpath,
                            const uint32_t rpath_len,
                            const char *fname,
                            const uint32_t fname_len,
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        ){

    UdpFrame frame;
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));

 
  // --- Validate rpath ---
    if(rpath == NULL){
        fprintf(stderr, "ERROR: send_file_metadata - Invalid relative file path pointer (NULL).\n");
        return RET_VAL_ERROR;
    }

    // NEW: rpath must be at least 1 character (for '\')
    if(rpath_len == 0){ // If declared length is 0
        fprintf(stderr, "ERROR: send_file_metadata - Relative file path has a declared length of 0. Minimum is 1 (for '\\').\n");
        return RET_VAL_ERROR;
    }
    if (strlen(rpath) == 0) { // If actual string content is empty
        fprintf(stderr, "ERROR: send_file_metadata - Relative file path content is empty. Minimum is 1 character (for '\\').\n");
        return RET_VAL_ERROR;
    }

    // Check against the size of the field in UdpFrame payload.
    // rpath_len is *content* length, and receiver expects `content_len < MAX_PATH`.
    // So, `rpath_len >= sizeof(frame.payload.file_metadata.rpath)` means it won't fit WITH a null terminator.
    if(rpath_len >= sizeof(frame.payload.file_metadata.rpath)){
        fprintf(stderr, "ERROR: send_file_metadata - Relative file path content (length %u) is too long for payload buffer (max %zu chars).\n",
                rpath_len, sizeof(frame.payload.file_metadata.rpath) - 1);
        return RET_VAL_ERROR;
    }
    // Consistency check: declared length vs. actual string length
    if (rpath_len != strlen(rpath)) {
        fprintf(stderr, "ERROR: send_file_metadata - Declared rpath_len (%u) does not match actual strlen(rpath) (%zu).\n",
                rpath_len, strlen(rpath));
        return RET_VAL_ERROR;
    }


    // --- Validate fname ---
    if(fname == NULL){
        fprintf(stderr, "ERROR: send_file_metadata - Invalid filename pointer (NULL).\n");
        return RET_VAL_ERROR;
    }
    // Filename should typically never be 0 length
    if(fname_len == 0){
        fprintf(stderr, "ERROR: send_file_metadata - Filename has a declared length of 0.\n");
        return RET_VAL_ERROR;
    }

    if(fname_len >= sizeof(frame.payload.file_metadata.fname)){
        fprintf(stderr, "ERROR: send_file_metadata - Filename content (length %u) is too long for payload buffer (max %zu chars).\n",
                fname_len, sizeof(frame.payload.file_metadata.fname) - 1);
        return RET_VAL_ERROR;
    }
    // Consistency check: declared length vs. actual string length
    if (fname_len != strlen(fname)) {
        fprintf(stderr, "ERROR: send_file_metadata - Declared fname_len (%u) does not match actual strlen(fname) (%zu).\n",
                fname_len, strlen(fname));
        return RET_VAL_ERROR;
    }



    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);


   // --- Copy rpath to payload buffer ---
    // At this point, we've validated:
    // 1. rpath is not NULL.
    // 2. rpath_len matches strlen(rpath).
    // 3. rpath_len is strictly less than sizeof(buffer), so it will fit and be null-terminated.
    // So, a direct copy with _snprintf_s is now safe and guaranteed not to truncate.
    _snprintf_s(frame.payload.file_metadata.rpath, sizeof(frame.payload.file_metadata.rpath),
                rpath_len, // Use rpath_len as the max count for _snprintf_s. This ensures we copy exactly rpath_len characters.
                "%s", rpath);
    frame.payload.file_metadata.rpath_len = _htonl(rpath_len); // Send the exact content length


    // --- Copy fname to payload buffer ---
    // Similar validations apply.
    _snprintf_s(frame.payload.file_metadata.fname, sizeof(frame.payload.file_metadata.fname),
                fname_len, // Use fname_len as the max count for _snprintf_s.
                "%s", fname);
    frame.payload.file_metadata.fname_len = _htonl(fname_len); // Send the exact content length


    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));
    
    if(ht_insert_frame(&buffers->ht_frame, &frame) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_file_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        ){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "\nInvalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_FRAGMENT;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_fragment.file_id = _htonl(file_id);
    frame.payload.file_fragment.size = _htonl(fragment_size);
    frame.payload.file_fragment.offset = _htonll(fragment_offset);
    memcpy(frame.payload.file_fragment.bytes, fragment_buffer, fragment_size);
    
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileFragmentPayload)));  

    if(ht_insert_frame(&buffers->ht_frame, &frame) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }


    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_file_end(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const char *file_hash,
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        ){

    UdpFrame frame;
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));

    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_END;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);

    memcpy(frame.payload.file_end.file_hash, file_hash, 32);
           
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileEndPayload)));
    
    if(ht_insert_frame(&buffers->ht_frame, &frame) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_long_text_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t message_id, 
                            const uint32_t message_len, 
                            const uint32_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_len, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr, 
                            ClientBuffers* buffers
                        ){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "Invalid text pointer parsed!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_LONG_TEXT_MESSAGE;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.text_fragment.message_id = _htonl(message_id);
    frame.payload.text_fragment.message_len = _htonl(message_len);
    frame.payload.text_fragment.fragment_len = _htonl(fragment_len);
    frame.payload.text_fragment.fragment_offset = _htonl(fragment_offset);
    
    memcpy(frame.payload.text_fragment.chars, fragment_buffer, fragment_len);
   
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  

    if(ht_insert_frame(&buffers->ht_frame, &frame) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is full, failed to allocate!\n");
        return RET_VAL_ERROR;
    }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}
