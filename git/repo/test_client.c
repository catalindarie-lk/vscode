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

HANDLE hthread_recv_send_frame[CLIENT_RECV_SEND_FRAME_WRK_THREADS];
HANDLE hthread_process_frame;
HANDLE hthread_resend_tx_frame;
HANDLE hthread_keep_alive;
HANDLE hthread_client_command;
HANDLE hthread_queue_command[MAX_CLIENT_ACTIVE_FSTREAMS];
HANDLE hthread_pop_tx_frame[1];
HANDLE hthread_pop_prio_tx_frame[1];

const char *server_ip = "10.10.10.1"; // loopback address
const char *client_ip = "10.10.10.3";

DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam);
DWORD WINAPI thread_proc_resend_tx_frame(LPVOID lpParam);
DWORD WINAPI thread_fstream_function(LPVOID lpParam);
DWORD WINAPI thread_mstream_function(LPVOID lpParam);
DWORD WINAPI thread_proc_client_command(LPVOID lpParam);

DWORD WINAPI thread_func_pop_tx_frame(LPVOID lpParam);
DWORD WINAPI thread_func_pop_prio_tx_frame(LPVOID lpParam);

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

    client.frame_count = 1;//(uint64_t)UINT32_MAX;
    client.fid_count = 0;
    client.mid_count = 0;

    client.sid = DEFAULT_CONNECT_REQUEST_SID;
    client.server_status = STATUS_CLOSED;
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client.server_name, 0, MAX_NAME_SIZE);
    
    return RET_VAL_SUCCESS;     
}
static int reset_client_session(){
    
    client.session_status = CONNECTION_CLOSED;
    
    client.frame_count = 1;//(uint64_t)UINT32_MAX;
    client.fid_count = 0;
    client.mid_count = 0;

    client.sid = DEFAULT_CONNECT_REQUEST_SID;
    client.server_status = STATUS_CLOSED;
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    // Initialize client data
    memset(client.server_name, 0, MAX_NAME_SIZE);
    return RET_VAL_SUCCESS;
}
static int init_client_config(){
    
    WSADATA wsaData;

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
    
    // pool_init(&buffers.ht_frame.pool, BLOCK_SIZE_FRAME, BLOCK_COUNT_FRAME);
    // init_ht_frame(&buffers.ht_frame, HASH_SIZE_FRAME);
    
    init_queue_command(&buffers.queue_fstream, CLIENT_SIZE_QUEUE_COMMAND_FSTREAM);
    init_queue_command(&buffers.queue_mstream, CLIENT_SIZE_QUEUE_COMMAND_MSTREAM);

    pool_init(&buffers.pool_iocp_send_context, sizeof(IOCP_CONTEXT), IOCP_SEND_MEM_POOL_BLOCKS);
    pool_init(&buffers.pool_iocp_recv_context, sizeof(IOCP_CONTEXT), IOCP_RECV_MEM_POOL_BLOCKS);

    s_pool_init(&buffers.pool_tx_frames, sizeof(UdpFrame), IOCP_SEND_MEM_POOL_BLOCKS / 2);

    init_queue_tx_frame(&buffers.queue_tx_frame, 128);
    init_queue_tx_frame(&buffers.queue_prio_tx_frame, 128);
    
    htbl_init_txframe(&buffers.htable_tx_frame, 128, IOCP_SEND_MEM_POOL_BLOCKS);


    for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
        memset(&buffers.fstream[index], 0, sizeof(ClientFileStream));
        buffers.fstream[index].chunk_buffer = malloc(FILE_CHUNK_SIZE);
    }
    for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
        memset(&buffers.mstream[index], 0, sizeof(ClientMessageStream));
        buffers.mstream[index].message_buffer = malloc(MAX_MESSAGE_SIZE_BYTES);
    }

    for (int i = 0; i < IOCP_RECV_MEM_POOL_BLOCKS; ++i) {
        IOCP_CONTEXT* recv_context = (IOCP_CONTEXT*)pool_alloc(&buffers.pool_iocp_recv_context);
        if (recv_context == NULL) {
            fprintf(stderr, "Failed to allocate receive context from pool %d. Exiting.\n", i);
            // Proper cleanup for partially initialized pools and sockets.
            pool_destroy(&buffers.pool_iocp_send_context);
            pool_destroy(&buffers.pool_iocp_recv_context);
            CloseHandle(client.iocp_handle);
            closesocket(client.socket);
            WSACleanup();
            return EXIT_FAILURE;
        }
        init_iocp_context(recv_context, OP_RECV); // Initialize the context

        if (udp_recv_from(client.socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "Failed to post initial receive operation %d. Exiting.\n", i);
            pool_free(&buffers.pool_iocp_recv_context, recv_context); // Free the one that failed
            // Proper cleanup for partially initialized pools and sockets.
            pool_destroy(&buffers.pool_iocp_send_context);
            pool_destroy(&buffers.pool_iocp_recv_context);
            CloseHandle(client.iocp_handle);
            closesocket(client.socket);
            WSACleanup();
            return EXIT_FAILURE;
        }
    }
    printf("Server: All initial receive operations posted.\n");
    
    return RET_VAL_SUCCESS;
}
static int init_client_handles(){
    // Initialize connection request event
    client.hevent_connection_pending = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (client.hevent_connection_pending == NULL) {
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
        buffers.fstream[index].hevent_metadata_response_ok = CreateEvent(NULL, FALSE, FALSE, NULL);
        buffers.fstream[index].hevent_metadata_response_nok = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (buffers.fstream[index].hevent_metadata_response_ok == NULL || 
            buffers.fstream[index].hevent_metadata_response_nok == NULL) {
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
static int start_threads(){
    for(int i = 0; i < CLIENT_RECV_SEND_FRAME_WRK_THREADS; i++){
        hthread_recv_send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_recv_send_frame, &client, 0, NULL);
        if (hthread_recv_send_frame[i] == NULL) {
            fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    hthread_process_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_process_frame, NULL, 0, NULL);
    if (hthread_process_frame == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_resend_tx_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_resend_tx_frame, NULL, 0, NULL);
    if (hthread_resend_tx_frame == NULL) {
        fprintf(stderr, "Failed to create resend tx_frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_keep_alive = (HANDLE)_beginthreadex(NULL, 0, thread_proc_keep_alive, NULL, 0, NULL);
    if (hthread_keep_alive == NULL) {
        fprintf(stderr, "Failed to create keep alive thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    hthread_client_command = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_command, NULL, 0, NULL);
    if (hthread_client_command == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    // START FSTREAM WORKER THREADS
    threads.fstream_semaphore = CreateSemaphore(NULL, MAX_CLIENT_ACTIVE_FSTREAMS, LONG_MAX, NULL);
    for(int index = 0; index < MAX_CLIENT_ACTIVE_FSTREAMS; index++){
        threads.fstream[index] = (HANDLE)_beginthreadex(NULL, 0, thread_fstream_function, NULL, 0, NULL);
        if (threads.fstream[index] == NULL){
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }

    // START MSTREAM WORKER THREADS
    threads.mstream_semaphore = CreateSemaphore(NULL, MAX_CLIENT_ACTIVE_MSTREAMS, LONG_MAX, NULL);
    for(int index = 0; index < MAX_CLIENT_ACTIVE_MSTREAMS; index++){
        threads.mstream[index] = (HANDLE)_beginthreadex(NULL, 0, thread_mstream_function, NULL, 0, NULL);
        if (threads.mstream[index] == NULL){
            fprintf(stderr, "Failed to create message send thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
for(int i = 0; i < 1; i++){
    hthread_pop_tx_frame[i] = (HANDLE)_beginthreadex(NULL, 0, thread_func_pop_tx_frame, NULL, 0, NULL);
    if (hthread_pop_tx_frame[i] == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
}
for(int i = 0; i < 1; i++){
    hthread_pop_prio_tx_frame[i] = (HANDLE)_beginthreadex(NULL, 0, thread_func_pop_prio_tx_frame, NULL, 0, NULL);
    if (hthread_pop_prio_tx_frame[i] == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
}
    return RET_VAL_SUCCESS;
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
DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam) {

    ClientData *cli = &client;
    ClientBuffers *buff = &buffers;

    QueueFrame *q_frame = &buffers.queue_frame;
    QueueFrame *q_prio_frame = &buffers.queue_priority_frame;

    MemPool *recv_pool = &buffers.pool_iocp_recv_context;
    MemPool *send_pool = &buffers.pool_iocp_send_context;
  
    HANDLE CompletitionPort = cli->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;
    char ip_string_buffer[INET_ADDRSTRLEN];

    QueueFrameEntry frame_entry;
    
    while(cli->client_status == STATUS_READY){
        WaitForSingleObject(cli->hevent_connection_pending, INFINITE);       

        cli->session_status = CONNECTION_PENDING;
        while(true){

            if(cli->session_status == CONNECTION_CLOSED){
                fprintf(stderr, "Stopped listening...\n");
                break;
            }

            BOOL getqcompl_result = GetQueuedCompletionStatus(
                CompletitionPort,
                &NrOfBytesTransferred,
                &lpCompletitionKey,
                &lpOverlapped,
                INFINITE //WSARECV_TIMEOUT_MS
            );
                        
            if (lpOverlapped == NULL) {
                fprintf(stderr, "Warning: NULL pOverlapped received. IOCP may be shutting down.\n");
                continue;
            }

            IOCP_CONTEXT* context = (IOCP_CONTEXT*)lpOverlapped;

            // --- Handle GetQueuedCompletionStatus failures (non-NULL lpOverlapped) ---
            if (!getqcompl_result) {
                int wsa_error = WSAGetLastError();
                if (wsa_error == GETQCOMPL_TIMEOUT) {
                    // Timeout, no completion occurred. Continue looping.
                    continue;
                } else {
                    fprintf(stderr, "GetQueuedCompletionStatus failed with error: %d\n", wsa_error);
                    // If it's a real error on a specific operation
                    if (context->type == OP_SEND) {
                        pool_free(send_pool, context);
                    } else if (context->type == OP_RECV) {
                        // Critical error on a receive context -"retire" this context from the pool.
                        fprintf(stderr, "Client: Error in RECV operation, attempting re-post context %p...\n", (void*)context);
                        pool_free(recv_pool, context);
                    }
                    continue; // Continue loop to get next completion
                }
            }

            switch(context->type){
                case OP_RECV:
                    // Validate and dispatch frame
                    if (NrOfBytesTransferred > 0 && NrOfBytesTransferred <= sizeof(UdpFrame)) {
                        memset(&frame_entry, 0, sizeof(QueueFrameEntry));
                        memcpy(&frame_entry.frame, context->buffer, NrOfBytesTransferred);
                        memcpy(&frame_entry.src_addr, &context->addr, sizeof(struct sockaddr_in));
                        frame_entry.frame_size = NrOfBytesTransferred;
            
                        uint8_t frame_type = frame_entry.frame.header.frame_type;
                        uint8_t op_code = 0;
                        if(frame_type == FRAME_TYPE_ACK){
                            op_code = frame_entry.frame.payload.ack.op_code;
                        }                
                        
                        BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_CONNECT_RESPONSE ||
                                                        frame_type == FRAME_TYPE_DISCONNECT ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == STS_KEEP_ALIVE) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_DISCONNECT) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_FILE_METADATA) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == STS_CONFIRM_FILE_END) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == ERR_DUPLICATE_FRAME) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == ERR_EXISTING_FILE) ||
                                                        (frame_type == FRAME_TYPE_ACK && op_code == ERR_STREAM_INIT)
                                                    );

                        if (is_high_priority_frame == TRUE) {
                            if (push_frame(q_prio_frame, &frame_entry) == RET_VAL_ERROR) {
                                Sleep(100);
                                continue;
                            }
                        } else {
                            if (push_frame(q_frame, &frame_entry) == RET_VAL_ERROR) {
                                Sleep(100);
                                continue;
                            }
                        }

                        // if (inet_ntop(AF_INET, &(context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                        //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                        // }
                        // printf("Server: Received %lu bytes from %s:%d. Type: %u\n",
                        //        NrOfBytesTransferred, ip_string_buffer, ntohs(context->addr.sin_port), frame_entry.frame.header.frame_type);

                    } else {
                        // 0 bytes transferred (e.g., graceful shutdown, empty packet)
                        fprintf(stdout, "Client: Receive operation completed with 0 bytes for context %p. Re-posting.\n", (void*)context);
                    }

                    // *** CRITICAL: Re-post the receive operation using the SAME context ***
                    // This ensures the buffer is continuously available for incoming data.
                    if (udp_recv_from(cli->socket, context) == RET_VAL_ERROR){
                        fprintf(stderr, "Critical: WSARecvFrom re-issue failed for context %p: %d. Freeing.\n", (void*)context, WSAGetLastError());
                        // This is a severe problem. Retire the context from the pool.
                        pool_free(recv_pool, context); // Return to pool if it fails
                    }
                    if(recv_pool->free_blocks > (recv_pool->block_count / 2)){
                        refill_recv_iocp_pool(cli->socket, recv_pool);
                    }
                    break; // End of OP_RECV case

                case OP_SEND:
                    // For send completions, simply free the context
                    if (NrOfBytesTransferred > 0) {
                        // if (inet_ntop(AF_INET, &(context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                        //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                        // }
                        // printf("Client: Sent %lu bytes to %s:%d (Message: '%s')\n",
                        //     NrOfBytesTransferred, ip_string_buffer, ntohs(iocp_context->addr.sin_port), iocp_context->buffer);
                    } else {
                        fprintf(stderr, "Client: Send operation completed with 0 bytes or error.\n");
                    }
                    pool_free(send_pool, context);
                    break;

                default:
                    fprintf(stderr, "Client: Unknown operation type in completion.\n");
                    pool_free(send_pool, context);
                    break;

            } // end of switch(context->type)
        } // end of while(true)
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

        memset(&frame_entry, 0, sizeof(QueueFrameEntry));
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
                
                uintptr_t entry = htbl_remove_txframe(&buffers.htable_tx_frame, recv_seq_num);
                if(!entry){
                    fprintf(stderr, "CRITICAL ERROR: fail to remove from tx_frame hash table? - null pointer returned!\n");
                    break;
                }
                s_pool_free(&buffers.pool_tx_frames, (void*)entry);
                
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
                                        (recv_op_code == STS_CONFIRM_FILE_METADATA)) 
                    {
                        buffers.fstream[i].pending_metadata_seq_num = 0; 
                        SetEvent(buffers.fstream[i].hevent_metadata_response_ok);
                    } else if(recv_seq_num == buffers.fstream[i].pending_metadata_seq_num && 
                                        (recv_op_code == ERR_DUPLICATE_FRAME ||
                                        recv_op_code == ERR_EXISTING_FILE ||
                                        recv_op_code == ERR_STREAM_INIT))
                    {
                        buffers.fstream[i].pending_metadata_seq_num = 0;
                        SetEvent(buffers.fstream[i].hevent_metadata_response_nok);                 
                    }
                    
                }

                if(recv_seq_num == DEFAULT_DISCONNECT_SEQ && recv_op_code == STS_CONFIRM_DISCONNECT){
                    SetEvent(client.hevent_connection_closed);
                    fprintf(stdout, "Received disconnect ACK code: %lu; for seq num: %llx\n", frame->payload.ack.op_code, recv_seq_num);
                }

                if(recv_op_code == STS_FRAME_DATA_ACK || 
                        recv_op_code == STS_KEEP_ALIVE || 
                        recv_op_code == STS_CONFIRM_DISCONNECT ||
                        recv_op_code == STS_CONFIRM_FILE_METADATA ||
                        recv_op_code == STS_CONFIRM_FILE_END ||
                        recv_op_code == ERR_DUPLICATE_FRAME || 
                        recv_op_code == ERR_EXISTING_FILE ||
                        recv_op_code == ERR_STREAM_INIT
                    ){
                    // ht_remove_frame(&buffers.ht_frame, recv_seq_num);

                    uintptr_t entry = htbl_remove_txframe(&buffers.htable_tx_frame, recv_seq_num);
                    if(!entry){
                        fprintf(stderr, "CRITICAL ERROR: fail to remove from tx_frame hash table? - null pointer returned!\n");
                        break;
                    }
                    s_pool_free(&buffers.pool_tx_frames, (void*)entry);
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
// --- Pop a frame from frame queue for processing ---
DWORD WINAPI thread_func_pop_tx_frame(LPVOID lpParam){
   
    while(client.client_status == STATUS_READY){
        
        UdpFrame *frame = (UdpFrame*)pop_tx_frame(&buffers.queue_tx_frame);
        if(!frame){
            fprintf(stderr,"Poped empty pointer from tx_frame?\n");
            continue;
        }
        htbl_insert_txframe(&buffers.htable_tx_frame, (uintptr_t)frame);
        send_frame(frame, client.socket, &client.server_addr, &buffers.pool_iocp_send_context);

   }
    _endthreadex(0);    
    return 0;
}
// --- Pop a frame from priority queue for processing ---
DWORD WINAPI thread_func_pop_prio_tx_frame(LPVOID lpParam){
   
    while(client.client_status == STATUS_READY){
        
        UdpFrame *frame = (UdpFrame*)pop_tx_frame(&buffers.queue_prio_tx_frame);
        if(!frame){
            fprintf(stderr,"Poped empty pointer from tx_frame?\n");
            continue;
        }
        htbl_insert_txframe(&buffers.htable_tx_frame, (uintptr_t)frame);
        send_frame(frame, client.socket, &client.server_addr, &buffers.pool_iocp_send_context);

   }
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
                UdpFrame *frame = s_pool_alloc(&buffers.pool_tx_frames);
                if(!frame){
                    fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for keep alive frame. Should never do since it has semaphore to block when full");
                    Sleep(1000);
                    continue;
                }
                int res = construct_keep_alive(frame,
                                            get_new_seq_num(), 
                                            client.sid);
                if(res == RET_VAL_ERROR){
                    fprintf(stderr, "CRITICAL ERROR: construct_keep_alive() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
                    Sleep(1000);
                    continue;
                }
                push_tx_frame(&buffers.queue_prio_tx_frame, (uintptr_t)frame);
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
    fprintf(stdout,"keep alive thread exiting...\n");
    _endthreadex(0);
    return 0;
}
// --- Send frames not acknowledges within set time ---
DWORD WINAPI thread_proc_resend_tx_frame(LPVOID lpParam){
   
    while(client.client_status == STATUS_READY){ 
        // if(client.session_status == CONNECTION_CLOSED){
        //     ht_clean(&buffers.ht_frame);
        //     Sleep(250);
        //     continue;
        // }
        time_t current_time = time(NULL);
        if(buffers.htable_tx_frame.count == 0){
            Sleep(250);
            continue;
        }
        for(int i = 0; i < MAX_CLIENT_ACTIVE_FSTREAMS; i++){
            BOOL frames_to_resend = buffers.fstream[i].pending_bytes == 0;
            // if(!frames_to_resend){
            //     Sleep(250);
            //     continue;
            // }
            EnterCriticalSection(&buffers.htable_tx_frame.mutex);
            for (int i = 0; i < buffers.htable_tx_frame.size; i++) {
                hTblNode_txFrame *ptr = buffers.htable_tx_frame.head[i];
                while (ptr) {
                    UdpFrame *frame = (UdpFrame*)ptr->frame;
                    if(frame->header.frame_type == FRAME_TYPE_FILE_METADATA){
                        // send_frame(&ptr->frame, client.socket, &client.server_addr, &buffers.pool_iocp_send_context);
                        // ptr->time = current_time;
                        // Sleep(1000);
                    }
                    if(current_time - ptr->sent_time > (time_t)RESEND_TIMEOUT_SEC){
                        send_frame(frame, client.socket, &client.server_addr, &buffers.pool_iocp_send_context);
                        // push_tx_frame(&buffers.queue_tx_frame, (uintptr_t)frame);
                        ptr->sent_time = current_time;
                    }
                    ptr = ptr->next;
                }                                         
            }
            LeaveCriticalSection(&buffers.htable_tx_frame.mutex);
        }
        Sleep(250);
    }
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

    UdpFrame *frame;
    int res;
   
    // DWORD wait_metadata_response;

    QueueCommandEntry entry;

    while(client.client_status == STATUS_READY){
        memset(&entry, 0, sizeof(QueueCommandEntry));
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
       
        // Safely copy paths using the lengths from the queue entry
        // fpath
        int result = snprintf(fstream->fpath, MAX_PATH, "%.*s",
                                   (int)entry.command.send_file.fpath_len,
                                   entry.command.send_file.fpath);
        if (result < 0 || (size_t)result != entry.command.send_file.fpath_len) {
            fprintf(stderr, "ERROR: thread_fstream_function - Failed to copy fpath '%.*s' (truncation or error). Result: %d, Expected: %u\n",
                    (int)entry.command.send_file.fpath_len, entry.command.send_file.fpath,
                    result, entry.command.send_file.fpath_len);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->fpath_len = entry.command.send_file.fpath_len;
        // rpath
        result = snprintf(fstream->rpath, MAX_PATH, "%.*s",
                                   (int)entry.command.send_file.rpath_len,
                                   entry.command.send_file.rpath);
        if (result < 0 || (size_t)result != entry.command.send_file.rpath_len) {
            fprintf(stderr, "ERROR: thread_fstream_function - Failed to copy rpath '%.*s' (truncation or error). Result: %d, Expected: %u\n",
                    (int)entry.command.send_file.rpath_len, entry.command.send_file.rpath,
                    result, entry.command.send_file.rpath_len);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->rpath_len = entry.command.send_file.rpath_len;

        // fname
        result = snprintf(fstream->fname, MAX_PATH, "%.*s",
                                   (int)entry.command.send_file.fname_len,
                                   entry.command.send_file.fname);
        if (result < 0 || (size_t)result != entry.command.send_file.fname_len) {
            fprintf(stderr, "ERROR: thread_fstream_function - Failed to copy fname '%.*s' (truncation or error). Result: %d, Expected: %u\n",
                    (int)entry.command.send_file.fname_len, entry.command.send_file.fname,
                    result, entry.command.send_file.fname_len);
            goto clean; // Essential to clean up if path copy fails
        }
        fstream->fname_len = entry.command.send_file.fname_len;

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
        
        frame = s_pool_alloc(&buffers.pool_tx_frames);
        if(!frame){
            fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for metadata frame. Should never do since it has semaphore to block when full");
            goto clean;
        }
        res = construct_file_metadata(frame,
                                        fstream->pending_metadata_seq_num, 
                                        client.sid, 
                                        fstream->fid, 
                                        fstream->fsize,
                                        fstream->rpath,
                                        fstream->rpath_len, 
                                        fstream->fname, 
                                        fstream->fname_len,
                                        FILE_FRAGMENT_SIZE
                                        );
        if(res == RET_VAL_ERROR){
            fprintf(stderr, "CRITICAL ERROR: construct_file_metadata() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
            goto clean;
        }
        push_tx_frame(&buffers.queue_prio_tx_frame, (uintptr_t)frame);

        HANDLE events[2] = {fstream->hevent_metadata_response_ok, fstream->hevent_metadata_response_nok};

        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        
        if(wait_result == WAIT_OBJECT_0){

        } else if (wait_result == WAIT_OBJECT_0 + 1){
            goto clean;
        } else {
            goto clean;
        }
        
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
                // if(buffers.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                //     fstream->throttle = TRUE;
                // }
                // if(buffers.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                //     fstream->throttle = FALSE;
                // }
                // if(fstream->throttle){
                //     Sleep(1);
                //     continue;
                // }

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
                
                frame = s_pool_alloc(&buffers.pool_tx_frames);
                if(!frame){
                    fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for file fragment frame. Should never do since it has semaphore to block when full");
                    goto clean;
                }
                res = construct_file_fragment(frame, 
                                                get_new_seq_num(),
                                                client.sid,
                                                fstream->fid,
                                                frame_fragment_offset,
                                                buffer, 
                                                frame_fragment_size 
                                                );
                if(res == RET_VAL_ERROR){
                    fprintf(stderr, "CRITICAL ERROR: construct_file_fragment() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
                    goto clean;
                }
                
                push_tx_frame(&buffers.queue_tx_frame, (uintptr_t)frame);

                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                fstream->pending_bytes -= frame_fragment_size;
            }     
        }                  

        sha256_final(&sha256_ctx, (uint8_t *)&fstream->fhash.sha256);

        frame = s_pool_alloc(&buffers.pool_tx_frames);
        if(!frame){
            fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for end frame. Should never do since it has semaphore to block when full");
            goto clean;
        }
        res = construct_file_end(frame,
                                    get_new_seq_num(), 
                                    client.sid, 
                                    fstream->fid, 
                                    fstream->fsize, 
                                    (uint8_t *)&fstream->fhash.sha256);
        if(res == RET_VAL_ERROR){
            fprintf(stderr, "CRITICAL ERROR: construct_file_end() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
            goto clean;
        }
        push_tx_frame(&buffers.queue_prio_tx_frame, (uintptr_t)frame);

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
    UdpFrame *frame;
    int res;

    while(client.client_status == STATUS_READY){

        WaitForSingleObject(threads.mstream_semaphore, INFINITE);

        if(pop_command(&buffers.queue_mstream, &entry) == RET_VAL_ERROR) {
            if(!entry.command.send_message.message_buffer){
                fprintf(stderr, "ERROR: Queue message buffer invalid pointer.\n");
                continue;
            }
            free(entry.command.send_message.message_buffer);
            entry.command.send_message.message_buffer = NULL;
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

            // if(buffers.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
            //     mstream->throttle = TRUE;
            // }
            // if(buffers.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
            //     mstream->throttle = FALSE;
            // }
            // if(mstream->throttle){
            //     Sleep(1);
            //     continue;
            // }

            char buffer[TEXT_FRAGMENT_SIZE];

            const char *offset = mstream->message_buffer + frame_fragment_offset;
            memcpy(buffer, offset, frame_fragment_len);
            if(frame_fragment_len < TEXT_FRAGMENT_SIZE){
                buffer[frame_fragment_len] = '\0';
            }

            // int fragment_bytes_sent = construct_text_fragment(get_new_seq_num(), 
            //                                                     client.sid, 
            //                                                     mstream->message_id, 
            //                                                     mstream->message_len, 
            //                                                     frame_fragment_offset, 
            //                                                     buffer, 
            //                                                     frame_fragment_len, 
            //                                                     client.socket, 
            //                                                     &client.server_addr,
            //                                                     &buffers,
            //                                                     &buffers.pool_iocp_send_context
            //                                                 );

            // if(fragment_bytes_sent == RET_VAL_ERROR){
            //     Sleep(10);
            //     continue;
            // }

            frame = s_pool_alloc(&buffers.pool_tx_frames);
            if(!frame){
                fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for text fragment. Should never do since it has semaphore to block when full");
                goto clean;
            }

            res = construct_text_fragment(frame,
                                            get_new_seq_num(), 
                                            client.sid, 
                                            mstream->message_id, 
                                            mstream->message_len, 
                                            frame_fragment_offset, 
                                            buffer, 
                                            frame_fragment_len
                                            );
            if(res == RET_VAL_ERROR){
                fprintf(stderr, "CRITICAL ERROR: construct_text_fragment() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
                goto clean;
            }
            push_tx_frame(&buffers.queue_tx_frame, (uintptr_t)frame);

            frame_fragment_offset += frame_fragment_len;                       
            mstream->remaining_bytes_to_send -= frame_fragment_len;
        }

    clean:
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
                // client_shutdown();
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'h':
            case 'H':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s%s", CLIENT_ROOT_FOLDER, "test_file.txt");
                SendSingleFile(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'f':
            case 'F':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", CLIENT_ROOT_FOLDER);
                SendAllFilesInFolder(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 'g':
            case 'G':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", CLIENT_ROOT_FOLDER);
                SendAllFilesInFolderAndSubfolders(_path, strlen(_path));
                break;
            //--------------------------------------------------------------------------------------------------------------------------
            case 't':
            case 'T':
                if(client.session_status != CONNECTION_ESTABLISHED){
                    break;
                }
                memset(_path, 0, MAX_PATH);
                snprintf(_path, MAX_PATH, "%s", "D:\\_test\\test_file.txt");
                SendTextInFile(_path, strlen(_path));
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
        fprintf(stdout, "\r\033[2K-- File: %.2f, Pending: %d; FreeRecv: %llu; FreeSend: %llu; TXQueue: %llu; HT_TX: %llu; PoolF: %llu", 
                            (float)(buffers.fstream[0].fsize - buffers.fstream[0].pending_bytes) / (float)buffers.fstream[0].fsize * 100.0, 
                            buffers.queue_fstream.pending,
                            buffers.pool_iocp_recv_context.free_blocks,
                            buffers.pool_iocp_send_context.free_blocks,
                            buffers.queue_tx_frame.pending,
                            buffers.htable_tx_frame.count,
                            buffers.pool_tx_frames.free_blocks
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
    SetEvent(client.hevent_connection_pending);

    UdpFrame *frame = s_pool_alloc(&buffers.pool_tx_frames);
    if(!frame){
        fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for connect request. Should never do since it has semaphore to block when full");
        return;
    }

    int res = construct_connect_request(frame,
                                        get_new_seq_num(), 
                                        client.sid, 
                                        client.cid, 
                                        client.flags, 
                                        client.client_name
                                        );
    if(res == RET_VAL_ERROR){
        fprintf(stderr, "CRITICAL ERROR: construct_connect_request() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
        return;
    }
    push_tx_frame(&buffers.queue_prio_tx_frame, (uintptr_t)frame);

    DWORD wait_connection_established = WaitForSingleObject(client.hevent_connection_established, CONNECT_REQUEST_TIMEOUT_MS);
    if (wait_connection_established == WAIT_OBJECT_0) {
        client.session_status = CONNECTION_ESTABLISHED;
        fprintf(stdout, "Connection established...\n");
    } else if (wait_connection_established == WAIT_TIMEOUT) {
        // ht_clean(&buffers.ht_frame);
        reset_client_session();
        fprintf(stderr, "Connection closed...\n");
    } else {
        // ht_clean(&buffers.ht_frame);
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

    UdpFrame *frame = s_pool_alloc(&buffers.pool_tx_frames);
    if(!frame){
        fprintf(stderr, "CRITICAL ERROR: s_pool_alloc() returned null pointer when allocating for disconnect request. Should never do since it has semaphore to block when full");
        return;
    }

    int res = construct_disconnect_request(frame, client.sid);
    if(res == RET_VAL_ERROR){
        fprintf(stderr, "CRITICAL ERROR: construct_disconnect_request() returned RET_VAL_ERROR. Should not happen since inputs are validated before calling");
        return;
    }
    push_tx_frame(&buffers.queue_prio_tx_frame, (uintptr_t)frame);
 
    DWORD wait_connection_closed = WaitForSingleObject(client.hevent_connection_closed, DISCONNECT_REQUEST_TIMEOUT_MS);

    if (wait_connection_closed == WAIT_OBJECT_0) {
        fprintf(stderr, "Connection closed\n"); 
    } else if (wait_connection_closed == WAIT_TIMEOUT) {
        fprintf(stdout, "Connection close timeout - closing connection anyway\n");
    } else {    
        fprintf(stderr, "Unexpected error for disconnect event: %lu\n", wait_connection_closed);
    }  
    // ht_clean(&buffers.ht_frame);
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
    fstream->fpath_len = 0;
    memset(&fstream->fpath, 0, MAX_PATH);
    fstream->rpath_len = 0;
    memset(&fstream->rpath, 0, MAX_PATH);
    fstream->fname_len = 0;
    memset(&fstream->fname, 0, MAX_PATH);
    memset(&fstream->fhash, 0, sizeof(FileHash));
    fstream->pending_bytes = 0;
    fstream->pending_metadata_seq_num = 0;
    memset(fstream->chunk_buffer, 0, FILE_CHUNK_SIZE);
    fstream->throttle = FALSE;
    fstream->fstream_busy = FALSE;
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









