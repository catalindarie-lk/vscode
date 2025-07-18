
#include <stdio.h>
#include <stdint.h>
#include <ws2tcpip.h>

#include "include/protocol_frames.h"
#include "include/checksum.h"
#include "include/netendians.h"

int send_frame(const UdpFrame *frame, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                ){
    // Determine the actual size to send based on frame type if payloads are variable
    size_t frame_size = 0;
    switch (frame->header.frame_type) {
        case FRAME_TYPE_FILE_METADATA:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataPayload);
            break;
        case FRAME_TYPE_FILE_METADATA_RESPONSE:
            frame_size = sizeof(FrameHeader) + sizeof(FileMetadataResponsePayload);
            break;
        case FRAME_TYPE_FILE_FRAGMENT:
            frame_size = sizeof(FrameHeader) + sizeof(FileFragmentPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_END:
            frame_size = sizeof(FrameHeader) + sizeof(FileEndPayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_FILE_COMPLETE:
            frame_size = sizeof(FrameHeader) + sizeof(FileCompletePayload); // Or header + payload_len + related metadata
            break;
        case FRAME_TYPE_LONG_TEXT_MESSAGE:
            frame_size = sizeof(FrameHeader) + sizeof(LongTextPayload); // Or header + payload_len + related metadata
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
    return bytes_sent;
}


int send_disconnect(const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                ){
    UdpFrame frame;
    
    memset(&frame, 0, sizeof(frame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_DISCONNECT;
    frame.header.seq_num = _htonll(FRAME_TYPE_DISCONNECT_SEQ);
    frame.header.session_id = _htonl(session_id); // Use the session ID provided
    // Calculate CRC32 for the ACK/NACK frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader)));
    
    uint32_t bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_disconnect() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}






