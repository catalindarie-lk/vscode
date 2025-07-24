
#include <stdint.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "include/protocol_frames.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/client.h"

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
                            const char *file_name,
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        ){

    UdpFrame frame;
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));

    if(file_name == NULL){
        fprintf(stderr, "ERROR: Invalid file name pointer (NULL).\n");
        return RET_VAL_ERROR;
    }
    uint32_t file_name_len = (uint32_t)strnlen(file_name, MAX_PATH - 1);
 
    if(file_name_len == 0){
        fprintf(stderr, "ERROR: File name is 0 length.\n");
        return RET_VAL_ERROR;
    }
    if(file_name_len >= MAX_PATH - 1){
        fprintf(stderr, "ERROR: File name is too long. Max length is %d characters.\n", MAX_PATH - 1);
        return RET_VAL_ERROR;
    }
    file_name_len += 1; // Add 1 for null terminator

    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);
    snprintf(frame.payload.file_metadata.filename, file_name_len, "%s", file_name);
    // sha256_final(&sha256_ctx, frame.payload.file_metadata.file_hash);
           
    // Calculate the checksum for the frame
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

    fprintf(stdout, "PAYLOAD: %s\n", frame.payload.text_fragment.chars);
    
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
