
#include <stdint.h>
#include <stdio.h>
#include <ws2tcpip.h>
#include "include/protocol_frames.h"
#include "include/checksum.h"
#include "include/netendians.h"         // For network byte order conversions
#include "include/mem_pool.h"


int send_connect_response(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr,
                    MemPool *mem_pool
                ) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_CONNECT_RESPONSE;

    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id); // Use client's session ID

    frame.payload.connection_response.session_timeout = _htonl(session_timeout);
    frame.payload.connection_response.server_status = status;

    snprintf(frame.payload.connection_response.server_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, server_name);

    // Calculate CRC32 for the ACK frame
    frame.header.checksum = _htonl(calculate_crc32_table(&frame, sizeof(FrameHeader) + sizeof(ConnectResponsePayload)));

    int bytes_sent = send_frame(&frame, src_socket, dest_addr, mem_pool);
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "send_connect_respose() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_file_metadata_response(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t file_id, 
                    const uint8_t op_code,
                    SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr,
                    MemPool *mem_pool
                ) {
    UdpFrame frame;
    // Initialize the response frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA_RESPONSE;

    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id); // Use client's session ID

    frame.payload.file_metadata_response.file_id = _htonl(file_id);
    frame.payload.file_metadata_response.op_code = op_code;

    // Calculate CRC32 for the ACK frame
    frame.header.checksum = _htonl(calculate_crc32_table(&frame, sizeof(FrameHeader) + sizeof(FileMetadataResponsePayload)));

    int bytes_sent = send_frame(&frame, src_socket, dest_addr, mem_pool);
    if (bytes_sent == SOCKET_ERROR) {
        fprintf(stderr, "send_file_metadata_respose() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}

int send_ack(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr,
                    MemPool *mem_pool
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
    ack_frame.header.checksum = _htonl(calculate_crc32_table(&ack_frame, sizeof(FrameHeader) + sizeof(AckPayload)));
    
    int bytes_sent = send_frame(&ack_frame, src_socket, dest_addr, mem_pool);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_ack() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}
