
#include <stdio.h>
#include <string.h>
#include <stdint.h>
//#include <winsock2.h>
#include <ws2tcpip.h>
#include <time.h>
#include <process.h>    // For _beginthreadex
#include <windows.h>    // For Windows-specific functions like CreateThread, Sleep

#include "include/protocol_frames.h"
#include "include/checksum.h"
#include "include/netendians.h"
#include "include/queue.h"
#include "include/hash.h"
#include "include/mem_pool.h"
#include "include/client.h"
#include "include/server.h"

int send_frame(const UdpFrame *frame, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
                ){
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = 0;
    switch (frame->header.frame_type) {
        case FRAME_TYPE_FILE_METADATA:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_FRAGMENT:
            frame_size = sizeof(FrameHeader) + sizeof(FileFragmentPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_ACK:
            frame_size = sizeof(FrameHeader) + sizeof(AckPayload); // Acknowledgment frame
            break;
        case FRAME_TYPE_CONNECT_REQUEST:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectRequestPayload);
            break;
        case FRAME_TYPE_CONNECT_RESPONSE:
            frame_size = sizeof(FrameHeader) + sizeof(ConnectResponsePayload);
            break;
        case FRAME_TYPE_DISCONNECT:
            frame_size = sizeof(FrameHeader);
            break;
        case FRAME_TYPE_KEEP_ALIVE:
            frame_size = sizeof(FrameHeader);
            break;
        default:
            frame_size = sizeof(UdpFrame); // Fallback to max size
            break;
    }

    int bytes_sent = sendto(src_socket, (const char*)frame, frame_size, 0, (SOCKADDR*)dest_addr, sizeof(*dest_addr));
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "sendto() failed with error: %d\n", WSAGetLastError());
        return SOCKET_ERROR;        
    }
    frame_counters->total_sent++;
    return bytes_sent;
}

int send_ack(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
                ){
    UdpFrame ack_frame;
    //initialize frame
    memset(&ack_frame, 0, sizeof(ack_frame));
    // Set the header fields
    ack_frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    ack_frame.header.frame_type = FRAME_TYPE_ACK;
    ack_frame.header.seq_num = _htonll(seq_num);
    ack_frame.header.session_id = _htonl(session_id); // Use the session ID provided
    ack_frame.payload.ack.op_code = op_code;
    // Calculate CRC32 for the ACK/NACK frame
    ack_frame.header.checksum = _htonl(calculate_crc32(&ack_frame, sizeof(FrameHeader) + sizeof(AckPayload)));
    
    int bytes_sent = send_frame(&ack_frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ack() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_disconnect(const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
                ){
    UdpFrame frame;
    
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_DISCONNECT;
    frame.header.seq_num = UINT64_MAX;
    frame.header.session_id = _htonl(session_id); // Use the session ID provided
    // Calculate CRC32 for the ACK/NACK frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_disconnect() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_connect_response(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
                ) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id); // Use client's session ID

    frame.payload.response.session_timeout = _htonl(session_timeout);
    frame.payload.response.server_status = status;

    snprintf(frame.payload.response.server_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, server_name);

    // Calculate CRC32 for the ACK frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));

    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "send_connect_respose() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_connect_request(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t client_id, 
                    const uint32_t flag, 
                    const char *client_name, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
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
    frame.payload.request.client_id = _htonl(client_id);
    frame.payload.request.flag = flag;

    snprintf(frame.payload.request.client_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, client_name);

    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(ConnectRequestPayload)));
    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_connect_request() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent; 
}

int send_keep_alive(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr, 
                    FrameCounters* frame_counters
                ){
    UdpFrame frame;

    // Initialize the ACK/NACK frame
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_KEEP_ALIVE;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id); // Use the session ID provided  
    // Calculate CRC32 for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ping_pong() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}



int send_file_metadata(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager, 
                            FrameCounters* frame_counters
                        ){

    UdpFrame frame;

    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);
       
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));  
    
    // if(insert_frame(io_manager->frame_ht, &io_manager->frame_ht_mutex, &frame, &io_manager->frame_ht_count, &io_manager->frame_mem_pool) == RET_VAL_ERROR){
    //     fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
    //     return RET_VAL_ERROR;
    // }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
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
                            ClientIOManager* io_manager,  
                            FrameCounters* frame_counters
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
    if(insert_frame(io_manager->frame_ht, &io_manager->frame_ht_mutex, &frame, &io_manager->frame_ht_count, &io_manager->frame_mem_pool) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }
    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_path);
    #endif
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
                            ClientIOManager* io_manager, 
                            FrameCounters* frame_counters
                        ){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "Invalid text parsed!.\n");
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
    frame.payload.long_text_msg.message_id = _htonl(message_id);
    frame.payload.long_text_msg.message_len = _htonl(message_len);
    frame.payload.long_text_msg.fragment_len = _htonl(fragment_len);
    frame.payload.long_text_msg.fragment_offset = _htonl(fragment_offset);
    
    memcpy(frame.payload.long_text_msg.fragment_text, fragment_buffer, fragment_len);
    
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  
    if(insert_frame(io_manager->frame_ht, &io_manager->frame_ht_mutex, &frame, &io_manager->frame_ht_count, &io_manager->frame_mem_pool) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }
    int bytes_sent = send_frame(&frame, src_socket, dest_addr, frame_counters);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}


