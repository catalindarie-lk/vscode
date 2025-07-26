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
#include "include/client_api.h"

ClientData client;
ClientThreads threads;
ClientBuffers buffers;

HANDLE hthread_recieve_frame;
HANDLE hthread_process_frame;
HANDLE hthread_resend_frame;
HANDLE hthread_keep_alive;
HANDLE hthread_client_command;
//HANDLE hthread_file_transfer[MAX_CLIENT_ACTIVE_FSTREAMS];
HANDLE hthread_queue_command[MAX_CLIENT_ACTIVE_FSTREAMS];

const char *server_ip = "10.10.10.1"; // loopback address
const char *client_ip = "10.10.10.3";

DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam);
DWORD WINAPI thread_proc_resend_frame(LPVOID lpParam);
DWORD WINAPI thread_fstream_function(LPVOID lpParam);
DWORD WINAPI thread_mstream_function(LPVOID lpParam);
DWORD WINAPI thread_proc_client_command(LPVOID lpParam);

static void clean_file_stream(ClientFileStream *fstream);
void SendTextMessage(const char *message_buffer, const size_t message_len);



void sent_text_file();


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
    // int rcvBufSize = 5 * 1024 * 1024;  // 2MB
    // int sndBufSize = 5 * 1024 * 1024;  // 2MB
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

    // setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBufSize, sizeof(rcvBufSize));
    // setsockopt(client.socket, SOL_SOCKET, SO_SNDBUF, (char*)&sndBufSize, sizeof(sndBufSize));

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

    init_queue_frame(&buffers.queue_frame, CLIENT_SIZE_QUEUE_FRAME);
    init_queue_frame(&buffers.queue_priority_frame, CLIENT_SIZE_QUEUE_PRIORITY_FRAME);
    
    pool_init(&buffers.ht_frame.pool, BLOCK_SIZE_FRAME, BLOCK_COUNT_FRAME);
    init_ht_frame(&buffers.ht_frame, HASH_SIZE_FRAME);
    
    init_queue_command(&buffers.queue_fstream, CLIENT_SIZE_QUEUE_COMMAND_FSTREAM);
    init_queue_command(&buffers.queue_mstream, CLIENT_SIZE_QUEUE_COMMAND_MSTREAM);

    for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
        memset(&buffers.fstream[index], 0, sizeof(ClientFileStream));
        buffers.fstream[index].chunk_buffer = malloc(FILE_CHUNK_SIZE);
    }
    for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
        memset(&buffers.mstream[index], 0, sizeof(ClientMessageStream));
        buffers.mstream[index].message_buffer = malloc(MAX_MESSAGE_SIZE_BYTES);
    }
    
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
    client.hevent_connection_established = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (client.hevent_connection_established == NULL) {
        fprintf(stdout, "CreateEvent established failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client.hevent_connection_closed = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (client.hevent_connection_closed == NULL) {
        fprintf(stdout, "CreateEvent disconnect failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // Initialize fstreams
    for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
        buffers.fstream[index].hevent_metadata_response = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (buffers.fstream[index].hevent_metadata_response == NULL) {
            fprintf(stderr, "Failed to create fstream events. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED;
            return RET_VAL_ERROR;
        }
        InitializeCriticalSection(&buffers.fstream[index].lock);
    }
    InitializeCriticalSection(&buffers.fstreams_lock);
    // Initialize mstreams
    for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
        InitializeCriticalSection(&buffers.mstream[index].lock);
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

    // START FSTREAM WORKER THREADS
    threads.fstream_semaphore = CreateSemaphore(NULL, MAX_CLIENT_ACTIVE_FSTREAMS, LONG_MAX, NULL);
    for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
        threads.fstream[index] = (HANDLE)_beginthreadex(NULL, 0, thread_fstream_function, NULL, 0, NULL);
        if (threads.fstream[index] == NULL){
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            client.session_status = CONNECTION_CLOSED;
            client.client_status = STATUS_CLOSED; // Signal immediate shutdown
        }
    }

   // START MSTREAM WORKER THREADS
    threads.mstream_semaphore = CreateSemaphore(NULL, MAX_CLIENT_ACTIVE_MSTREAMS, LONG_MAX, NULL);
    for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
        threads.mstream[index] = (HANDLE)_beginthreadex(NULL, 0, thread_mstream_function, NULL, 0, NULL);
        if (threads.mstream[index] == NULL){
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
        // goto exit_thread;
    }

    HANDLE CompletitionPort = client->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;

    // DWORD wait_events;
    DWORD check_shutdown;
   
    while(client->client_status == STATUS_READY){
        WaitForSingleObject(client->hevent_connection_listening, INFINITE);       

        client->session_status = CONNECTION_LISTENING;
        while(1){

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

    HANDLE events[2] = {buffers.queue_priority_frame.push_semaphore,
                        buffers.queue_frame.push_semaphore,
                        };

    while(client.client_status == STATUS_READY){

        DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            if (pop_frame(&buffers.queue_priority_frame, &frame_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from frame priority queue RET_VAL_ERROR\n");
                continue;
            }
        } else if (result == WAIT_OBJECT_0 + 1) {
            if (pop_frame(&buffers.queue_frame, &frame_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from frame queue RET_VAL_ERROR\n");
                continue;
            }
        } else {
            fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Unexpected result wait semaphore frame queues: %lu\n", result);
            continue;
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
                    if(recv_seq_num == buffers.fstream[i].pending_metadata_seq_num && 
                                    (recv_op_code == STS_CONFIRM_FILE_METADATA || 
                                     recv_op_code == ERR_DUPLICATE_FRAME || 
                                     recv_op_code == ERR_EXISTING_FILE)
                        ) {
                        buffers.fstream[i].pending_metadata_seq_num = 0; // Reset pending metadata sequence number  
                        SetEvent(buffers.fstream[i].hevent_metadata_response);
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
    fprintf(stdout,"process frame thread exiting...\n");
    _endthreadex(0);    
    return 0;
}
// --- Send keep alive ---
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam){

    time_t now_keep_alive = time(NULL);
    time_t last_keep_alive = time(NULL);
    
    while(client.client_status == STATUS_READY){
        if(client.session_status == CONNECTION_ESTABLISHED){
            DWORD keep_alive_clock_sec = (DWORD)((DWORD)client.session_timeout / 5);

            now_keep_alive = time(NULL);
            if(now_keep_alive - last_keep_alive > keep_alive_clock_sec){
                send_keep_alive(get_new_seq_num(), client.sid, client.socket, &client.server_addr);
                // fprintf(stdout, "Sending keep alive frame session id: %u\n", client.sid);
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
        for(int i = 0; i < MAX_CLIENT_ACTIVE_FSTREAMS; i++){
            BOOL frames_to_resend = buffers.fstream[i].pending_bytes == 0 || buffers.fstream[i].throttle;
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
DWORD WINAPI thread_fstream_function(LPVOID lpParam){
   
    SHA256_CTX sha256_ctx;
    uint32_t chunk_bytes_to_send;
    uint32_t chunk_fragment_offset;
    uint32_t frame_fragment_size;
    uint64_t frame_fragment_offset;
   
    // DWORD wait_metadata_response;

    QueueCommandEntry entry;

    while(client.client_status == STATUS_READY){

        WaitForSingleObject(threads.fstream_semaphore, INFINITE);
        
        pop_command(&buffers.queue_fstream, &entry);
         
        ClientFileStream *fstream = NULL;
        EnterCriticalSection(&buffers.fstreams_lock);
        for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
            fstream = &buffers.fstream[index];
            if(!fstream->fstream_busy){
                fstream->fstream_busy = TRUE;
                break;
            }
        }
        LeaveCriticalSection(&buffers.fstreams_lock);

        if(!fstream){
            fprintf(stderr, "ERROR: All fstreams are busy!\n");
            continue;
        }
        
        EnterCriticalSection(&fstream->lock);
        
        snprintf(fstream->fpath, MAX_PATH, "%s", entry.command.send_file.fpath);
        snprintf(fstream->rpath, MAX_PATH, "%s", entry.command.send_file.rpath);
        snprintf(fstream->fname, MAX_PATH, "%s", entry.command.send_file.fname);

        sha256_init(&sha256_ctx);       
        fstream->fp = NULL;
        
        char _FileName[MAX_PATH] = {0};
        snprintf(_FileName, MAX_PATH, "%s%s", fstream->fpath, fstream->fname);

        fstream->fsize = get_file_size(_FileName);
        if(fstream->fsize == RET_VAL_ERROR){
            goto clean;
        }

        fstream->fp = fopen(_FileName, "rb");
        if(fstream->fp == NULL){
            fprintf(stdout, "Error opening file!!!\n");
            goto clean;
        }
 
        fstream->fid = InterlockedIncrement(&client.fid_count);
        fprintf(stdout, "Metadata ID: %d\n", fstream->fid);

        fstream->pending_metadata_seq_num = get_new_seq_num();
        int metadata_bytes_sent = send_file_metadata(fstream->pending_metadata_seq_num, 
                                                        client.sid, 
                                                        fstream->fid, 
                                                        fstream->fsize,
                                                        fstream->rpath, 
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

        WaitForSingleObject(fstream->hevent_metadata_response, INFINITE);
        
        frame_fragment_offset = 0;
        fstream->pending_bytes = fstream->fsize;

        while(fstream->pending_bytes > 0){

            chunk_bytes_to_send = fread(fstream->chunk_buffer, 1, FILE_CHUNK_SIZE, fstream->fp);
            if (chunk_bytes_to_send == 0 && ferror(fstream->fp)) {
                fprintf(stderr, "Error reading file\n");
                goto clean;
            }           

            sha256_update(&sha256_ctx, (const uint8_t *)fstream->chunk_buffer, chunk_bytes_to_send);
 
            chunk_fragment_offset = 0;

            while (chunk_bytes_to_send > 0){

                // THROTTLE
                if(buffers.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                    fstream->throttle = TRUE;
                }
                if(buffers.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                    fstream->throttle = FALSE;
                }
                if(fstream->throttle){
                    Sleep(1);
                    continue;
                }

                // CONTINUE
                if(chunk_bytes_to_send > FILE_FRAGMENT_SIZE){
                    frame_fragment_size = FILE_FRAGMENT_SIZE;
                } else {
                    frame_fragment_size = chunk_bytes_to_send;
                }
                
                char buffer[FILE_FRAGMENT_SIZE];

                const char *offset = fstream->chunk_buffer + chunk_fragment_offset;
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
                    Sleep(10);
                    continue;
                }

                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                fstream->pending_bytes -= frame_fragment_size;
                
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
    clean:
        clean_file_stream(fstream);
        LeaveCriticalSection(&fstream->lock);
        ReleaseSemaphore(threads.fstream_semaphore, 1, NULL);
    }

    fprintf(stderr, "EXITING FILE STREAM THREAD\n");
    _endthreadex(0);
    return 0;               
}
// --- Send message thread function ---
DWORD WINAPI thread_mstream_function(LPVOID lpParam){

    uint32_t frame_fragment_offset;
    uint32_t frame_fragment_len;

    QueueCommandEntry entry;

    while(client.client_status == STATUS_READY){

         WaitForSingleObject(threads.mstream_semaphore, INFINITE);

        if (pop_command(&buffers.queue_mstream, &entry) == RET_VAL_ERROR) {
            free(entry.command.send_message.message_buffer);
            fprintf(stdout, "ERROR: Popping mstream command from queue\n");
            continue;
        }
        
        if(entry.command.send_message.message_len >= MAX_MESSAGE_SIZE_BYTES){
            fprintf(stderr, "ERROR: Message size is too big.\n");
            continue;
        }

        if(entry.command.send_message.message_len <= 0){
            fprintf(stderr, "ERROR: Message size not valid (0 or less).\n");
            continue;
        }
        
        ClientMessageStream *mstream = NULL;
        for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
            mstream = &buffers.mstream[index];
            if(!mstream->mstream_busy){
                EnterCriticalSection(&mstream->lock);
                mstream->mstream_busy = TRUE;
                mstream->message_len = entry.command.send_message.message_len;
                memcpy(mstream->message_buffer, entry.command.send_message.message_buffer, mstream->message_len);
                mstream->message_buffer[mstream->message_len] = '\0';
                free(entry.command.send_message.message_buffer);
                fprintf(stdout, "Free message stream %d opened...\n", index);
                break;
            }
        }

        if(!mstream){
            LeaveCriticalSection(&mstream->lock);
            fprintf(stderr, "ERROR: All fstreams are busy!\n");
            continue;
        }
        
        mstream->message_id = InterlockedIncrement(&client.mid_count);
        frame_fragment_offset = 0;

        mstream->remaining_bytes_to_send = mstream->message_len;

        while(mstream->remaining_bytes_to_send > 0){

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
                Sleep(1);
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

        clean_message_stream(mstream);
        ReleaseSemaphore(threads.mstream_semaphore, 1, NULL);
        LeaveCriticalSection(&mstream->lock);

    }
    _endthreadex(0);
    return 0; 
}

// --- Process command ---
DWORD WINAPI thread_proc_client_command(LPVOID lpParam) {

    char cmd;
    int index;
    int retry_count;
    char _path[MAX_PATH] = {0};
     
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
            case 'h':
            case 'H':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s%s", SRC_FPATH, "test_file.txt");
                SendSingleFile(_path);
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'f':
            case 'F':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", SRC_FPATH);
                SendAllFilesInFolder(_path);
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'g':
            case 'G':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", SRC_FPATH);
                SendAllFilesInFolderAndSubfolders(_path);
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 't':
            case 'T':
                sent_text_file();
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
        fprintf(stdout, "\r\033[2K-- File: %.2f , Text: %.2f , Hash F: %u, Free B: %llu; Pending: %d", 
                            (float)(buffers.fstream[0].fsize - buffers.fstream[0].pending_bytes) / (float)buffers.fstream[0].fsize * 100.0, 
                            (float)(buffers.mstream[0].message_len - buffers.mstream[0].remaining_bytes_to_send) / (float)buffers.mstream[0].message_len * 100.0,
                            buffers.ht_frame.count,
                            buffers.ht_frame.pool.free_blocks,
                            buffers.queue_fstream.pending
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

    ResetEvent(client.hevent_connection_closed);
    ResetEvent(client.hevent_connection_established);
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
    } else {
        ht_clean(&buffers.ht_frame);
        reset_client_session();
        fprintf(stderr, "Unexpected error for established event: %lu\n", wait_connection_established);
    }
    return;
}

void request_disconnect(){
    if(client.session_status == CONNECTION_CLOSED){
        fprintf(stdout, "Not connected to server\n");
        return;
    }
    // send disconnect frame
    ResetEvent(client.hevent_connection_established);
    send_disconnect(client.sid, client.socket, &client.server_addr);
 
    DWORD wait_connection_closed = WaitForSingleObject(client.hevent_connection_closed, DISCONNECT_REQUEST_TIMEOUT_MS);

    if (wait_connection_closed == WAIT_OBJECT_0) {
        fprintf(stderr, "Connection closed\n"); 
    } else if (wait_connection_closed == WAIT_TIMEOUT) {
        fprintf(stdout, "Connection close timeout - closing connection anyway\n");
    } else {    
        fprintf(stderr, "Unexpected error for disconnect event: %lu\n", wait_connection_closed);
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
    ht_clean(&buffers.ht_frame);
    reset_client_session();
    return;
}

void timeout_disconnect(){
    return;
}


static void clean_file_stream(ClientFileStream *fstream){
    EnterCriticalSection(&fstream->lock);
    memset((uint8_t *)&fstream->fhash.sha256, 0, 32);
    if(fstream->fp){
        fclose(fstream->fp);
        fstream->fp = NULL;
    }
    fstream->fid = 0;
    fstream->fsize = 0;
    fstream->pending_bytes = 0;
    fstream->throttle = FALSE;
    fstream->fstream_busy = FALSE;
    memset(&fstream->fpath, 0, MAX_PATH);
    memset(&fstream->rpath, 0, MAX_PATH);
    memset(&fstream->fname, 0, MAX_PATH);
    LeaveCriticalSection(&fstream->lock);
    return;
}

static void clean_message_stream(ClientMessageStream *mstream){
    EnterCriticalSection(&mstream->lock);
    memset(mstream->message_buffer, 0, MAX_MESSAGE_SIZE_BYTES);
    mstream->message_id = 0;
    mstream->message_len = 0;
    mstream->remaining_bytes_to_send = 0;
    mstream->throttle = FALSE;
    mstream->mstream_busy = FALSE;
    LeaveCriticalSection(&mstream->lock);
    return;
}



void sent_text_file(){


        char *fpath = "H:\\_test\\";
        char *fname = "test_file.txt";
        
        char full_path[MAX_PATH] = {0};
        snprintf(full_path, MAX_PATH, "%s%s", fpath, fname);
        
        FILE *fp = NULL;
        
        size_t text_file_size = get_file_size(full_path);
      
        uint32_t message_len = (uint32_t)text_file_size;

        fp = fopen(full_path, "rb");
        if(fp == NULL){
            fprintf(stdout, "Error opening file!\n");
            return;
        }

        char *message_buffer = malloc(message_len + 1);
        if(message_buffer == NULL){
            fprintf(stdout, "Error allocating memeory buffer for message!\n");
            return;
        }


        size_t bytes_read = fread(message_buffer, 1, message_len, fp);
        message_buffer[message_len] = '\0';
        if (bytes_read == 0 && ferror(fp)) {
            fprintf(stdout, "Error reading message file!\n");
            return;
        }
        SendTextMessage(message_buffer, message_len);
        free(message_buffer);
 
    return;
}

void SendTextMessage(const char *message_buffer, const size_t message_len) {

    QueueCommandEntry entry;

    if(!message_buffer){
        fprintf(stderr, "ERROR: Message buffer invalid pointer.\n");
        return;
    }
    if(message_len <= 0){
        fprintf(stderr, "ERROR: Message length zero or less lenght.\n");
        return;
    }
    if(message_len >= MAX_MESSAGE_SIZE_BYTES){
        fprintf(stderr, "ERROR: Message size too long.\n");
        return;
    }

    snprintf(entry.command.send_message.text, sizeof("SendTextMessage"), "%s", "SendTextMessage");
    entry.command.send_message.message_len = message_len;
    entry.command.send_message.message_buffer = malloc(message_len + 1);
    memcpy(entry.command.send_message.message_buffer, message_buffer, message_len);
    entry.command.send_message.message_buffer[message_len] = '\0';

    push_command(&buffers.queue_mstream, &entry);
    return;
}




