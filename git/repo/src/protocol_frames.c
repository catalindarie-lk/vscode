
#include <stdio.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#include "include/protocol_frames.h"
#include "include/checksum.h"
#include "include/netendians.h"
#include "include/mem_pool.h"


// Utility function to initialize an IOCP_CONTEXT
void init_iocp_context(IOCP_CONTEXT *iocp_context, OPERATION_TYPE type) {
    if (!iocp_context) 
        return;
    memset(iocp_context, 0, sizeof(IOCP_CONTEXT));
    iocp_context->overlapped.hEvent = NULL; // Not using event handles for IOCP completions
    iocp_context->wsaBuf.buf = iocp_context->buffer;
    iocp_context->wsaBuf.len = sizeof(UdpFrame);
    iocp_context->addr_len = sizeof(struct sockaddr_in);
    iocp_context->type = type;
    return;
}

int udp_recv_from(const SOCKET socket, IOCP_CONTEXT *iocp_context){

    if (!iocp_context) {
        return RET_VAL_ERROR;
    }

    init_iocp_context(iocp_context, OP_RECV);

    DWORD bytes_recv = 0;
    DWORD flags = 0;

    int recvfrom_result = WSARecvFrom(
        socket,
        &iocp_context->wsaBuf,
        1,
        &bytes_recv,
        &flags,
        (SOCKADDR*)&iocp_context->addr,
        &iocp_context->addr_len,
        &iocp_context->overlapped,
        NULL
    );

    if (recvfrom_result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        int error_code = WSAGetLastError();
        // fprintf(stderr, "WSARecvFrom failed with error: %d\n", error_code);
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}

int udp_send_to(const SOCKET socket, const char *data, size_t data_len, const struct sockaddr_in *dest_addr, MemPool *mem_pool) {
    // Allocate a new iocp_context for each send operation
    IOCP_CONTEXT *iocp_context = (IOCP_CONTEXT*)pool_alloc(mem_pool);
    if (iocp_context == NULL) {
        fprintf(stderr, "Failed to allocate IOCP_CONTEXT for send.\n");
        return RET_VAL_ERROR;
    }
    init_iocp_context(iocp_context, OP_SEND); // Initialize as a send iocp_context

    // Copy data to the iocp_context's buffer
    if (data_len > sizeof(UdpFrame)) {
        fprintf(stderr, "Send data larger than sizeof(UdpFrame).\n");
        free(iocp_context);
        return RET_VAL_ERROR;
    }
    memcpy(iocp_context->buffer, data, data_len);
    iocp_context->wsaBuf.len = (ULONG)data_len; // Set actual data length for send

    // Set destination address
    memcpy(&iocp_context->addr, dest_addr, sizeof(struct sockaddr_in));
    iocp_context->addr_len = sizeof(struct sockaddr_in);

    DWORD bytes_sent = 0;
    int result = WSASendTo(
        socket,
        &iocp_context->wsaBuf,
        1,
        &bytes_sent,
        0, // Flags
        (SOCKADDR*)&iocp_context->addr,
        iocp_context->addr_len,
        &iocp_context->overlapped,
        NULL
    );

    if (result == SOCKET_ERROR){
        int error = WSAGetLastError();
        if(error != WSA_IO_PENDING) {
            fprintf(stderr, "WSASendTo %d", WSAGetLastError());
            pool_free(mem_pool, iocp_context); // Free iocp_context immediately if not pending
            return RET_VAL_ERROR;
        } else {
            // pending
        }
    } else {
        // operation succeded
    }
    return (int)bytes_sent;
}

void refill_recv_iocp_pool(const SOCKET socket, MemPool *mem_pool){
    uint64_t mem_pool_free_blocks = mem_pool->free_blocks;
    for(int i = 0; i < mem_pool_free_blocks; i++){
        IOCP_CONTEXT* recv_context = (IOCP_CONTEXT*)pool_alloc(mem_pool);
        if (recv_context == NULL) {
            fprintf(stderr, "Failed to allocate receive context from mem_pool %d. Exiting.\n", i);
            continue;
        }
        init_iocp_context(recv_context, OP_RECV);
        if (udp_recv_from(socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "Failed to re-post receive operation %d. Exiting.\n", i);
            pool_free(mem_pool, recv_context);
            continue;
        }
    }
    return;
}

int send_frame(const UdpFrame *frame, const SOCKET socket, const struct sockaddr_in *dest_addr, MemPool *mem_pool){
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

    // int bytes_sent = sendto(socket, (const char*)frame, frame_size, 0, (SOCKADDR*)dest_addr, sizeof(*dest_addr));
    // if (bytes_sent == SOCKET_ERROR) {
    //     fprintf(stderr, "sendto() failed with error: %d\n", WSAGetLastError());
    //     return SOCKET_ERROR;        
    // }
    // return bytes_sent;

    return udp_send_to(socket, (const char*)frame, frame_size, dest_addr, mem_pool);

}



int send_ack_frame(const AckUdpFrame *frame, const SOCKET socket, const struct sockaddr_in *dest_addr, MemPool *mem_pool){
    // Determine the actual size to send based on frame type if payloads are variable
    return udp_send_to(socket, (const char*)frame, sizeof(AckUdpFrame), dest_addr, mem_pool);
}














