// --- udp_client.c ---
#include <stdio.h>                      // For printf, fprintf
#include <string.h>                     // For memset, memcpy
#include <stdint.h>                     // For fixed-width integer types
#include <time.h>                       // For time functions
#include <process.h>                    // For _beginthreadex
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <mswsock.h>                    // Optional: For WSARecvFrom and advanced I/O
#include <iphlpapi.h>                   // For IP Helper API functions

#pragma comment(lib, "Ws2_32.lib")      // Link against Winsock library
#pragma comment(lib, "iphlpapi.lib")    // Link against IP Helper API library

#include "include/client.h"
#include "include/client_frames.h"
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/netendians.h"         // For network byte order conversions
#include "include/checksum.h"           // For checksum validation
#include "include/sha256.h"
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management

ClientData client;
ClientBuffers buffers;

HANDLE hthread_recieve_frame;
HANDLE hthread_process_frame;
HANDLE hthread_resend_frame;
HANDLE hthread_keep_alive;
HANDLE hthread_client_command;

const char *server_ip = "10.10.10.1"; // loopback address
const char *client_ip = "10.10.10.3";

DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam);
DWORD WINAPI thread_proc_resend_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_file_transfer(LPVOID lpParam);
DWORD WINAPI thread_proc_message_send(LPVOID lpParam);
DWORD WINAPI thread_proc_client_command(LPVOID lpParam);


static uint64_t get_new_seq_num(){
    return InterlockedIncrement64(&client.frame_count);
}
static int init_client_session(){

    memset(&client, 0, sizeof(ClientData));
    
    client.client_status = STATUS_BUSY;
    client.session_status = CONNECTION_CLOSED;

    client.cid = CLIENT_ID;
    
    client.flags = 0;
    snprintf(client.client_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, CLIENT_NAME);
    client.last_active_time = time(NULL);

    client.frame_count = (uint64_t)UINT32_MAX;
    client.fid_count = 0;
    client.mid_count = 0;

    client.sid = FRAME_TYPE_CONNECT_REQUEST_SID;
    client.server_status = STATUS_CLOSED;
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client.server_name, 0, MAX_NAME_SIZE);
    
    return RET_VAL_SUCCESS;     
}
static int reset_client_session(){
    
    client.session_status = CONNECTION_CLOSED;
    
    client.frame_count = (uint64_t)UINT32_MAX;
    client.fid_count = 0;
    client.mid_count = 0;

    client.sid = FRAME_TYPE_CONNECT_REQUEST_SID;
    client.server_status = STATUS_CLOSED;
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client.server_name, 0, MAX_NAME_SIZE);
    return RET_VAL_SUCCESS;
}
static int init_client_config(){
    
    WSADATA wsaData;
    int rcvBufSize = 5 * 1024 * 1024;  // 2MB
    int sndBufSize = 5 * 1024 * 1024;  // 2MB
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
        return RET_VAL_ERROR;
    }

    client.socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (client.socket == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    client.client_addr.sin_family = AF_INET;
    client.client_addr.sin_port = _htons(0); // Let OS choose port
    client.client_addr.sin_addr.s_addr = inet_addr(client_ip);

            // Set receive buffer
    setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBufSize, sizeof(rcvBufSize));
    // Set send buffer
    setsockopt(client.socket, SOL_SOCKET, SO_SNDBUF, (char*)&sndBufSize, sizeof(sndBufSize));

    if (bind(client.socket, (struct sockaddr *)&client.client_addr, sizeof(client.client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }
   
    client.iocp_handle = CreateIoCompletionPort((HANDLE)client.socket, NULL, 0, 0);
    if (client.iocp_handle == NULL || client.iocp_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateIoCompletionPort failed: %d\n", GetLastError());
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

     // Define server address
    memset(&client.server_addr, 0, sizeof(client.server_addr));
    client.server_addr.sin_family = AF_INET;
    client.server_addr.sin_port = _htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    };

    return RET_VAL_SUCCESS;
}
static int init_client_buffers(){

    // Initialize frames queue
    buffers.queue_frame.head = 0;
    buffers.queue_frame.tail = 0;
    InitializeCriticalSection(&buffers.queue_frame.mutex);
    buffers.queue_frame.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    // Initialize priority frames queue
    buffers.queue_priority_frame.head = 0;
    buffers.queue_priority_frame.tail = 0;
    InitializeCriticalSection(&buffers.queue_priority_frame.mutex);
    buffers.queue_priority_frame.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    
    // Initialize frame hash table
    for(int i = 0; i < HASH_SIZE_FRAME; i++){
        buffers.ht_frame.entry[i] = NULL;
    }
    InitializeCriticalSection(&buffers.ht_frame.mutex); 
    buffers.ht_frame.count = 0;

    // Initialize memory pool for frames
    buffers.ht_frame.pool.block_size = BLOCK_SIZE_FRAME;
    buffers.ht_frame.pool.block_count = BLOCK_COUNT_FRAME;
    pool_init(&buffers.ht_frame.pool);
    
    return RET_VAL_SUCCESS;

}
static int init_client_handles(){
    // Initialize connection request event
    client.hevent_connection_listening = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (client.hevent_connection_listening == NULL) {
        fprintf(stdout, "CreateEvent listen failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    // Initialize connection successfull event
    client.hevent_connection_established = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (client.hevent_connection_established == NULL) {
        fprintf(stdout, "CreateEvent established failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client.hevent_connection_closed = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (client.hevent_connection_closed == NULL) {
        fprintf(stdout, "CreateEvent disconnect failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client.hevent_shutdown = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (client.hevent_shutdown == NULL) {
        fprintf(stdout, "CreateEvent shutdown failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // Initialize stream events
    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        client.fstream[index].hevent_start_file_transfer = CreateEvent(NULL, TRUE, FALSE, NULL);
        client.fstream[index].hevent_close_file_stream_thread = CreateEvent(NULL, FALSE, FALSE, NULL);
        client.fstream[index].hevent_metadata_response = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (client.fstream[index].hevent_start_file_transfer == NULL || 
                client.fstream[index].hevent_close_file_stream_thread == NULL || 
                client.fstream[index].hevent_metadata_response == NULL) {
            
                    fprintf(stderr, "Failed to create file transfer events. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED;
            return RET_VAL_ERROR;
        }
    }

    for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        client.mstream[index].hevent_start_message_send = CreateEvent(NULL, TRUE, FALSE, NULL);
        client.mstream[index].hevent_close_message_stream_thread = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (client.mstream[index].hevent_start_message_send == NULL || client.mstream[index].hevent_close_message_stream_thread == NULL) {
            
            fprintf(stderr, "Failed to create message send events. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED;
            return RET_VAL_ERROR;
        }
    }
    // CLIENT_READY
    client.client_status = STATUS_READY;
    return RET_VAL_SUCCESS;

}
static void start_threads(){

    hthread_process_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_process_frame, NULL, 0, NULL);
    if (hthread_process_frame == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client.session_status = CONNECTION_CLOSED;
        client.client_status = STATUS_CLOSED; // Signal immediate shutdown
    }
    hthread_recieve_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_receive_frame, &client, 0, NULL);
    if (hthread_recieve_frame == NULL) {
        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
        client.session_status = CONNECTION_CLOSED;
        client.client_status = STATUS_CLOSED; // Signal immediate shutdown
    }
    hthread_resend_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_resend_frame, NULL, 0, NULL);
    if (hthread_resend_frame == NULL) {
        fprintf(stderr, "Failed to create resend frame thread. Error: %d\n", GetLastError());
        client.session_status = CONNECTION_CLOSED;
        client.client_status = STATUS_CLOSED; // Signal immediate shutdown
    }
    hthread_keep_alive = (HANDLE)_beginthreadex(NULL, 0, thread_proc_keep_alive, NULL, 0, NULL);
    if (hthread_keep_alive == NULL) {
        fprintf(stderr, "Failed to create keep alive thread. Error: %d\n", GetLastError());
        client.session_status = CONNECTION_CLOSED;
        client.client_status = STATUS_CLOSED; // Signal immediate shutdown
    }
    hthread_client_command = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_command, NULL, 0, NULL);
    if (hthread_client_command == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        client.session_status = CONNECTION_CLOSED;
        client.client_status = STATUS_CLOSED; // Signal immediate shutdown
    }
    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        client.fstream[index].hthread_file_transfer = (HANDLE)_beginthreadex(NULL, 0, thread_proc_file_transfer, (LPVOID)(intptr_t)index, 0, NULL);
        if (client.fstream[index].hthread_file_transfer == NULL){
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED; // Signal immediate shutdown
        }
    }
   for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        client.mstream[index].hthread_message_send = (HANDLE)_beginthreadex(NULL, 0, thread_proc_message_send, (LPVOID)(intptr_t)index, 0, NULL);
        if (client.mstream[index].hthread_message_send == NULL){
            fprintf(stderr, "Failed to create message send thread. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED; // Signal immediate shutdown
        }
    }
}
static void client_shutdown(){

    if(client.session_status == CONNECTION_ESTABLISHED){
        request_disconnect();
    } else {
        force_disconnect();
    }
    
    SetEvent(client.hevent_shutdown);

    if (hthread_recieve_frame) {
        WaitForSingleObject(hthread_recieve_frame, INFINITE);
        CloseHandle(hthread_recieve_frame);
    }
    if (hthread_process_frame) {
        WaitForSingleObject(hthread_process_frame, INFINITE);
        CloseHandle(hthread_process_frame);
    }
    if (hthread_resend_frame) {
        WaitForSingleObject(hthread_resend_frame, INFINITE);
        CloseHandle(hthread_resend_frame);
    }
    if (hthread_keep_alive) {
        WaitForSingleObject(hthread_keep_alive, INFINITE);
        CloseHandle(hthread_keep_alive);
    }
    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        if (client.fstream[index].hthread_file_transfer) {
            WaitForSingleObject(client.fstream[index].hthread_file_transfer, INFINITE);
            CloseHandle(client.fstream[index].hthread_file_transfer);
        }
        CloseHandle(client.fstream[index].hevent_start_file_transfer);
        CloseHandle(client.fstream[index].hevent_close_file_stream_thread);
        CloseHandle(client.fstream[index].hevent_metadata_response);
    }
    for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        if (client.mstream[index].hthread_message_send) {
            WaitForSingleObject(client.mstream[index].hthread_message_send, INFINITE);
            CloseHandle(client.mstream[index].hthread_message_send);
        }
        CloseHandle(client.mstream[index].hevent_start_message_send);
        CloseHandle(client.mstream[index].hevent_close_message_stream_thread);
    }
    CloseHandle(client.hevent_connection_listening);
    CloseHandle(client.hevent_connection_established);
    CloseHandle(client.hevent_connection_closed);

    DeleteCriticalSection(&buffers.queue_frame.mutex);
    DeleteCriticalSection(&buffers.queue_priority_frame.mutex);
    DeleteCriticalSection(&buffers.ht_frame.mutex);

    closesocket(client.socket);
    WSACleanup();

    client.client_status = STATUS_CLOSED;
    
}

// --- Receive frame thread function ---
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam) {

    ClientData *client = (ClientData*)lpParam;
  
    if(issue_WSARecvFrom(client->socket, &client->iocp_context) == RET_VAL_ERROR){
        fprintf(stderr, "Initial WSARecvFrom failed: %d\n", WSAGetLastError());
        client->client_status = STATUS_ERROR;
        //TODO initiate some kind of global error and stop client?
        goto exit_thread;
    }

    HANDLE CompletitionPort = client->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;

    HANDLE events[2] = {client->hevent_connection_listening, client->hevent_shutdown};
    DWORD wait_events;
    DWORD check_shutdown;
   
    while(client->client_status == STATUS_READY){
        
        wait_events = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (wait_events == WAIT_OBJECT_0) {
            fprintf(stdout, "Started listening...\n");
            client->session_status = CONNECTION_LISTENING;
        } else if (wait_events == WAIT_OBJECT_0 + 1){
            goto exit_thread;

        } else {
            fprintf(stderr, "Unexpected error for listening event: %lu\n", wait_events);
            break;
        }

        while(1){

            check_shutdown = WaitForSingleObject(client->hevent_shutdown, 0);
            if (check_shutdown == WAIT_OBJECT_0) {
                goto exit_thread;
            }

            if(client->session_status == CONNECTION_CLOSED){
                fprintf(stderr, "Stopped listening...\n");
                break;
            }

            BOOL getqcompl_result = GetQueuedCompletionStatus(
                CompletitionPort,
                &NrOfBytesTransferred,
                &lpCompletitionKey,
                &lpOverlapped,
                WSARECV_TIMEOUT_MS
            );
            if (!getqcompl_result) {
                int wsa_error = WSAGetLastError();
                // GETQCOMPL_TIMEOUT is expected if no data for WSARECV_TIMEOUT_MS
                if (wsa_error == GETQCOMPL_TIMEOUT) {
                    continue;
                } else {
                    fprintf(stderr, "GetQueuedCompletionStatus failed with error: %d\n", wsa_error);
                    continue;
                }
            }
            
            if (lpOverlapped == NULL) {
                fprintf(stderr, "Warning: NULL pOverlapped received. IOCP may be shutting down.\n");
                continue;
            }

            IOCP_CONTEXT* iocp_overlapped = (IOCP_CONTEXT*)lpOverlapped;

            // Validate and dispatch frame
            if (NrOfBytesTransferred > 0 && NrOfBytesTransferred <= sizeof(UdpFrame)) {
                
                QueueFrameEntry frame_entry = {0};
                memcpy(&frame_entry.frame, iocp_overlapped->buffer, NrOfBytesTransferred);
                memcpy(&frame_entry.src_addr, &iocp_overlapped->src_addr, sizeof(struct sockaddr_in));
                frame_entry.frame_size = NrOfBytesTransferred;

                if(frame_entry.frame_size > sizeof(UdpFrame)){
                    fprintf(stdout, "Frame received with bytes > max frame size!\n");
                    continue;
                }
    
                uint8_t frame_type = frame_entry.frame.header.frame_type;
                uint8_t op_code = frame_entry.frame.payload.ack.op_code;
                
                BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_CONNECT_RESPONSE ||
                                                 frame_type == FRAME_TYPE_DISCONNECT ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == STS_KEEP_ALIVE) ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_DISCONNECT) ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_FILE_METADATA) ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_FILE_END) ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == ERR_DUPLICATE_FRAME) ||
                                                (frame_type == FRAME_TYPE_ACK && op_code == ERR_EXISTING_FILE)
                                            );

                QueueFrame *target_queue = NULL;
                if (is_high_priority_frame == TRUE) {
                    target_queue = &buffers.queue_priority_frame;
                } else {
                    target_queue = &buffers.queue_frame;
                }
                if (push_frame(target_queue, &frame_entry) == RET_VAL_ERROR) {
                    fprintf(stderr, "Failed to push frame to queue. Queue full?\n");
                }          
            }

            if(issue_WSARecvFrom(client->socket, iocp_overlapped) == RET_VAL_ERROR){
                fprintf(stderr, "WSARecvFrom re-issue failed: %d\n", WSAGetLastError());
                continue;
            }

        } // end of while(1)
    } // end of while(client.client_status == STATUS_READY)
exit_thread:
    fprintf(stdout,"receive frame thread closed...\n");
    _endthreadex(0);    
    return 0;
}
// --- Processes a received frame ---
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam) {

    QueueFrameEntry frame_entry;
    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;
    uint32_t recvfrom_bytes_received;

    uint16_t recv_delimiter = 0;
    uint8_t  recv_frame_type = 0;
    uint64_t recv_seq_num = 0;
    uint32_t recv_session_id = 0;    

    uint32_t recv_session_timeout;
    uint8_t recv_server_status;

    HANDLE queue_semaphores[3] = {buffers.queue_priority_frame.semaphore, 
                                    buffers.queue_frame.semaphore,
                                    client.hevent_shutdown
                                };

    while(client.client_status == STATUS_READY){
        // Pop a frame from the queue (prioritize control queue)

        DWORD wait_result = WaitForMultipleObjects(3, queue_semaphores, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            if (pop_frame(&buffers.queue_priority_frame, &frame_entry) == RET_VAL_SUCCESS) {
                // Frame successfully retrieved from the priority queue.
            } else {
                continue;
            }
        } else if (wait_result == WAIT_OBJECT_0 + 1) {
            if (pop_frame(&buffers.queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
                // Frame successfully retrieved from the general data queue.
            } else {
                continue;
            }
        } else if (wait_result == WAIT_OBJECT_0 + 2){
            goto exit_thread;

        } else {
            fprintf(stderr, "Unexpected wait result: %lu\n", wait_result);
            continue;
        }

        DWORD check_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
        if (check_shutdown == WAIT_OBJECT_0) {
            goto exit_thread;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        recvfrom_bytes_received = frame_entry.frame_size;

        // Extract header fields   
        recv_delimiter = _ntohs(frame->header.start_delimiter);
        recv_frame_type = frame->header.frame_type;
        recv_seq_num = _ntohll(frame->header.seq_num);
        recv_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);
       
        if (recv_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, recv_delimiter);
            continue;
        }        
        if (!is_checksum_valid(frame, recvfrom_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }
        switch (recv_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE:
                recv_server_status = frame->payload.connection_response.server_status;
                recv_session_timeout = _ntohl(frame->payload.connection_response.session_timeout);               
                if(recv_session_id == 0 || recv_server_status != STATUS_READY){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    break;
                }
                if(recv_session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    break;
                }
                fprintf(stdout, "Received connect response from %s:%d with session ID: %d, timeout: %d seconds, server status: %d\n", 
                                                        src_ip, src_port, recv_session_id, recv_session_timeout, recv_server_status);
                client.server_status = recv_server_status;
                client.session_timeout = recv_session_timeout;
                client.sid = recv_session_id;
                snprintf(client.server_name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, frame->payload.connection_response.server_name);
                client.last_active_time = time(NULL);
                SetEvent(client.hevent_connection_established);
                break;

            case FRAME_TYPE_ACK:
                if(recv_session_id != client.sid){
                    //fprintf(stderr, "Received ACK frame with invalid session ID: %d\n", recv_session_id);
                    //TODO - send ACK frame with error code for invalid session ID
                    break;
                }
                client.last_active_time = time(NULL);
                uint8_t recv_op_code = frame->payload.ack.op_code;

                for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
                    if(recv_seq_num == client.fstream[i].pending_metadata_seq_num && 
                                    (recv_op_code == STS_CONFIRM_FILE_METADATA || 
                                     recv_op_code == ERR_DUPLICATE_FRAME || 
                                     recv_op_code == ERR_EXISTING_FILE)
                        ) {
                        client.fstream[i].pending_metadata_seq_num = 0; // Reset pending metadata sequence number  
                        SetEvent(client.fstream[i].hevent_metadata_response);
                    }
                }

                if(recv_seq_num == FRAME_TYPE_DISCONNECT_SEQ && recv_op_code == STS_CONFIRM_DISCONNECT){
                    SetEvent(client.hevent_connection_closed);
                    fprintf(stdout, "Received disconnect ACK code: %lu; for seq num: %llx\n", frame->payload.ack.op_code, recv_seq_num);
                }

                if(recv_op_code == STS_FRAME_DATA_ACK || 
                        recv_op_code == STS_KEEP_ALIVE || 
                        recv_op_code == STS_CONFIRM_DISCONNECT ||
                        recv_op_code == STS_CONFIRM_FILE_METADATA ||
                        recv_op_code == STS_CONFIRM_FILE_END ||
                        recv_op_code == ERR_DUPLICATE_FRAME || 
                        recv_op_code == ERR_EXISTING_FILE 
                    ) {
                    ht_remove_frame(&buffers.ht_frame, recv_seq_num);
                }
                break;

            case FRAME_TYPE_DISCONNECT:
                if(recv_session_id != client.sid){
                    break;                    
                }
                force_disconnect();
                fprintf(stdout, "Session closed by server...\n");
                break;

            case FRAME_TYPE_CONNECT_REQUEST:
                break;
                
            case FRAME_TYPE_KEEP_ALIVE:
                break;
            default:
                break;
        }
    }
exit_thread:
    fprintf(stdout,"process frame thread exiting...\n");
    _endthreadex(0);    
    return 0;
}
// --- Send keep alive ---
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam){

    time_t now_keep_alive = time(NULL);
    time_t last_keep_alive = time(NULL);
    
    while(client.client_status == STATUS_READY){

        DWORD check_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
        if (check_shutdown == WAIT_OBJECT_0) {
            goto exit_thread;
        }

        if(client.session_status == CONNECTION_ESTABLISHED){
            DWORD keep_alive_clock_sec = (DWORD)((DWORD)client.session_timeout / 5);

            now_keep_alive = time(NULL);
            if(now_keep_alive - last_keep_alive > keep_alive_clock_sec){
                send_keep_alive(get_new_seq_num(), client.sid, client.socket, &client.server_addr);
                fprintf(stdout, "Sending keep alive frame session id: %u\n", client.sid);
                last_keep_alive = time(NULL);
            }
            if(time(NULL) > (time_t)(client.last_active_time + client.session_timeout * 2)){
                force_disconnect();
            }

            Sleep(1000);
        } else {
            now_keep_alive = time(NULL);
            last_keep_alive = time(NULL);
            Sleep(1000);
            continue;
        }
    }
exit_thread:
    fprintf(stdout,"keep alive thread exiting...\n");
    _endthreadex(0);
    return 0;
}
// --- Re-send frames that ack time expired ---
DWORD WINAPI thread_proc_resend_frame(LPVOID lpParam){
   
    while(client.client_status == STATUS_READY){
        
        DWORD check_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
        if (check_shutdown == WAIT_OBJECT_0) {
            ht_clean(&buffers.ht_frame);
            goto exit_thread;
        }
        if(client.session_status == CONNECTION_CLOSED){
            ht_clean(&buffers.ht_frame);
            Sleep(100);
            continue;
        }
        time_t current_time = time(NULL);
        if(buffers.ht_frame.count == 0){
            Sleep(100);
            continue;
        }
        for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
            BOOL frames_to_resend = client.fstream[i].remaining_bytes_to_send == 0 || client.fstream[i].throttle;
            if(!frames_to_resend){
                Sleep(100);
                continue;
            }
            EnterCriticalSection(&buffers.ht_frame.mutex);
            for (int i = 0; i < HASH_SIZE_FRAME; i++) {
                FramePendingAck *ptr = buffers.ht_frame.entry[i];
                while (ptr) {
                    if(current_time - ptr->time > (time_t)RESEND_TIMEOUT_SEC){
                        send_frame(&ptr->frame, client.socket, &client.server_addr);
                        ptr->time = current_time;
                    }
                    ptr = ptr->next;
                }                                         
            }
            LeaveCriticalSection(&buffers.ht_frame.mutex);
        }
        Sleep(100);
    }
exit_thread:
    fprintf(stdout,"resend frame thread exiting...\n");
    _endthreadex(0);
    return 0;
}
// --- File transfer thread function ---
DWORD WINAPI thread_proc_file_transfer(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;
    FileStream *fstream = &client.fstream[index];
    
    SHA256_CTX sha256_ctx;
    uint32_t chunk_bytes_to_send;
    uint32_t chunk_fragment_offset;
    uint32_t frame_fragment_size;
    uint64_t frame_fragment_offset;

    uint8_t chunk_buffer[FILE_CHUNK_SIZE];
    
    DWORD wait_file_events;
    DWORD wait_metadata_response;
    DWORD check_stop_transfer;
    DWORD check_client_shutdown;
    HANDLE file_events[3] = {fstream->hevent_start_file_transfer, fstream->hevent_close_file_stream_thread, client.hevent_shutdown};

    while(client.client_status == STATUS_READY){
        
        wait_file_events = WaitForMultipleObjects(3, file_events, FALSE, INFINITE);

        if(wait_file_events == WAIT_OBJECT_0){
            // start file transfer event
        } else if (wait_file_events == WAIT_OBJECT_0 + 1){
            // stop file transfer event
            goto clean;
        } else if (wait_file_events == WAIT_OBJECT_0 + 2){
            goto exit_thread;
        } else {
            fprintf(stderr, "Unexpected wait file events result: %lu\n", wait_file_events);
            goto clean;
        }
        
        fstream->fpath = TEST_FILE_PATH;
        fstream->fname = "test_file.txt";

        sha256_init(&sha256_ctx);
        fstream->fp = NULL;
        
        fstream->fsize = get_file_size(fstream->fpath);       
        if(fstream->fsize == RET_VAL_ERROR){
            goto clean;
        }

        fstream->fp = fopen(fstream->fpath, "rb");
        if(fstream->fp == NULL){
            fprintf(stdout, "Error opening file!!!\n");
            goto clean;
        }
 
        fstream->fid = InterlockedIncrement(&client.fid_count);

        fstream->pending_metadata_seq_num = get_new_seq_num();
        int metadata_bytes_sent = send_file_metadata(fstream->pending_metadata_seq_num, 
                                                        client.sid, 
                                                        fstream->fid, 
                                                        fstream->fsize, 
                                                        fstream->fname, 
                                                        FILE_FRAGMENT_SIZE, 
                                                        client.socket, 
                                                        &client.server_addr,
                                                        &buffers
                                                    );
        if(metadata_bytes_sent == RET_VAL_ERROR){
            fprintf(stderr, "Failed to send file metadata frame. Cancelling transfer\n");
            goto clean;
        }

        //TODO - fix bug?
//        wait_metadata_response = WaitForSingleObject(fstream->hevent_metadata_response, TIMEOUT_METADATA_RESPONSE_MS);
        wait_metadata_response = WaitForSingleObject(fstream->hevent_metadata_response, INFINITE);
        
        if (wait_metadata_response == WAIT_OBJECT_0) {
        // The event was signaled within the timeout —> proceed sending the rest of the file fragments
        } else if (wait_metadata_response == WAIT_TIMEOUT) {
            fprintf(stderr, "Timeout error waiting for metadata response\n");
            continue;  
        } else {
            // Unexpected error — maybe invalid handle
            fprintf(stderr, "Unexpected wait metadata response result: %lu\n", wait_metadata_response);
            goto clean;
        }

        check_stop_transfer = WaitForSingleObject(fstream->hevent_close_file_stream_thread, 0);
        if (check_stop_transfer == WAIT_OBJECT_0) {
            goto clean;
        }
        check_client_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
        if (check_client_shutdown == WAIT_OBJECT_0) {
            goto exit_thread;
        }

        frame_fragment_offset = 0;
        fstream->remaining_bytes_to_send = fstream->fsize;

        while(fstream->remaining_bytes_to_send > 0){
            // handle stop signal received during file transfer
            check_stop_transfer = WaitForSingleObject(fstream->hevent_close_file_stream_thread, 0);
            if (check_stop_transfer == WAIT_OBJECT_0) {
                //fprintf(stderr, "File transfer force-stopped for stream %d\n", index);
                goto clean;
            }

            check_client_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
            if (check_client_shutdown == WAIT_OBJECT_0) {
                goto exit_thread;
            }

            if(buffers.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                fstream->throttle = TRUE;
            }
            if(buffers.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                fstream->throttle = FALSE;
            }
            if(fstream->throttle){
                Sleep(10);
                continue;
            }

            chunk_bytes_to_send = fread(chunk_buffer, 1, FILE_CHUNK_SIZE, fstream->fp);
            if (chunk_bytes_to_send == 0 && ferror(fstream->fp)) {
                fprintf(stderr, "Error reading file\n");
                goto clean;
            }           

            sha256_update(&sha256_ctx, (const uint8_t *)chunk_buffer, chunk_bytes_to_send);
 
            chunk_fragment_offset = 0;

            while (chunk_bytes_to_send > 0){

                if(chunk_bytes_to_send > FILE_FRAGMENT_SIZE){
                    frame_fragment_size = FILE_FRAGMENT_SIZE;
                } else {
                    frame_fragment_size = chunk_bytes_to_send;
                }
                
                char buffer[FILE_FRAGMENT_SIZE];

                const char *offset = chunk_buffer + chunk_fragment_offset;
                memcpy(buffer, offset, frame_fragment_size);
                if(frame_fragment_size < FILE_FRAGMENT_SIZE){
                    memset(buffer + frame_fragment_size, 0, FILE_FRAGMENT_SIZE - frame_fragment_size);
                }

                int fragment_bytes_sent = send_file_fragment(get_new_seq_num(), 
                                                                client.sid, 
                                                                fstream->fid, 
                                                                frame_fragment_offset, 
                                                                buffer, 
                                                                frame_fragment_size, 
                                                                client.socket, &client.server_addr,
                                                                &buffers
                                                            );
                if(fragment_bytes_sent == RET_VAL_ERROR){
                    Sleep(100);
                    continue;
                }

                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                fstream->remaining_bytes_to_send -= frame_fragment_size;
            }     
        }                  

        sha256_final(&sha256_ctx, (uint8_t *)&fstream->fhash.sha256);
        send_file_end(get_new_seq_num(), 
                        client.sid, 
                        fstream->fid, 
                        fstream->fsize, 
                        (uint8_t *)&fstream->fhash.sha256,
                        client.socket, 
                        &client.server_addr,
                        &buffers
                    );

    clean:  //clean stream
        clean_file_stream(fstream);
    }
exit_thread:
    clean_file_stream(fstream);
    fprintf(stdout,"file transfer thread [%d] closing...\n", index);
    _endthreadex(0);
    return 0;               
}
// --- Send message thread function ---
DWORD WINAPI thread_proc_message_send(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;
    MessageStream *mstream = &client.mstream[index];

    uint32_t frame_fragment_offset;
    uint32_t frame_fragment_len;

    DWORD wait_message_events;
    DWORD check_stop_send;
    DWORD check_client_shutdown;
    HANDLE message_events[3] = {mstream->hevent_start_message_send, mstream->hevent_close_message_stream_thread, client.hevent_shutdown};

    while(client.client_status == STATUS_READY){
        
        wait_message_events = WaitForMultipleObjects(3, message_events, FALSE, INFINITE);

        if(wait_message_events == WAIT_OBJECT_0){
            // start message event
        } else if (wait_message_events == WAIT_OBJECT_0 + 1){
            // stop message send event
            goto clean;
        } else if (wait_message_events == WAIT_OBJECT_0 + 2){
            goto exit_thread; 
        } else {
            fprintf(stderr, "Unexpected wait message events result: %lu\n", wait_message_events);
            goto clean;
        }
        
        mstream->fpath = TEST_FILE_PATH;
        mstream->fname = "test_file.txt";       
        
        mstream->fp = NULL;
        mstream->message_buffer = NULL;
        
        mstream->text_file_size = get_file_size(mstream->fpath);
      
        if(mstream->text_file_size == RET_VAL_ERROR){
            goto clean;
        }
        if(mstream->text_file_size > MAX_MESSAGE_SIZE_BYTES){
            fprintf(stdout, "Message file is too large! Message Size: %llu > Max size: %u\n", mstream->text_file_size, MAX_MESSAGE_SIZE_BYTES);
            goto clean;
        }
        mstream->message_len = (uint32_t)mstream->text_file_size;

        mstream->fp = fopen(mstream->fpath, "rb");
        if(mstream->fp == NULL){
            fprintf(stdout, "Error opening file!\n");
            goto clean;
        }

        mstream->message_id = InterlockedIncrement(&client.mid_count);
        frame_fragment_offset = 0;

        mstream->message_buffer = malloc(mstream->message_len + 1);
        if(mstream->message_buffer == NULL){
            fprintf(stdout, "Error allocating memeory buffer for message!\n");
            goto clean;
        }

        mstream->remaining_bytes_to_send = fread(mstream->message_buffer, 1, mstream->message_len, mstream->fp);
        mstream->message_buffer[mstream->message_len] = '\0';
        if (mstream->remaining_bytes_to_send == 0 && ferror(mstream->fp)) {
            fprintf(stdout, "Error reading message file!\n");
            goto clean;
        }

        while(mstream->remaining_bytes_to_send > 0){

            check_stop_send = WaitForSingleObject(mstream->hevent_close_message_stream_thread, 0);
            if (check_stop_send == WAIT_OBJECT_0) {
                goto clean;
            }

            check_client_shutdown = WaitForSingleObject(client.hevent_shutdown, 0);
            if (check_client_shutdown == WAIT_OBJECT_0) {
                goto exit_thread;
            }

            if(mstream->remaining_bytes_to_send > TEXT_FRAGMENT_SIZE){
                frame_fragment_len = TEXT_FRAGMENT_SIZE;
            } else {
                frame_fragment_len = mstream->remaining_bytes_to_send;
            }

            if(buffers.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                mstream->throttle = TRUE;
            }
            if(buffers.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                mstream->throttle = FALSE;
            }
            if(mstream->throttle){
                Sleep(5);
                continue;
            }

            char buffer[TEXT_FRAGMENT_SIZE];

            const char *offset = mstream->message_buffer + frame_fragment_offset;
            memcpy(buffer, offset, frame_fragment_len);
            if(frame_fragment_len < TEXT_FRAGMENT_SIZE){
                buffer[frame_fragment_len] = '\0';
            }

            int fragment_bytes_sent = send_long_text_fragment(get_new_seq_num(), 
                                                                client.sid, 
                                                                mstream->message_id, 
                                                                mstream->message_len, 
                                                                frame_fragment_offset, 
                                                                buffer, 
                                                                frame_fragment_len, 
                                                                client.socket, 
                                                                &client.server_addr,
                                                                &buffers
                                                            );
            if(fragment_bytes_sent == RET_VAL_ERROR){
                Sleep(10);
                continue;
            }
            frame_fragment_offset += frame_fragment_len;                       
            mstream->remaining_bytes_to_send -= frame_fragment_len;
        }

    clean:
        clean_message_stream(mstream);
    }
exit_thread:
    clean_message_stream(mstream);
    fprintf(stdout,"message thread [%d] exiting...\n", index);
    _endthreadex(0);
    return 0; 
}
// --- Process command ---
DWORD WINAPI thread_proc_client_command(LPVOID lpParam) {

    char cmd;
    int index;
    int retry_count;
     
    while(client.client_status == STATUS_READY){

        //fprintf(stdout,"Waiting for command...\n");

        cmd = getchar();
        switch(cmd) {
            //--------------------------------------------------------------------------------------------------------------------------
            case 'c':
            case 'C': // connect
                request_connect();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'd':
            case 'D': // disconnect
                request_disconnect();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'q':
            case 'Q': // shutdown
                client_shutdown();
                goto exit_thread;
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'f':
            case 'F':
                transfer_file();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 't':
            case 'T':
                send_text_message();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case '\n':
                break;
            default:
                fprintf(stdout, "Invalid command!\n");
                break;
            
        }
    Sleep(100);
    }        
exit_thread:
    fprintf(stdout, "client command thread exiting...\n");
    _endthreadex(0);    
    return 0;
}

// --- Main function ---
int main() {

    init_client_session();
    init_client_config();
    init_client_buffers();
    init_client_handles();
    start_threads();
    while(client.client_status == STATUS_READY){
        fprintf(stdout, "\r\033[2K-- File: %.2f , Text: %.2f , Hash F: %u, Free B: %llu", 
                            (float)(client.fstream[0].fsize - client.fstream[0].remaining_bytes_to_send) / (float)client.fstream[0].fsize * 100.0, 
                            (float)(client.mstream[0].message_len - client.mstream[0].remaining_bytes_to_send) / (float)client.mstream[0].message_len * 100.0,
                            buffers.ht_frame.count,
                            buffers.ht_frame.pool.free_blocks
                            );
        fflush(stdout);

        // if(client.session_status == CONNECTION_CLOSED){
        //     reset_client_session();
        // }     
        Sleep(250); // Simulate some delay between messages        
    }
    if (hthread_client_command) {
        WaitForSingleObject(hthread_client_command, INFINITE);
        CloseHandle(hthread_client_command);
    }
    //fprintf(stdout, "Client shutting down!!!\n");
    //client_shutdown();
    
    return 0;
}

void request_connect(){
    
    SetEvent(client.hevent_connection_listening);
    // send connect request frame
    send_connect_request(get_new_seq_num(), 
                            client.sid, 
                            client.cid, 
                            client.flags, 
                            client.client_name, 
                            client.socket, 
                            &client.server_addr
                        );
    DWORD wait_connection_established = WaitForSingleObject(client.hevent_connection_established, CONNECT_REQUEST_TIMEOUT_MS);
    if (wait_connection_established == WAIT_OBJECT_0) {
        client.session_status = CONNECTION_ESTABLISHED;
        fprintf(stdout, "Connection established...\n");
    } else if (wait_connection_established == WAIT_TIMEOUT) {
        ht_clean(&buffers.ht_frame);
        reset_client_session();
        fprintf(stderr, "Connection closed...\n");
        return;
    } else {
        ht_clean(&buffers.ht_frame);
        reset_client_session();
        fprintf(stderr, "Unexpected error for established event: %lu\n", wait_connection_established);
        return;
    }
    return;
}

void request_disconnect(){
    if(client.session_status == CONNECTION_CLOSED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    // send disconnect frame
    send_disconnect(client.sid, client.socket, &client.server_addr);
 
    DWORD wait_connection_closed = WaitForSingleObject(client.hevent_connection_closed, DISCONNECT_REQUEST_TIMEOUT_MS);

    if (wait_connection_closed == WAIT_OBJECT_0) {
        fprintf(stderr, "Connection closed\n"); 
    } else if (wait_connection_closed == WAIT_TIMEOUT) {
        fprintf(stdout, "Connection close timeout - closing connection anyway\n");
    } else {    
        fprintf(stderr, "Unexpected error for disconnect event: %lu\n", wait_connection_closed);
    }
       
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        SetEvent(client.fstream[i].hevent_close_file_stream_thread);
    }
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        SetEvent(client.mstream[i].hevent_close_message_stream_thread);
    }
    ht_clean(&buffers.ht_frame);
    reset_client_session();
    return;
}

void force_disconnect(){
    if(client.session_status == CONNECTION_CLOSED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    SetEvent(client.hevent_connection_closed);
    DWORD wait_connection_closed = WaitForSingleObject(client.hevent_connection_closed, DISCONNECT_REQUEST_TIMEOUT_MS);
    if (wait_connection_closed == WAIT_OBJECT_0) {
        fprintf(stderr, "Connection closed\n"); 
    } else if (wait_connection_closed == WAIT_TIMEOUT) {
        fprintf(stdout, "CONNECTION CLOSE BY SERVER (TIMEOUT?)-> SHOULD NOT HAPPEN\n");
    } else {    
        fprintf(stderr, "Unexpected error for disconnect event: %lu\n", wait_connection_closed);
    }
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        SetEvent(client.fstream[i].hevent_close_file_stream_thread);
    }
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        SetEvent(client.mstream[i].hevent_close_message_stream_thread);
    }
    ht_clean(&buffers.ht_frame);
    reset_client_session();
    return;
}

void timeout_disconnect(){
    return;
}

void transfer_file(){

    int index;

    if(client.session_status != CONNECTION_ESTABLISHED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    for(index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        DWORD check_file_transfer = WaitForSingleObject(client.fstream[index].hevent_start_file_transfer, 0);
        if (check_file_transfer == WAIT_OBJECT_0) {
            // Event is signaled (This stream is busy with transferring)
            continue;
        } else if (check_file_transfer == WAIT_TIMEOUT) {
            // Event is not signaled (This stream can be used)
            fprintf(stdout, "File stream %d opened...\n", index);
            SetEvent(client.fstream[index].hevent_start_file_transfer);
            return;
        }
    }
    if(index == MAX_CLIENT_FILE_STREAMS){
        fprintf(stderr, "Max threads reached!\n");
    }
    return;

}

void send_text_message(){

    int index;

    if(client.session_status != CONNECTION_ESTABLISHED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    
    for(index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        DWORD check_message_send = WaitForSingleObject(client.mstream[index].hevent_start_message_send, 0);
        if (check_message_send == WAIT_OBJECT_0) {
            // Event is signaled
            continue;
        } else if (check_message_send == WAIT_TIMEOUT) {
            // Event is not signaled
            SetEvent(client.mstream[index].hevent_start_message_send);
            fprintf(stdout, "Message stream %d opened...\n", index);
            return;
        }
    }
    if(index == MAX_CLIENT_MESSAGE_STREAMS){
        fprintf(stderr, "Max message threads reached!\n");
    }
    return;
}

static void clean_file_stream(FileStream *fstream){
    memset((uint8_t *)&fstream->fhash.sha256, 0, 32);
    if(fstream->fp != NULL){
        fclose(fstream->fp);
        fstream->fp = NULL;
    }
    fstream->fid = 0;
    fstream->fsize = 0;
    fstream->remaining_bytes_to_send = 0;
    fstream->throttle = FALSE;
    ResetEvent(fstream->hevent_start_file_transfer);
    return;
}

static void clean_message_stream(MessageStream *mstream){
    if(mstream->message_buffer != NULL){
        free(mstream->message_buffer);
        mstream->message_buffer = NULL;
    }
    if(mstream->fp != NULL){
        fclose(mstream->fp);
        mstream->fp = NULL;
    }
    mstream->message_id = 0;
    mstream->message_len = 0;
    mstream->text_file_size = 0;
    mstream->remaining_bytes_to_send = 0;
    mstream->throttle = FALSE;
    ResetEvent(mstream->hevent_start_message_send);
    return;
}