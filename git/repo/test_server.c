
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

#include "include/server.h"             // For server data structures and definitions
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/netendians.h"         // For network byte order conversions
#include "include/checksum.h"           // For checksum validation
#include "include/sha256.h"
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management
#include "include/file_handler.h"       // For frame handling functions
#include "include/message_handler.h"    // For frame handling functions
#include "include/server_frames.h"
#include "include/server_statistics.h"

ServerData Server;
ServerBuffers Buffers;
ClientListData ClientList;

HANDLE hthread_recv_send_frame[SERVER_RECV_SEND_FRAME_WRK_THREADS];
HANDLE hthread_process_frame[SERVER_PROCESS_FRAME_WRK_THREAS];
HANDLE hthread_ack_frame;
HANDLE hthread_send_priority_ack;
HANDLE hthread_send_message_ack[SERVER_ACK_MESSAGE_FRAMES_THREADS];
HANDLE hthread_send_file_ack[SERVER_ACK_FILE_FRAMES_THREADS];
HANDLE hthread_send_file_ack_frame[SERVER_ACK_FILE_FRAMES_THREADS];
HANDLE hthread_client_timeout;
HANDLE hthread_file_stream[MAX_SERVER_ACTIVE_FSTREAMS];
HANDLE hthread_server_command;

const char *server_ip = "10.10.10.1";

// Thread functions
DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam);
DWORD WINAPI func_thread_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_send_priority_ack(LPVOID lpParam);
DWORD WINAPI thread_proc_send_message_ack(LPVOID lpParam);
DWORD WINAPI thread_proc_send_file_ack_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_client_timeout(LPVOID lpParam);
DWORD WINAPI thread_proc_file_stream(LPVOID lpParam);
DWORD WINAPI thread_proc_server_command(LPVOID lpParam);

static void get_network_config(){
    DWORD bufferSize = 0;
    IP_ADAPTER_ADDRESSES *adapterAddresses = NULL, *adapter = NULL;

    // Get required buffer size
    GetAdaptersAddresses(AF_INET, 0, NULL, adapterAddresses, &bufferSize);
    adapterAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufferSize);

    if (GetAdaptersAddresses(AF_INET, 0, NULL, adapterAddresses, &bufferSize) == NO_ERROR) {
        for (adapter = adapterAddresses; adapter; adapter = adapter->Next) {
            fprintf(stdout, "Adapter: %ls\n", adapter->FriendlyName);
            IP_ADAPTER_UNICAST_ADDRESS *address = adapter->FirstUnicastAddress;
            while (address) {
                SOCKADDR_IN *sockaddr = (SOCKADDR_IN *)address->Address.lpSockaddr;
                printf("IP Address: %s\n", inet_ntoa(sockaddr->sin_addr));
                address = address->Next;
            }
        }
    } else {
        printf("Failed to retrieve adapter information.\n");
    }

    free(adapterAddresses);
    return;
}

static int init_server_session(){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    memset(&client_list, 0, sizeof(ClientListData));

    server->server_status = STATUS_NONE;
    server->session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    server->session_id_counter = 0;
    snprintf(server->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, SERVER_NAME);

    return RET_VAL_SUCCESS;
}
static int init_server_config(){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    WSADATA wsaData;

    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }

    server->socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (server->socket == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        closesocket(server->socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_port = _htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server->server_addr.sin_addr);
 
    if (bind(server->socket, (SOCKADDR*)&server->server_addr, sizeof(server->server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server->socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);


    server->iocp_handle = CreateIoCompletionPort((HANDLE)server->socket, NULL, 0, 0);
    if (server->iocp_handle == NULL || server->iocp_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateIoCompletionPort failed: %d\n", GetLastError());
        closesocket(server->socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    return RET_VAL_SUCCESS;

}
static int init_server_buffers(){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    init_queue_frame(queue_frame, SERVER_SIZE_QUEUE_FRAME);
    init_queue_frame(queue_priority_frame, SERVER_SIZE_QUEUE_PRIORITY_FRAME);
    init_queue_ack(mqueue_ack, SERVER_SIZE_MQUEUE_ACK);
    init_queue_ack(queue_priority_ack, SERVER_SIZE_QUEUE_PRIORITY_ACK);
    init_queue_fstream(queue_fstream, MAX_SERVER_ACTIVE_FSTREAMS);
    init_ht_id(ht_fid);
    init_ht_id(ht_mid);
    init_pool(pool_file_chunk, BLOCK_SIZE_CHUNK, BLOCK_COUNT_CHUNK);
    init_pool(pool_iocp_send_context, sizeof(IOCP_CONTEXT), IOCP_SEND_MEM_POOL_BLOCKS);
    init_pool(pool_iocp_recv_context, sizeof(IOCP_CONTEXT), IOCP_RECV_MEM_POOL_BLOCKS);

    init_pool(pool_queue_ack_udp_frame, sizeof(PoolEntryAckFrame), 32768);
    init_queue_ack_frame(queue_ack_udp_frame, 32768);

    for(int i = 0; i < MAX_CLIENTS; i++){
        InitializeCriticalSection(&client_list->client[i].lock);
        for(int j = 0; j < MAX_SERVER_ACTIVE_MSTREAMS; j++){
            InitializeCriticalSection(&client_list->client[i].mstream[j].lock);
        }
    }
    InitializeCriticalSection(&client_list->lock);

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        InitializeCriticalSection(&server->fstream[i].lock);
    }
    InitializeCriticalSection(&server->fstreams_lock);

    server->server_status = STATUS_READY;

    for (int i = 0; i < IOCP_RECV_MEM_POOL_BLOCKS; ++i) {
        IOCP_CONTEXT* recv_context = (IOCP_CONTEXT*)pool_alloc(pool_iocp_recv_context);
        if (recv_context == NULL) {
            fprintf(stderr, "Failed to allocate receive context from pool %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
        init_iocp_context(recv_context, OP_RECV); // Initialize the context

        if (udp_recv_from(server->socket, recv_context) == RET_VAL_ERROR) {
            fprintf(stderr, "Failed to post initial receive operation %d. Exiting.\n", i);
            return RET_VAL_ERROR;
        }
    }
    printf("Server: All initial receive operations posted.\n");

    return RET_VAL_SUCCESS;

}
static int start_threads() {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    // Create threads for receiving and processing frames
    for(int i = 0; i < SERVER_RECV_SEND_FRAME_WRK_THREADS; i++){
        hthread_recv_send_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_recv_send_frame, NULL, 0, NULL);
        if (hthread_recv_send_frame[i] == NULL) {
            fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    for(int i = 0; i < SERVER_PROCESS_FRAME_WRK_THREAS; i++){
        hthread_process_frame[i] = (HANDLE)_beginthreadex(NULL, 0, func_thread_process_frame, NULL, 0, NULL);
        if (hthread_process_frame[i] == NULL) {
            fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }

    hthread_send_priority_ack = (HANDLE)_beginthreadex(NULL, 0, thread_proc_send_priority_ack, NULL, 0, NULL);
    if (hthread_send_priority_ack == NULL) {
        fprintf(stderr, "Failed to create send ack thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    for(int i = 0; i < SERVER_ACK_MESSAGE_FRAMES_THREADS; i++){
        hthread_send_message_ack[i] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_send_message_ack, NULL, 0, NULL);
        if (hthread_send_message_ack[i] == NULL) {
            fprintf(stderr, "Failed to create send ack thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }
    // for(int i = 0; i < SERVER_ACK_FILE_FRAMES_THREADS; i++){
    //     hthread_send_file_ack[i] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_send_file_ack, NULL, 0, NULL);
    //     if (hthread_send_file_ack[i] == NULL) {
    //         fprintf(stderr, "Failed to create send ack thread. Error: %d\n", GetLastError());
    //         return RET_VAL_ERROR;
    //     }
    // }
    for(int i = 0; i < SERVER_ACK_FILE_FRAMES_THREADS; i++){
        hthread_send_file_ack_frame[i] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_send_file_ack_frame, NULL, 0, NULL);
        if (hthread_send_file_ack_frame[i] == NULL) {
            fprintf(stderr, "Failed to create send ack frame thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }


    hthread_client_timeout = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_timeout, &client_list, 0, NULL);
    if (hthread_client_timeout == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        hthread_file_stream[i] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_file_stream, NULL, 0, NULL);
        if (hthread_file_stream[i] == NULL) {
            fprintf(stderr, "Failed to file stream thread. Error: %d\n", GetLastError());
            return RET_VAL_ERROR;
        }
    }

    hthread_server_command = (HANDLE)_beginthreadex(NULL, 0, thread_proc_server_command, NULL, 0, NULL);
    if (hthread_server_command == NULL) {
        fprintf(stderr, "Failed to create server command thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    server->server_status = STATUS_READY;
    return RET_VAL_SUCCESS;
}
static void shutdown_server() {
    printf("Server shut down!\n");
}

// Find client by session ID
static Client* find_client(const uint32_t session_id) {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    // Search for a client within the provided ClientList that matches the given session ID.
    EnterCriticalSection(&client_list->lock);
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        Client *client = &client_list->client[slot];
        if(client->slot_status == SLOT_FREE){
            continue; // Move to the next slot in the loop.
        }
        if(client->sid == session_id){
            LeaveCriticalSection(&client_list->lock);
            return client;
        }
    }
    LeaveCriticalSection(&client_list->lock);
    return NULL;
}
// Add a new client
static Client* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
       
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    uint32_t free_slot = 0;

    EnterCriticalSection(&client_list->lock);
    while(free_slot < MAX_CLIENTS){
        if(client_list->client[free_slot].slot_status == SLOT_FREE) {
            break;
        }
        free_slot++;
    }

    if(free_slot >= MAX_CLIENTS){
        fprintf(stderr, "\nMax clients reached. Cannot add new client.\n");
        LeaveCriticalSection(&client_list->lock);
        return NULL;
    }
   
    Client *new_client = &client_list->client[free_slot]; 
    
    EnterCriticalSection(&new_client->lock);

    new_client->slot = free_slot;
    new_client->slot_status = SLOT_BUSY;
    new_client->srv_socket = server->socket;
    memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
    new_client->sid = InterlockedIncrement(&server->session_id_counter);
    new_client->flags = recv_frame->payload.connection_request.flags;

    snprintf(new_client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    new_client->port = _ntohs(client_addr->sin_port);

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->sid);

    LeaveCriticalSection(&new_client->lock);
    LeaveCriticalSection(&client_list->lock);
    return new_client;
}
// Remove a client
static int remove_client(const uint32_t slot) {
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    if(client_list == NULL){
        fprintf(stderr, "\nInvalid client pointer!\n");
        return RET_VAL_ERROR;
    }
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "\nInvalid client slot nr:  %d", slot); 
        return RET_VAL_ERROR;
    }
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", client_list->client[slot].sid, client_list->client[slot].slot);   
    
    EnterCriticalSection(&client_list->lock);    
    cleanup_client(&client_list->client[slot]);
    LeaveCriticalSection(&client_list->lock);

    return RET_VAL_SUCCESS;
}
// Release client resources
static void cleanup_client(Client *client){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    if(client == NULL){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }
    EnterCriticalSection(&client->lock);

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &server->fstream[i];
        if(fstream->sid ==  client->sid){
            close_file_stream(fstream);
        }
    }

    ht_remove_all_sid(ht_fid, client->sid);

    for(int i = 0; i < MAX_SERVER_ACTIVE_MSTREAMS; i++){
        EnterCriticalSection(&client->mstream[i].lock);
        close_message_stream(&client->mstream[i]);
        LeaveCriticalSection(&client->mstream[i].lock);
    }

    memset(&client->client_addr, 0, sizeof(struct sockaddr_in));
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    client->port = 0;
    client->cid = 0;
    memset(&client->name, 0, MAX_NAME_SIZE);
    client->flags = 0;
    client->connection_status = CLIENT_DISCONNECTED;
    client->last_activity_time = time(NULL);
    client->slot = 0;
    client->slot_status = SLOT_FREE;
    LeaveCriticalSection(&client->lock);
    return;
}
// Compare received hash with calculated hash
static BOOL validate_file_hash(ServerFileStream *fstream){

    // fprintf(stdout, "File hash received: ");
    // for(int i = 0; i < 32; i++){
    //     fprintf(stdout, "%02x", (uint8_t)fstream->received_sha256[i]);
    // }
    // fprintf(stdout, "\n");
    // fprintf(stdout, "File hash calculated: ");
    // for(int i = 0; i < 32; i++){
    //     fprintf(stdout, "%02x", (uint8_t)fstream->calculated_sha256[i]);
    // }
    // fprintf(stdout, "\n");

    for(int i = 0; i < 32; i++){
        if((uint8_t)fstream->calculated_sha256[i] != (uint8_t)fstream->received_sha256[i]){
            fprintf(stdout, "ERROR: SHA256 MISSMATCH\n");
            return FALSE;
        }
    }

    return TRUE;
}
// Check for any open file streams across all clients.
static void check_open_file_stream(){
    
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    for(int i = 0; i < MAX_SERVER_ACTIVE_FSTREAMS; i++){
        if(server->fstream[i].fstream_busy == TRUE){
            fprintf(stdout, "File stream still open: %d\n", i);
        }
    }
    fprintf(stdout, "Completed checking opened file streams\n");
    return;
}
// --- Receive frame thread function ---
DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)
    
    HANDLE CompletitionPort = server->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;
    char ip_string_buffer[INET_ADDRSTRLEN];

    QueueFrameEntry frame_entry;
    
    while (server->server_status == STATUS_READY) {

        BOOL getqcompl_result = GetQueuedCompletionStatus(
            CompletitionPort,
            &NrOfBytesTransferred,
            &lpCompletitionKey,
            &lpOverlapped,
            INFINITE// WSARECV_TIMEOUT_MS
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
                    pool_free(pool_iocp_send_context, context);
                } else if (context->type == OP_RECV) {
                    // Critical error on a receive context -"retire" this context from the pool.
                    fprintf(stderr, "Server: Error in RECV operation, attempting re-post context %p...\n", (void*)context);
                    pool_free(pool_iocp_recv_context, context);
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
                    BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                                    frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                                    frame_type == FRAME_TYPE_FILE_METADATA ||
                                                    frame_type == FRAME_TYPE_DISCONNECT);

                    if (is_high_priority_frame == TRUE) {
                        if (push_frame(queue_priority_frame, &frame_entry) == RET_VAL_ERROR) {
                            Sleep(100);
                            continue;
                        }
                    } else {
                        if (push_frame(queue_frame, &frame_entry) == RET_VAL_ERROR) {
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
                    fprintf(stdout, "Server: Receive operation completed with 0 bytes for context %p. Re-posting.\n", (void*)context);
                }

                // *** CRITICAL: Re-post the receive operation using the SAME context ***
                // This ensures the buffer is continuously available for incoming data.
                if (udp_recv_from(server->socket, context) == RET_VAL_ERROR){
                    fprintf(stderr, "Critical: WSARecvFrom re-issue failed for context %p: %d. Freeing.\n", (void*)context, WSAGetLastError());
                    // This is a severe problem. Retire the context from the pool.
                    pool_free(pool_iocp_recv_context, context); // Return to pool if it fails
                }
                // refill the recv context mem pool when it drops bellow half
                if(pool_iocp_recv_context->free_blocks > (pool_iocp_recv_context->block_count / 2)){
                    refill_recv_iocp_pool(server->socket, pool_iocp_recv_context);
                }
                break; // End of OP_RECV case

            case OP_SEND:
                // For send completions, simply free the context
                if (NrOfBytesTransferred > 0) {
                    // if (inet_ntop(AF_INET, &(context->addr.sin_addr), ip_string_buffer, INET_ADDRSTRLEN) == NULL) {
                    //     strcpy(ip_string_buffer, "UNKNOWN_IP");
                    // }
                    // printf("Server: Sent %lu bytes to %s:%d (Message: '%s')\n",
                    //        NrOfBytesTransferred, ip_string_buffer, ntohs(context->addr.sin_port), context->buffer);
                } else {
                    fprintf(stderr, "Server: Send operation completed with 0 bytes or error.\n");
                }
                pool_free(pool_iocp_send_context, context);
                break;

            default:
                fprintf(stderr, "Server: Unknown operation type in completion.\n");
                // Free context if it's unknown and shouldn't be re-used, to prevent leak
                pool_free(pool_iocp_send_context, context);
                break;
        } // end of switch(context->type)
    } // end of while (server->server_status == STATUS_READY)
           
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}
// --- Processes a received frame ---
DWORD WINAPI func_thread_process_frame(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    uint16_t recv_delimiter;      // Stores the extracted start delimiter from the frame header.
    uint8_t  recv_frame_type;     // Stores the extracted frame type from the frame header.
    uint64_t recv_seq_num;        // Stores the extracted sequence number from the frame header.
    uint32_t recv_session_id;     // Stores the extracted session ID from the frame header.

    QueueFrameEntry frame_entry;    // A structure to temporarily hold a frame popped from a queue, along with its source address and size.
    QueueAckEntry ack_entry;        // A structure to hold details for an ACK/NAK to be sent (declared but not directly used in this snippet's logic).

    UdpFrame *frame;                // A pointer to the UDP frame data within frame_entry.
    struct sockaddr_in *src_addr;   // A pointer to the source address of the received UDP frame.
    uint32_t frame_bytes_received;  // The actual number of bytes received for the current UDP frame.

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    HANDLE events[2] = {queue_priority_frame->push_semaphore,
                        queue_frame->push_semaphore,
                        };

    while(server->server_status == STATUS_READY) {
        memset(&frame_entry, 0, sizeof(QueueFrameEntry));
        DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            if (pop_frame(queue_priority_frame, &frame_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from frame priority queue RET_VAL_ERROR\n");
                continue;
            }
        } else if (result == WAIT_OBJECT_0 + 1) {
            if (pop_frame(queue_frame, &frame_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from frame queue RET_VAL_ERROR\n");
                continue;
            }
        } else {
            fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Unexpected result wait semaphore frame queues: %lu\n", result);
            continue;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        recv_delimiter = _ntohs(frame->header.start_delimiter);
        recv_frame_type = frame->header.frame_type;
        recv_seq_num = _ntohll(frame->header.seq_num);
        recv_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (recv_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, recv_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            continue;
        }

        Client *client = NULL;
        time_t now = time(NULL);

        if(recv_frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                                recv_session_id == DEFAULT_CONNECT_REQUEST_SID &&
                                recv_seq_num == DEFAULT_CONNECT_REQUEST_SEQ){
            client = add_client(frame, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
                continue;
            }
            EnterCriticalSection(&client->lock);
            client->last_activity_time = now;
            LeaveCriticalSection(&client->lock);            
            send_connect_response(DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, 
                                    &client->client_addr,
                                    pool_iocp_send_context
                                );
            fprintf(stdout, "Client %s:%d Requested connection. Responding to connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;

        } else if (recv_frame_type == FRAME_TYPE_CONNECT_REQUEST && 
                        (recv_session_id != DEFAULT_CONNECT_REQUEST_SID || recv_seq_num != DEFAULT_CONNECT_REQUEST_SEQ)) {
            // fprintf(stdout, "DEBUG: received connect request. SID: %lx; seq: %llx\n", recv_session_id, recv_seq_num);
            client = find_client(recv_session_id);
            if(client == NULL){
                fprintf(stderr, "Unknown client tried to re-connect\n");
                continue;
            }
            EnterCriticalSection(&client->lock);
            client->last_activity_time = now;
            LeaveCriticalSection(&client->lock);
            send_connect_response(DEFAULT_CONNECT_REQUEST_SEQ, 
                                    client->sid, 
                                    server->session_timeout, 
                                    server->server_status, 
                                    server->name, 
                                    server->socket, 
                                    &client->client_addr,
                                    pool_iocp_send_context
                                );
            fprintf(stdout, "Client %s:%d Requested re-connection. Responding to re-connect request with Session ID: %u\n", 
                                                    client->ip, client->port, client->sid);
            continue;
        } else {
            client = find_client(recv_session_id);
            if(client == NULL){
                //fprintf(stderr, "Received frame from unknown client\n");
                continue;
            }
        }

        switch (recv_frame_type) {

            case FRAME_TYPE_ACK:
                EnterCriticalSection(&client->lock);
                client->last_activity_time = now;
                LeaveCriticalSection(&client->lock);
                // TODO: Implement the full ACK processing logic here. This typically involves:
                //   - Removing acknowledged packets from the sender's retransmission queue.
                //   - Updating window sizes for flow and congestion control.
                //   - Advancing sequence numbers to indicate successfully received data.
                break;

            case FRAME_TYPE_KEEP_ALIVE:
                EnterCriticalSection(&client->lock);
                client->last_activity_time = now;
                LeaveCriticalSection(&client->lock);

                new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_KEEP_ALIVE, server->socket, &client->client_addr);
                push_ack(queue_priority_ack, &ack_entry);

                break;

            case FRAME_TYPE_FILE_METADATA:
                handle_file_metadata(client, frame);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                handle_file_fragment(client, frame);
                break;

            case FRAME_TYPE_FILE_END:
                handle_file_end(client, frame);
                break;

            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                handle_message_fragment(client, frame);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "Client %s:%d with session ID: %d requested disconnect...\n", client->ip, client->port, client->sid);
                new_ack_entry(&ack_entry, recv_seq_num, recv_session_id, STS_CONFIRM_DISCONNECT, server->socket, &client->client_addr);
                push_ack(queue_priority_ack, &ack_entry);
                remove_client(client->slot);
                break;

            default:
                fprintf(stderr, "Received unknown frame type: %u from %s:%d (Session ID: %u). Discarding.\n",
                        recv_frame_type, src_ip, src_port, recv_session_id);
                break;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Ack Thread functions ---
DWORD WINAPI thread_proc_send_priority_ack(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    QueueAckEntry ack_entry;

    while (server->server_status == STATUS_READY) {
        // DWORD result = WaitForMultipleObjects(3, events, FALSE, INFINITE);
        DWORD result = WaitForSingleObject(queue_priority_ack->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            if (pop_ack(queue_priority_ack, &ack_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "CRITICAL ERROR: Popping from ack priority queue RET_VAL_ERROR\n");
                continue;
            }
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore priority ack queues: %lu\n", result);
            continue;
        }
        send_ack(ack_entry.seq, ack_entry.sid, ack_entry.op_code, ack_entry.src_socket, &ack_entry.dest_addr, pool_iocp_send_context);
    }
    _endthreadex(0);
    return 0;
}
DWORD WINAPI thread_proc_send_message_ack(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    QueueAckEntry ack_entry;

    while (server->server_status == STATUS_READY) {
        DWORD result = WaitForSingleObject(mqueue_ack->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            if (pop_ack(mqueue_ack, &ack_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "CRITICAL ERROR: Popping from message ack queue RET_VAL_ERROR\n");
                continue;
            }
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore message ack queues: %lu\n", result);
            continue;
        }
        send_ack(ack_entry.seq, ack_entry.sid, ack_entry.op_code, ack_entry.src_socket, &ack_entry.dest_addr, pool_iocp_send_context);
    }
    _endthreadex(0);
    return 0;
}
DWORD WINAPI thread_proc_send_file_ack_frame(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    while (server->server_status == STATUS_READY) {
        DWORD result = WaitForSingleObject(queue_ack_udp_frame->push_semaphore, INFINITE);
        PoolEntryAckFrame *entry = (PoolEntryAckFrame*)pop_ack_frame(queue_ack_udp_frame);
        if (result == WAIT_OBJECT_0) {
            if(!entry){
                fprintf(stderr,"CRITICAL ERROR: pop_ack_frame() null pointer from queue ack frame");
                continue;
            }
            send_pool_ack_frame(entry, pool_iocp_send_context);
            pool_free(pool_queue_ack_udp_frame, entry);
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore queue ack frame: %lu\n", result);
            continue;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Client timeout thread function ---
DWORD WINAPI thread_proc_client_timeout(LPVOID lpParam){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    time_t time_now;
    while(server->server_status == STATUS_READY) {
        time_now = time(NULL);
        for(int slot = 0; slot < MAX_CLIENTS; slot++){
            if(client_list->client[slot].slot_status == SLOT_FREE){
                continue; // Skip to the next client slot.
            }
            if(time_now - (time_t)client_list->client[slot].last_activity_time < (time_t)server->session_timeout){
                continue; // Skip to the next client slot.
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", client_list->client[slot].sid);
            remove_client(slot);
        }
        Sleep(1000);
    }

    // After the `while` loop condition (`server.status == SERVER_READY`) becomes false,
    _endthreadex(0);
    return 0; // Return 0 to indicate that the thread terminated successfully.
}
// --- Thread for writing file streams to disk ---
DWORD WINAPI thread_proc_file_stream(LPVOID lpParam) {

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    SHA256_CTX sha256_ctx;

    while (server->server_status == STATUS_READY) { 
        ServerFileStream *fstream = NULL;
        DWORD result = WaitForSingleObject(queue_fstream->push_semaphore, INFINITE);
        if (result == WAIT_OBJECT_0) {
            fstream = (ServerFileStream*)pop_fstream(queue_fstream);
        } else {
            fprintf(stderr, "CRITICAL ERROR: Unexpected result wait semaphore fstream queue: %lu\n", result);
            continue;
        }        
        if(!fstream){
            fprintf(stderr, "CRITICAL ERROR: Received null pointer from fstream queue!\n");
            continue;
        }

        sha256_init(&sha256_ctx);                              
        while(fstream->fstream_busy){
            EnterCriticalSection(&fstream->lock);
            // Iterate through all bitmap entries (chunks) for the current file stream.
            for(long long k = 0; k < fstream->bitmap_entries_count; k++){
                // Calculate the absolute file offset where this chunk should be written.
                uint64_t file_offset = k * FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK;
                // Get the memory buffer associated with this chunk.
                char* buffer = fstream->pool_block_file_chunk[k];
                uint64_t buffer_size;

                // Case 1: This is the trailing (last, potentially partial) chunk.
                // Check if:
                //   a) The fstream is marked as having a trailing chunk.
                //   b) The trailing chunk's expected bytes have all been received.
                //   c) The current chunk's flag indicates it's the trailing chunk AND it hasn't been written yet.
                if (fstream->trailing_chunk && fstream->trailing_chunk_complete && (fstream->flag[k] == CHUNK_TRAILING)) {
                    buffer_size = fstream->trailing_chunk_size; // Use the specific calculated size for the trailing chunk.

                // Case 2: This is a full-sized chunk (not trailing).
                // Check if:
                //   a) All fragments within this 64-bit bitmap entry have been received (~0ULL means all bits set).
                //   b) The current chunk's flag indicates it's a body chunk AND it hasn't been written yet.
                } else if(fstream->bitmap[k] == ~0ULL && (fstream->flag[k] == CHUNK_BODY)) {
                    buffer_size = FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK; // Full chunk size.
                }
                // Case 3: This chunk is neither a ready trailing chunk nor a complete, unwritten full chunk.
                // It means this chunk either:
                //   - Has not received all its data yet.
                //   - Has already been written to disk.
                //   - Is not the trailing chunk and not a full chunk (logic error in flag setting perhaps).
                else {
                    continue; // Skip this chunk and move to the next bitmap entry.
                }

                // --- FILE WRITE OPERATIONS ---
                // Check if the file pointer is valid. If NULL, something went wrong during file opening.
                if (fstream->fp == NULL) {
                    fprintf(stderr, "Error: FILE pointer is null for chunk %llu. Session ID: %u, File ID: %u\n", k, fstream->sid, fstream->fid);
                    fstream->fstream_err = STREAM_ERR_FP; // Set a specific error code.
                    break;
                }

                // Attempt to seek to the correct offset in the file.
                if (_fseeki64(fstream->fp, file_offset, SEEK_SET) != 0) {
                    fprintf(stderr, "Error: Failed to seek to offset %llu for chunk %llu. Session ID: %u, File ID: %u\n", file_offset, k, fstream->sid, fstream->fid);
                    fstream->fstream_err = STREAM_ERR_FSEEK; // Set a specific error code.
                    break;
                }

                // Write the chunk data from the buffer to the file.
                size_t written = fwrite(buffer, 1, buffer_size, fstream->fp);
                // Check if the number of bytes written matches the expected buffer size.
                if (written != buffer_size) {
                    fprintf(stderr, "Error: Failed to write data (expected %llu, wrote %llu) for chunk %llu. Session ID: %u, File ID: %u\n", buffer_size, written, k, fstream->sid, fstream->fid);
                    fstream->fstream_err = STREAM_ERR_FWRITE; // Set a specific error code.
                    break;
                }
                fstream->written_bytes_count += written; // Accumulate the total bytes written to disk.
                fstream->flag[k] |= CHUNK_WRITTEN; // Update the chunk's flag to reflect it has been written.

                // Return the memory buffer for this chunk back to the pre-allocated pool.
                pool_free(pool_file_chunk, fstream->pool_block_file_chunk[k]);
                fstream->pool_block_file_chunk[k] = NULL;
            }

            if(fstream->fstream_err != STREAM_ERR_NONE){
                close_file_stream(fstream); // Clean up the entire file stream.
                LeaveCriticalSection(&fstream->lock);
                break;
            }

            for(long long l = 0; l < fstream->bitmap_entries_count; l++){
                BOOL is_chunk_body = (fstream->flag[l] & CHUNK_BODY) == CHUNK_BODY;
                BOOL is_chunk_body_complete = fstream->bitmap[l] == ~0ULL;
                BOOL is_chunk_written = (fstream->flag[l] & CHUNK_WRITTEN) == CHUNK_WRITTEN;

                BOOL is_chunk_body_and_written = fstream->flag[l] == (CHUNK_BODY | CHUNK_WRITTEN);
                BOOL is_chunk_trailing_and_written = fstream->flag[l] == (CHUNK_TRAILING | CHUNK_WRITTEN);

                if(is_chunk_body && !is_chunk_body_complete){
                    break;  // exit the for loop
                }

                if(!is_chunk_written){
                    break; // exit the for loop
                }

                if(is_chunk_body_and_written || is_chunk_trailing_and_written){
                    uint64_t file_offset = l * FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK;
                    // Attempt to seek to the correct offset in the file.
                    if (_fseeki64(fstream->fp, file_offset, SEEK_SET) != 0) {
                        fprintf(stderr, "Error: Failed to seek to offset %llu for chunk %llu. Session ID: %u, File ID: %u\n", file_offset, l, fstream->sid, fstream->fid);
                        fstream->fstream_err = STREAM_ERR_FSEEK; // Set a specific error code.
                        break;
                    }

                    char chunk_buffer[(FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)];
                    size_t chunk_bytes_read = fread(chunk_buffer, 1, sizeof(chunk_buffer), fstream->fp);
                    
                    if (chunk_bytes_read <= 0) {
                        fprintf(stderr, "Error: Failed to read data from file for chunk offset %llu. Session ID: %u, File ID: %u\n", file_offset, fstream->sid, fstream->fid);
                        fstream->fstream_err = STREAM_ERR_FREAD; // Set a specific error code.
                        break;
                    }                              
                    
                    sha256_update(&sha256_ctx, chunk_buffer, chunk_bytes_read);

                    fstream->flag[l] |= CHUNK_HASHED;
                    fstream->hashed_chunks_count++;
                }
            }

            if(fstream->fstream_err != STREAM_ERR_NONE){
                close_file_stream(fstream); // Clean up the entire file stream.
                LeaveCriticalSection(&fstream->lock);
                break;
            }

            fstream->file_bytes_received = (fstream->recv_bytes_count == fstream->fsize);
            fstream->file_bytes_written = (fstream->hashed_chunks_count == fstream->bitmap_entries_count);

            // After attempting to write all available chunks for this file stream:
            // Check if the total bytes received equals the total file size AND
            // if the total bytes written to disk also equals the total file size.
            if(fstream->file_bytes_received && fstream->file_bytes_written &&
                    (fstream->hashed_chunks_count == fstream->bitmap_entries_count) &&
                    !fstream->file_hash_calculated){
                sha256_final(&sha256_ctx, (uint8_t *)&fstream->calculated_sha256);
                fstream->file_hash_calculated = TRUE;
            }
            
            if(fstream->file_hash_calculated && fstream->file_hash_received && !fstream->file_hash_validated){
                fstream->file_hash_validated = validate_file_hash(fstream);
                if(!fstream->file_hash_validated){
                    fprintf(stderr, "STREAM ERROR: sha256 mismatch!\n");
                    fstream->fstream_err = STREAM_ERR_SHA256_MISMATCH; // Set a specific error code.                        
                    close_file_stream(fstream);
                    LeaveCriticalSection(&fstream->lock);
                    break;  //exit the while loop
                }                                                    
            }

            fstream->file_complete = (fstream->file_bytes_received && 
                                        fstream->file_bytes_written &&
                                        fstream->file_hash_received &&
                                        fstream->file_hash_calculated &&
                                        fstream->file_hash_validated &&
                                        !fstream->file_complete
                                        );

            // If the file is now completely received and written:
            if(fstream->file_complete){           
                
                ht_update_id_status(ht_fid, fstream->sid, fstream->fid, ID_RECV_COMPLETE);
                // fprintf(stdout, "Receved file: '%s'\n", fstream->fname);

                QueueAckEntry ack_entry;
                new_ack_entry(&ack_entry, fstream->file_end_frame_seq_num, fstream->sid, STS_CONFIRM_FILE_END, server->socket, &fstream->client_addr);
                push_ack(queue_priority_ack, &ack_entry);

                close_file_stream(fstream);
                LeaveCriticalSection(&fstream->lock);
                break; //exit the while loop
            }
            
            LeaveCriticalSection(&fstream->lock);
            Sleep(1);
        } // end of while(fstream->busy)
    }
    //fprintf(stdout,"stream successfully closed %d\n", fstream->fstream_index);
    _endthreadex(0);
    return 0;
}
// --- Process server command ---
DWORD WINAPI thread_proc_server_command(LPVOID lpParam){
    
    char cmd;
    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)
    while (server->server_status == STATUS_READY){

        fprintf(stdout,"Waiting for command...\n");
        cmd = getchar();
        switch(cmd) {
            case 's':
            case 'S':
            //check what file streams are still open
                check_open_file_stream();
                break;

            case 'l':
            case 'L':
                refill_recv_iocp_pool(server->socket, pool_iocp_recv_context);
                break;

            default:
                break;
            }
        Sleep(200);
    }

    _endthreadex(0);
    return 0;
}

// Clean up the file stream resources after a file transfer is completed or aborted.
void close_file_stream(ServerFileStream *fstream){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    if(fstream == NULL){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer file stream\n");
        return;
    }

    EnterCriticalSection(&fstream->lock);

    if(fstream->fp && fstream->fstream_busy && !fstream->file_complete){
        fclose(fstream->fp);
        remove(fstream->fpath);
        fstream->fp = NULL;
    }
    if(fstream->fp != NULL){
        if(fflush(fstream->fp) != 0){
            fprintf(stderr, "Error flushing the file to disk. File is still in use.\n");
        } else {
            int fclosed = fclose(fstream->fp);
            Sleep(50); // Sleep for 50 milliseconds to ensure the file is properly closed before proceeding.
            if(fclosed != 0){
                fprintf(stderr, "Error closing the file stream: %s (errno: %d)\n", fstream->fpath, errno);
            }
            fstream->fp = NULL; // Set the file pointer to NULL after closing.
        }
    }
    if(fstream->bitmap != NULL){
        free(fstream->bitmap);
        fstream->bitmap = NULL;
    }
    if(fstream->flag != NULL){
        free(fstream->flag);
        fstream->flag = NULL;
    }    
    for(long long k = 0; k < fstream->bitmap_entries_count; k++){
        if(fstream->pool_block_file_chunk[k] != NULL){
            pool_free(pool_file_chunk, fstream->pool_block_file_chunk[k]);
        }
        fstream->pool_block_file_chunk[k] = NULL;
    }

    fstream->sid = 0;                                   // Session ID associated with this file stream.
    fstream->fid = 0;                                   // File ID, unique identifier for the file associated with this file stream.
    fstream->fsize = 0;                                 // Total size of the file being transferred.
    fstream->trailing_chunk = FALSE;                // True if the last bitmap entry represents a partial chunk (less than 64 fragments).
    fstream->trailing_chunk_complete = FALSE;       // True if all bytes for the last, potentially partial, chunk have been received.
    fstream->trailing_chunk_size = 0;               // The actual size of the last chunk (if partial).
    fstream->file_end_frame_seq_num = 0;
    fstream->fragment_count = 0;                    // Total number of fragments in the entire file.
    fstream->recv_bytes_count = 0;                  // Total bytes received for this file so far.
    fstream->written_bytes_count = 0;               // Total bytes written to disk for this file so far.
    fstream->bitmap_entries_count = 0;              // Number of uint64_t entries in the bitmap array.
    fstream->hashed_chunks_count = 0;
 
    fstream->fstream_err = STREAM_ERR_NONE;         // Stores an error code if something goes wrong with the stream.
    fstream->file_complete = FALSE;                 // True if the entire file has been received and written.
    fstream->file_bytes_received = FALSE;
    fstream->file_bytes_written = FALSE;
    fstream->file_hash_received = FALSE;
    fstream->file_hash_calculated = FALSE;
    fstream->file_hash_validated = FALSE;

    memset(&fstream->received_sha256, 0, 32);
    memset(&fstream->calculated_sha256, 0, 32);
    fstream->rpath_len = 0;
    memset(&fstream->rpath, 0, MAX_PATH);
    fstream->fname_len = 0;
    memset(&fstream->fname, 0, MAX_PATH);
    memset(&fstream->fpath, 0, MAX_PATH);
    
    fstream->fstream_busy = FALSE;                  // Indicates if this stream channel is currently in use for a transfer.

    LeaveCriticalSection(&fstream->lock);
    return;
}
// Clean up the message stream resources after a file transfer is completed or aborted.
void close_message_stream(MessageStream *mstream){

    PARSE_SERVER_GLOBAL_DATA(Server, ClientList, Buffers) // this macro is defined in server header file (server.h)

    if(mstream == NULL){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer message stream\n");
        return;
    }
    if(mstream->buffer){
        free(mstream->buffer);
        mstream->buffer = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    if(mstream->bitmap != NULL){
        free(mstream->bitmap); // Free the memory allocated for the bitmap.
        mstream->bitmap = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    mstream->busy = FALSE; // Reset the busy flag.
    mstream->stream_err = STREAM_ERR_NONE; // Reset error status.
    mstream->sid = 0; // Reset session ID.
    mstream->mid = 0; // Reset message ID.
    mstream->mlen = 0; // Reset message length.
    mstream->fragment_count = 0; // Reset fragment count.
    mstream->chars_received = 0; // Reset characters received counter.
    mstream->bitmap_entries_count = 0; // Reset bitmap entries count.
    memset(mstream->fnm, 0, MAX_NAME_SIZE); // Clear the file name buffer by filling it with zeros.
    return;

}
// Clean client resources

int main() {
    //get_network_config();
    init_server_session();
    init_server_config();
    init_server_buffers();
    start_threads();
    init_server_statistics_gui();
    // Main server loop for general management, timeouts, and state updates
    while (Server.server_status == STATUS_READY) {

        Sleep(250); // Prevent busy-waiting
   
        // fprintf(stdout, "\r\033[2K-- Pending: %d; FreeSend: %llu; FreeRecv: %llu; FreeAckFrame: %llu", 
        //                     buffers.queue_fstream.pending,
        //                     buffers.pool_iocp_send_context.free_blocks,
        //                     buffers.pool_iocp_recv_context.free_blocks,
        //                     buffers.pool_queue_ack_udp_frame.free_blocks
        //                     );

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}
















// update file transfer progress and speed in MBs
void update_statistics(Client * client){

    // //update file transfer speed in MB/s
    // GetSystemTimePreciseAsFileTime(&client->statistics.ft);
    // client->statistics.crt_uli.LowPart = client->statistics.ft.dwLowDateTime;
    // client->statistics.crt_uli.HighPart = client->statistics.ft.dwHighDateTime;
    // client->statistics.crt_microseconds = client->statistics.crt_uli.QuadPart / 10;

    // client->statistics.crt_bytes_received = (float)client->fstream[0].recv_bytes_count;

    // //TRANSFER SPEED
    // //current speed (1 cycle)
    // client->statistics.file_transfer_speed = (client->statistics.crt_bytes_received - client->statistics.prev_bytes_received) / (float)((client->statistics.crt_microseconds - client->statistics.prev_microseconds));
    // client->statistics.prev_bytes_received = client->statistics.crt_bytes_received;
    // client->statistics.prev_microseconds = client->statistics.crt_microseconds;
    // //PROGRESS - update file transfer progress percentage
    // client->statistics.file_transfer_progress = (float)client->fstream[0].recv_bytes_count / (float)client->fstream[0].fsize * 100.0;

    // fprintf(stdout, "\rFile transfer progress: %.2f %% - Speed: %.2f MB/s", client->statistics.file_transfer_progress, client->statistics.file_transfer_speed);
    // fflush(stdout);
}

// DWORD WINAPI thread_proc_send_ack(LPVOID lpParam){

//     QueueAckEntry ack_entry;
//     HANDLE events[3] = {buffers.queue_priority_ack.push_semaphore, 
//                                     buffers.mqueue_ack.push_semaphore, 
//                                     buffers.fqueue_ack.push_semaphore
//                                     };

//     while (server.server_status == STATUS_READY) {
//         DWORD result = WaitForMultipleObjects(3, events, FALSE, INFINITE);
//         if (result == WAIT_OBJECT_0) {
//             if (pop_ack(&buffers.queue_priority_ack, &ack_entry) == RET_VAL_ERROR) {
//                 fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from ack priority queue RET_VAL_ERROR\n");
//                 continue;
//             }
//         } else if (result == WAIT_OBJECT_0 + 1) {
//             if (pop_ack(&buffers.mqueue_ack, &ack_entry) == RET_VAL_ERROR) {
//                 fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from ack mqueue RET_VAL_ERROR\n");
//                 continue;
//             }
//         } else if (result == WAIT_OBJECT_0 + 2) {
//             if (pop_ack(&buffers.fqueue_ack, &ack_entry) == RET_VAL_ERROR) {
//                 fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Popping from ack fqueue RET_VAL_ERROR\n");
//                 continue;
//             }
//         } else {
//             fprintf(stderr, "ERROR SHOULD NOT HAPPEN: Unexpected result wait semaphore ack queues: %lu\n", result);
//             continue;
//         }
//         send_ack(ack_entry.seq, ack_entry.sid, ack_entry.op_code, ack_entry.src_socket, &ack_entry.dest_addr);
//     }
//     _endthreadex(0);
//     return 0;
// }





// Create output file
int create_output_file(const char *buffer, const uint64_t size, const char *path){
    FILE *fp = fopen(path, "wb");           
    if(fp == NULL){
        fprintf(stderr, "Error creating output file!!!\n");
        return RET_VAL_ERROR;
    }
    size_t written = safe_fwrite(fp, buffer, size);
    if (written != size) {
        fprintf(stderr, "Incomplete bytes written to file. Expected: %llu, Written: %zu\n", size, written);
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    fprintf(stderr, "Creating output file: %s\n", path);
    return RET_VAL_SUCCESS;
}