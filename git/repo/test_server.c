
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


ServerData server;
ServerBuffers buffers;
ClientList client_list;

HANDLE hthread_receive_frame;
HANDLE hthread_process_frame;
HANDLE hthread_ack_frame;
HANDLE hthread_send_ack; 
HANDLE hthread_client_timeout;
HANDLE hthread_file_stream[MAX_ACTIVE_FSTREAMS];
HANDLE hthread_server_command;

ClientMap client_map;

const char *server_ip = "127.0.0.1"; // IPv4 example

// Client management functions
static Client* find_client(ClientList *client_list, const uint32_t session_id);
static Client* add_client(ClientList *client_list, const UdpFrame *recv_frame, const SOCKET srv_socket, const struct sockaddr_in *client_addr);
static int remove_client(ClientList *client_list, ServerBuffers* buffers, const uint32_t slot);

void file_cleanup_stream(ServerFileStream *fstream, ServerBuffers* buffers);
void message_cleanup_stream(MessageStream *mstream, ServerBuffers* buffers);
void cleanup_client(Client *client, ServerBuffers* buffers);
BOOL validate_file_hash(ServerFileStream *fstream);
void check_open_file_stream(ServerBuffers *buffers);

static void update_statistics(Client *client);

// Thread functions
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_send_ack(LPVOID lpParam);
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

    memset(&client_list, 0, sizeof(ClientList));

    server.server_status = STATUS_NONE;
    server.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    server.session_id_counter = 0;
    snprintf(server.name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, SERVER_NAME);

    return RET_VAL_SUCCESS;
}
static int init_server_config(){
    WSADATA wsaData;

    int rcvBufSize = 50 * 1024 * 1024;  // 50MB
    int sndBufSize = 5 * 1024 * 1024;  // 5MB


    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }

    server.socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (server.socket == INVALID_SOCKET) {
        fprintf(stderr, "WSASocket failed: %d\n", WSAGetLastError());
        closesocket(server.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

        // Set receive buffer
    setsockopt(server.socket, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBufSize, sizeof(rcvBufSize));

    // Set send buffer
    setsockopt(server.socket, SOL_SOCKET, SO_SNDBUF, (char*)&sndBufSize, sizeof(sndBufSize));

    server.server_addr.sin_family = AF_INET;
    server.server_addr.sin_port = _htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server.server_addr.sin_addr);
 
    if (bind(server.socket, (SOCKADDR*)&server.server_addr, sizeof(server.server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);


    server.iocp_handle = CreateIoCompletionPort((HANDLE)server.socket, NULL, 0, 0);
    if (server.iocp_handle == NULL || server.iocp_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateIoCompletionPort failed: %d\n", GetLastError());
        closesocket(server.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    return RET_VAL_SUCCESS;

}
static int init_server_buffers(){
    // initialize queue frames
    buffers.queue_frame.head = 0;
    buffers.queue_frame.tail = 0;
    memset(&buffers.queue_frame.frame_entry, 0, FRAME_QUEUE_SIZE * sizeof(QueueFrameEntry));
    InitializeCriticalSection(&buffers.queue_frame.mutex);
    buffers.queue_frame.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    // initialize priority queue frames
    buffers.queue_priority_frame.head = 0;
    buffers.queue_priority_frame.tail = 0;
    memset(&buffers.queue_priority_frame.frame_entry, 0, FRAME_QUEUE_SIZE * sizeof(QueueFrameEntry));
    InitializeCriticalSection(&buffers.queue_priority_frame.mutex);
    buffers.queue_priority_frame.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);

    buffers.queue_priority_ack.head = 0;
    buffers.queue_priority_ack.tail = 0;
    memset(&buffers.queue_priority_ack.entry, 0, QUEUE_ACK_SIZE * sizeof(QueueAckEntry));
    InitializeCriticalSection(&buffers.queue_priority_ack.mutex);
    buffers.queue_priority_ack.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);

    buffers.mqueue_ack.head = 0;
    buffers.mqueue_ack.tail = 0;
    memset(&buffers.mqueue_ack.entry, 0, QUEUE_ACK_SIZE * sizeof(QueueAckEntry));
    InitializeCriticalSection(&buffers.mqueue_ack.mutex);
    buffers.mqueue_ack.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);

    buffers.fqueue_ack.head = 0;
    buffers.fqueue_ack.tail = 0;
    memset(&buffers.fqueue_ack.entry, 0, QUEUE_ACK_SIZE * sizeof(QueueAckEntry));
    InitializeCriticalSection(&buffers.fqueue_ack.mutex);
    buffers.fqueue_ack.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);

    buffers.queue_fstream.head = 0;
    buffers.queue_fstream.tail = 0;
    buffers.queue_fstream.pending = 0;
    memset(&buffers.queue_fstream.pfstream, 0, QUEUE_FSTREAM_SIZE * sizeof(intptr_t));
    InitializeCriticalSection(&buffers.queue_fstream.lock);
    buffers.queue_fstream.semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
 
    for(int i = 0; i < MAX_CLIENTS; i++){
        InitializeCriticalSection(&client_list.client[i].lock);
        for(int j = 0; j < MAX_CLIENT_MESSAGE_STREAMS; j++){
            InitializeCriticalSection(&client_list.client[i].mstream[j].lock);
        }
    }
    InitializeCriticalSection(&client_list.lock);

    for(int i = 0; i < MAX_ACTIVE_FSTREAMS; i++){
        InitializeCriticalSection(&buffers.fstream[i].lock);
    }

    buffers.pool_file_chunk.block_size = BLOCK_SIZE_CHUNK;
    buffers.pool_file_chunk.block_count = BLOCK_COUNT_CHUNK;
    pool_init(&buffers.pool_file_chunk);

    for(int i = 0; i < HASH_SIZE_ID; i++){
        buffers.ht_fid.entry[i] = NULL;
    }
    InitializeCriticalSection(&buffers.ht_fid.mutex);
    buffers.ht_fid.count = 0;

    for(int i = 0; i < HASH_SIZE_ID; i++){
        buffers.ht_mid.entry[i] = NULL;
    }
    InitializeCriticalSection(&buffers.ht_mid.mutex);
    buffers.ht_mid.count = 0;

    server.server_status = STATUS_READY;
    return RET_VAL_SUCCESS;

}
static int start_threads() {
    // Create threads for receiving and processing frames
    hthread_receive_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_receive_frame, &server, 0, NULL);
    if (hthread_receive_frame == NULL) {
        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_process_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_process_frame, &server, 0, NULL);
    if (hthread_process_frame == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_send_ack = (HANDLE)_beginthreadex(NULL, 0, thread_proc_send_ack, NULL, 0, NULL);
    if (hthread_send_ack == NULL) {
        fprintf(stderr, "Failed to create send ack thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_client_timeout = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_timeout, &client_list, 0, NULL);
    if (hthread_client_timeout == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    for(int i = 0; i < MAX_ACTIVE_FSTREAMS; i++){
        hthread_file_stream[i] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_file_stream, (LPVOID)(intptr_t)i, 0, NULL);
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
    server.server_status = STATUS_READY;
    return RET_VAL_SUCCESS;
}
static void shutdown_server() {

    server.server_status = STATUS_NONE;

    if (hthread_receive_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_receive_frame, INFINITE);
        CloseHandle(hthread_receive_frame);
    }
    fprintf(stdout,"receive frame thread closed...\n");

    if (hthread_process_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_process_frame, INFINITE);
        CloseHandle(hthread_process_frame);
    }
    fprintf(stdout,"process thread closed...\n");

    // if (hthread_ack_frame) {
    //     // Signal the receive thread to stop and wait for it to finish
    //     WaitForSingleObject(hthread_ack_frame, INFINITE);
    //     CloseHandle(hthread_ack_frame);
    // }
    // fprintf(stdout,"ack thread closed...\n");

    if (hthread_send_ack) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_send_ack, INFINITE);
        CloseHandle(hthread_send_ack);
    }
    fprintf(stdout,"send ack thread closed...\n");

    if (hthread_client_timeout) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_client_timeout, INFINITE);
        CloseHandle(hthread_client_timeout);
    }
    fprintf(stdout,"client timeout thread closed...\n");

    if (hthread_file_stream) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_file_stream, INFINITE);
        CloseHandle(hthread_file_stream);
    }
    fprintf(stdout,"file stream io thread closed...\n");

    if (hthread_server_command) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_server_command, INFINITE);
        CloseHandle(hthread_server_command);
    }
    fprintf(stdout,"server command thread closed...\n");

    for(int i = 0; i < MAX_CLIENTS; i++){
        DeleteCriticalSection(&client_list.client[i].lock);
        // for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
        //     DeleteCriticalSection(&client_list.client[i].fstream[j].lock);
        // }
        for(int j = 0; j < MAX_CLIENT_MESSAGE_STREAMS; j++){
            DeleteCriticalSection(&client_list.client[i].mstream[j].lock);
        }
    }
    DeleteCriticalSection(&client_list.lock);

    DeleteCriticalSection(&buffers.mqueue_ack.mutex);
    DeleteCriticalSection(&buffers.fqueue_ack.mutex);
    DeleteCriticalSection(&buffers.queue_priority_ack.mutex);

    DeleteCriticalSection(&buffers.queue_frame.mutex);
    DeleteCriticalSection(&buffers.queue_priority_frame.mutex);
    

    closesocket(server.socket);
    WSACleanup();
    printf("Server shut down!\n");
}

// Find client by session ID
static Client* find_client(ClientList *client_list, const uint32_t session_id) {
    // Search for a client within the provided ClientList that matches the given session ID.

    // Iterate through each possible client slot in the list.
    EnterCriticalSection(&client_list->lock);
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        
        // Acquire the critical section lock for the current client slot.
        EnterCriticalSection(&client_list->client[slot].lock);

        // Check if the current client slot is marked as SLOT_FREE.
        // A free slot indicates no active client, so it cannot be the one we are searching for.
        if(client_list->client[slot].status_index == SLOT_FREE){
            // If the slot is free, release its lock before moving to the next slot.
            LeaveCriticalSection(&client_list->client[slot].lock);
            LeaveCriticalSection(&client_list->lock);
            continue; // Move to the next slot in the loop.
        }
        // If the slot is not free, compare its stored session ID with the target session_id.
        if(client_list->client[slot].sid == session_id){
            // If a client with a matching session ID is found, release its lock.
            LeaveCriticalSection(&client_list->client[slot].lock);
            LeaveCriticalSection(&client_list->lock);
            // Return a pointer to the found ClientData structure.
            return &client_list->client[slot];
        }
        // If the slot is not free, but its session ID does not match,
        // release the lock for this slot and continue the search.
        LeaveCriticalSection(&client_list->client[slot].lock);
    }
    // If the loop completes without finding any client matching the session ID,
    // return NULL to indicate that no such client is currently active.
    LeaveCriticalSection(&client_list->lock);
    return NULL;
}
// Add a new client
static Client* add_client(ClientList *client_list, const UdpFrame *recv_frame, const SOCKET srv_socket, const struct sockaddr_in *client_addr) {
       
    uint32_t free_slot = 0;

    EnterCriticalSection(&client_list->lock);
    while(free_slot < MAX_CLIENTS){
        if(client_list->client[free_slot].status_index == SLOT_FREE) {
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
    
    // Enter a critical section to protect the 'new_client' data structure.
    // This lock ensures that the client's data is initialized safely without race conditions.
    EnterCriticalSection(&new_client->lock);

    new_client->client_index = free_slot; // Assigns the found slot number to the new client's data structure.
    new_client->status_index = SLOT_BUSY; // Marks the slot as busy, indicating it's now in use.
    // Copies the client's network address information (IP and port) from 'client_addr' to the new client's structure.
    new_client->srv_socket = srv_socket;
    memcpy(&new_client->client_addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED; // Sets the connection status to 'CLIENT_CONNECTED'.
    new_client->last_activity_time = time(NULL); // Records the current time as the last activity time for the new client.

    // Extracts the client ID from the received frame's payload, converting it from network byte order.
    new_client->cid = _ntohl(recv_frame->payload.connection_request.client_id); 
    // Assigns a unique session ID to the new client by atomically incrementing a global counter.
    new_client->sid = InterlockedIncrement(&server.session_id_counter);
    // Copies the flag from the received frame's request payload to the new client's data.
    new_client->flags = recv_frame->payload.connection_request.flags;
    
    // Formats and copies the client's name from the received frame's payload into the new client's structure.
    // 'snprintf' is used for safe string copying, preventing buffer overflows.
    snprintf(new_client->name, MAX_NAME_SIZE, "%.*s", MAX_NAME_SIZE - 1, recv_frame->payload.connection_request.client_name);

    // Converts the client's IP address from binary form to a human-readable string (IPv4).
    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    // Converts the client's port number from network byte order to host byte order.
    new_client->port = _ntohs(client_addr->sin_port);

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->sid);

    // Leaves the critical section, releasing the lock on the 'new_client' data structure.
    // This makes the newly initialized client data accessible to other threads.
    LeaveCriticalSection(&new_client->lock);
    LeaveCriticalSection(&client_list->lock);
    return new_client; // Returns a pointer to the newly added and initialized ClientData structure.
}
// Remove a client
static int remove_client(ClientList *client_list, ServerBuffers* buffers, const uint32_t slot) {
    // Search for the client with the given session ID
    // Checks if the provided ClientList pointer is NULL, indicating an invalid list.
    if(client_list == NULL){
        fprintf(stderr, "\nInvalid client pointer!\n"); // Prints an error message to standard error.
        return RET_VAL_ERROR;
    }
    // Validates the 'slot' index to ensure it is within the permissible range [0, MAX_CLIENTS - 1].
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "\nInvalid client slot nr:  %d", slot); // Prints an error message if the slot number is out of bounds.
        return RET_VAL_ERROR;
    }
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", client_list->client[slot].sid, client_list->client[slot].client_index);
    // Enters a critical section associated with the specific client slot.    
    EnterCriticalSection(&client_list->lock);
    // Calls a helper function to perform cleanup operations for the client in the specified slot.
    
    cleanup_client(&client_list->client[slot], buffers);
    // Leaves the critical section, releasing the lock on the client's data.
    LeaveCriticalSection(&client_list->lock);
    fprintf(stdout, "\nRemoved client successfully!\n");
    return RET_VAL_SUCCESS; // Returns a success value.
}
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

// --- Receive frame thread function ---
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam) {

    ServerData *server = (ServerData*)lpParam;

    if(issue_WSARecvFrom(server->socket, &server->iocp_context) == RET_VAL_ERROR){
        fprintf(stderr, "Initial WSARecvFrom failed: %d\n", WSAGetLastError());
        //TODO initiate some kind of global error and stop client?
        //goto exit_thread;
    }

    HANDLE CompletitionPort = server->iocp_handle;
    DWORD NrOfBytesTransferred;
    ULONG_PTR lpCompletitionKey;
    LPOVERLAPPED lpOverlapped;


    while (server->server_status == STATUS_READY) {

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
                    goto retry;
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
            BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                            frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                            frame_type == FRAME_TYPE_FILE_METADATA ||
                                            frame_type == FRAME_TYPE_DISCONNECT);

            QueueFrame* target_queue = NULL;
            if (is_high_priority_frame == TRUE) {
                target_queue = &buffers.queue_priority_frame;
            } else {
                target_queue = &buffers.queue_frame;
            }
            if (push_frame(target_queue, &frame_entry) == RET_VAL_ERROR) {
                fprintf(stderr, "Failed to push frame to queue. Queue full?\n");
            }
        }
retry:
        if(issue_WSARecvFrom(server->socket, iocp_overlapped) == RET_VAL_ERROR){
            fprintf(stderr, "WSARecvFrom re-issue failed: %d\n", WSAGetLastError());
            goto retry;
        }
    }
exit_thread:
    fprintf(stdout, "recv thread exiting\n");
    _endthreadex(0);
    return 0;
}
// --- Processes a received frame ---
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam) {

    uint16_t header_delimiter;      // Stores the extracted start delimiter from the frame header.
    uint8_t  header_frame_type;     // Stores the extracted frame type from the frame header.
    uint64_t header_seq_num;        // Stores the extracted sequence number from the frame header.
    uint32_t header_session_id;     // Stores the extracted session ID from the frame header.

    QueueFrameEntry frame_entry;    // A structure to temporarily hold a frame popped from a queue, along with its source address and size.
    QueueAckEntry ack_entry;     // A structure to hold details for an ACK/NAK to be sent (declared but not directly used in this snippet's logic).

    UdpFrame *frame;                // A pointer to the UDP frame data within frame_entry.
    struct sockaddr_in *src_addr;   // A pointer to the source address of the received UDP frame.
    uint32_t frame_bytes_received;  // The actual number of bytes received for the current UDP frame.

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    Client *client;                 // A pointer to the Client structure associated with the current frame's session.

    HANDLE queue_semaphores[2] = {buffers.queue_priority_frame.semaphore, buffers.queue_frame.semaphore};

    while(server.server_status == STATUS_READY) {
        //memset(&frame_entry, 0, sizeof(QueueFrameEntry));
        DWORD wait_result = WaitForMultipleObjects(2, queue_semaphores, FALSE, INFINITE);
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
        } else {
            fprintf(stderr, "Unexpected wait result: %lu\n", wait_result);
            continue;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        header_delimiter = _ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = _ntohll(frame->header.seq_num);
        header_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);

        if (header_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, header_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            continue;
        }

        client = NULL;
        if(header_frame_type == FRAME_TYPE_CONNECT_REQUEST && header_session_id == FRAME_TYPE_CONNECT_REQUEST_SID){
            client = add_client(&client_list, frame, server.socket, src_addr);
            if (client == NULL) {
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
                continue;
            }
            EnterCriticalSection(&client->lock);
            client->last_activity_time = time(NULL);
            fprintf(stdout, "Client %s:%d Requested connection. Responding to connect request with Session ID: %u\n", client->ip, src_port, client->sid);
            send_connect_response(header_seq_num, 
                                    client->sid, 
                                    server.session_timeout, 
                                    server.server_status, 
                                    server.name, 
                                    server.socket, 
                                    &client->client_addr
                                );
            // Release the client's lock.
            LeaveCriticalSection(&client->lock);
            continue;

        } else if (header_frame_type == FRAME_TYPE_CONNECT_REQUEST && header_session_id != FRAME_TYPE_CONNECT_REQUEST_SID) {
            client = find_client(&client_list, header_session_id);
            if(client == NULL){
                fprintf(stderr, "Unknown client tried to re-connect\n");
                continue;
            }
            EnterCriticalSection(&client->lock);
            client->last_activity_time = time(NULL);
            fprintf(stdout, "Client %s:%d Requested re-connection. Responding to re-connect request with Session ID: %u\n", client->ip, src_port, client->sid);
            send_connect_response(header_seq_num, 
                                    client->sid, 
                                    server.session_timeout, 
                                    server.server_status, 
                                    server.name, 
                                    server.socket, 
                                    &client->client_addr
                                );
            // Release the client's lock.
            LeaveCriticalSection(&client->lock);
            continue;
        } else {
            client = find_client(&client_list, header_session_id);
            if(client == NULL){
                fprintf(stderr, "Received frame from unknown client\n");
                continue;
            }
        }

        switch (header_frame_type) {

            case FRAME_TYPE_ACK:
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&client->lock);
                // TODO: Implement the full ACK processing logic here. This typically involves:
                //   - Removing acknowledged packets from the sender's retransmission queue.
                //   - Updating window sizes for flow and congestion control.
                //   - Advancing sequence numbers to indicate successfully received data.
                break;

            case FRAME_TYPE_KEEP_ALIVE:
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&client->lock);

                new_ack_entry(&ack_entry, header_seq_num, header_session_id, STS_KEEP_ALIVE, client->srv_socket, &client->client_addr);
                push_ack(&buffers.queue_priority_ack, &ack_entry);

                break;

            case FRAME_TYPE_FILE_METADATA:
                // fprintf(stdout, "\n   FRAME_TYPE_FILE_METADATA\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %u\n   File Size: %llu\n   File Name: %s\n   File sha256: ", 
                //                                     _ntohll(frame->header.seq_num), 
                //                                     _ntohl(frame->header.session_id), 
                //                                     _ntohl(frame->header.checksum),
                //                                     _ntohl(frame->payload.file_metadata.file_id),
                //                                     _ntohll(frame->payload.file_metadata.file_size),    
                //                                     frame->payload.file_metadata.filename                
                // );
                // for(int i = 0; i < 32; i++){
                //     fprintf(stdout, "%02x", (unsigned char)frame->payload.file_metadata.file_hash[i]);
                // }
                // fprintf(stdout,"\n");
                handle_file_metadata(client, frame, &buffers);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                handle_file_fragment(client, frame, &buffers);
                break;

            case FRAME_TYPE_FILE_END:
                // fprintf(stdout, "\n   FRAME_TYPE_FILE_END\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %u\n   File Size: %llu\n   File sha256: ", 
                //                                     _ntohll(frame->header.seq_num), 
                //                                     _ntohl(frame->header.session_id), 
                //                                     _ntohl(frame->header.checksum),
                //                                     _ntohl(frame->payload.file_end.file_id),
                //                                     _ntohll(frame->payload.file_end.file_size)         
                // );
                // for(int i = 0; i < 32; i++){
                //     fprintf(stdout, "%02x", (unsigned char)frame->payload.file_end.file_hash[i]);
                // }
                fprintf(stdout,"\n");
                handle_file_end(client, frame, &buffers);
                break;

            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                handle_message_fragment(client, frame, &buffers);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "Client %s:%d with session ID: %d requested disconnect...\n", client->ip, src_port, client->sid);
                new_ack_entry(&ack_entry, header_seq_num, header_session_id, STS_CONFIRM_DISCONNECT, client->srv_socket, &client->client_addr);
                push_ack(&buffers.queue_priority_ack, &ack_entry);
                remove_client(&client_list, &buffers, client->client_index);
                break;

            default:
                fprintf(stderr, "Received unknown frame type: %u from %s:%d (Session ID: %u). Discarding.\n",
                        header_frame_type, src_ip, src_port, header_session_id);
                break;
        }
    }
    _endthreadex(0);
    return 0;
}
// --- Ack Thread function ---
DWORD WINAPI thread_proc_send_ack(LPVOID lpParam){

    QueueAckEntry ack_entry;
    HANDLE queue_semaphores[3] = {buffers.queue_priority_ack.semaphore, buffers.mqueue_ack.semaphore, buffers.fqueue_ack.semaphore};

    while (server.server_status == STATUS_READY) {
        DWORD wait_result = WaitForMultipleObjects(3, queue_semaphores, FALSE, INFINITE);
        //memset(&ack_entry, 0, sizeof(QueueAckEntry));
        if (wait_result == WAIT_OBJECT_0) {
            if (pop_ack(&buffers.queue_priority_ack, &ack_entry) == RET_VAL_SUCCESS) {
                // Frame successfully retrieved from the priority queue.
            } else {
                continue;
            }
        } else if (wait_result == WAIT_OBJECT_0 + 1) {
            if (pop_ack(&buffers.mqueue_ack, &ack_entry) == RET_VAL_SUCCESS) {
                // Frame successfully retrieved from the priority queue.
            } else {
                continue;
            }
        } else if (wait_result == WAIT_OBJECT_0 + 2) {
            if (pop_ack(&buffers.fqueue_ack, &ack_entry) == RET_VAL_SUCCESS) {
                // Frame successfully retrieved from the general data queue.
            } else {
                continue;
            }
        } else {
            fprintf(stderr, "Unexpected wait result: %lu\n", wait_result);
            continue;
        }
        send_ack(ack_entry.seq, ack_entry.sid, ack_entry.op_code, ack_entry.src_socket, &ack_entry.dest_addr);
    }
    _endthreadex(0);
    return 0;
}
// --- Client timeout thread function ---
DWORD WINAPI thread_proc_client_timeout(LPVOID lpParam){

    time_t time_now;
    ClientList *client_list = (ClientList*)lpParam; // Cast the parameter to ClientList.
    while(server.server_status == STATUS_READY) {
        time_now = time(NULL);
        EnterCriticalSection(&client_list->lock);
        for(int i = 0; i < MAX_CLIENTS; i++){
            EnterCriticalSection(&client_list->client[i].lock);
            if(client_list->client[i].status_index == SLOT_FREE){
                LeaveCriticalSection(&client_list->client[i].lock);
                continue; // Skip to the next client slot.
            }

            if(time_now - (time_t)client_list->client[i].last_activity_time < (time_t)server.session_timeout){
                LeaveCriticalSection(&client_list->client[i].lock);
                continue; // Skip to the next client slot.
            }

            LeaveCriticalSection(&client_list->client[i].lock);

            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", client_list->client[i].sid);
            send_disconnect(client_list->client[i].sid, server.socket, &client_list->client[i].client_addr);
            remove_client(client_list, &buffers, i);
        }
        LeaveCriticalSection(&client_list->lock);
        Sleep(1000);
    }

    // After the `while` loop condition (`server.status == SERVER_READY`) becomes false,
    _endthreadex(0);
    return 0; // Return 0 to indicate that the thread terminated successfully.
}
// --- Thread for writing file streams to disk ---
DWORD WINAPI thread_proc_file_stream(LPVOID lpParam) {
 
    int index = (int)(intptr_t)lpParam;

    SHA256_CTX sha256_ctx;

    while (server.server_status == STATUS_READY) { 
        // Wait for file transfer event or client disconnect
        DWORD wait_semaphore = WaitForSingleObject(buffers.queue_fstream.semaphore, INFINITE);
 
        ServerFileStream *fstream = (ServerFileStream*)pop_fstream(&buffers.queue_fstream);

        if(!fstream){
            fprintf(stderr, "Received null fstream pointer from fstream queue!\n");
            continue;
        }

        sha256_init(&sha256_ctx);                              
        // Check if the file stream is currently active/busy with a transfer.
        
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
                pool_free(&buffers.pool_file_chunk, fstream->pool_block_file_chunk[k]);
                fstream->pool_block_file_chunk[k] = NULL;
            }

            if(fstream->fstream_err != STREAM_ERR_NONE){
                file_cleanup_stream(fstream, &buffers); // Clean up the entire file stream.
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
                file_cleanup_stream(fstream, &buffers); // Clean up the entire file stream.
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
                    file_cleanup_stream(fstream, &buffers);
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
                
                ht_update_id_status(&buffers.ht_fid, fstream->sid, fstream->fid, ID_RECV_COMPLETE);
                fprintf(stdout, "File '%s' received written to disk and validated", fstream->fnm);

                QueueAckEntry ack_entry;
                new_ack_entry(&ack_entry, fstream->file_end_frame_seq_num, fstream->sid, STS_CONFIRM_FILE_END, server.socket, &fstream->client_addr);
                push_ack(&buffers.queue_priority_ack, &ack_entry);

                file_cleanup_stream(fstream, &buffers);
                LeaveCriticalSection(&fstream->lock);
                break; //exit the while loop
            }
            
            LeaveCriticalSection(&fstream->lock);
            Sleep(10);
        } // end of while(fstream->busy)
    }
exit:
    //fprintf(stdout,"stream successfully closed %d\n", fstream->fstream_index);
    _endthreadex(0);
    return 0;
}
// --- Process server command ---
DWORD WINAPI thread_proc_server_command(LPVOID lpParam){
    
    char cmd;
    
    while (server.server_status == STATUS_READY){

        fprintf(stdout,"Waiting for command...\n");
        cmd = getchar();
        switch(cmd) {
            case 's':
            case 'S':
            //check what file streams are still open
                check_open_file_stream(&buffers);
                break;

            case 'q':
            case 'Q':
                server.server_status = STATUS_NONE;
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
void file_cleanup_stream(ServerFileStream *fstream, ServerBuffers* buffers){

    if(fstream == NULL){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer file stream\n");
        return;
    }

    EnterCriticalSection(&fstream->lock);

    if(fstream->fp && fstream->fstream_busy && !fstream->file_complete){
        fclose(fstream->fp);
        remove(fstream->fnm);
        fstream->fp = NULL;
    }
    if(fstream->fp != NULL){
        if(fflush(fstream->fp) != 0){
            fprintf(stderr, "Error flushing the file to disk. File is still in use.\n");
        } else {
            int fclosed = fclose(fstream->fp);
            Sleep(50); // Sleep for 50 milliseconds to ensure the file is properly closed before proceeding.
            if(fclosed != 0){
                fprintf(stderr, "Error closing the file stream: %s (errno: %d)\n", fstream->fnm, errno);
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
            pool_free(&buffers->pool_file_chunk, fstream->pool_block_file_chunk[k]);
        }
        fstream->pool_block_file_chunk[k] = NULL;
    }

    fstream->sid;                                   // Session ID associated with this file stream.
    fstream->fid;                                   // File ID, unique identifier for the file associated with this file stream.
    fstream->fsize;                                 // Total size of the file being transferred.
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
    memset(fstream->fnm, 0, MAX_NAME_SIZE);
    
    fstream->fstream_busy = FALSE;                  // Indicates if this stream channel is currently in use for a transfer.

    LeaveCriticalSection(&fstream->lock);
    return;
}
// Clean up the message stream resources after a file transfer is completed or aborted.
void message_cleanup_stream(MessageStream *mstream, ServerBuffers* buffers){

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
void cleanup_client(Client *client, ServerBuffers* buffers){
    
    if(client == NULL){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }
    EnterCriticalSection(&client->lock);

    for(int i = 0; i < MAX_ACTIVE_FSTREAMS; i++){
        ServerFileStream *fstream = &buffers->fstream[i];
        if(fstream->sid ==  client->sid){
            file_cleanup_stream(fstream, buffers);
        }
    }

    ht_remove_all_sid(&buffers->ht_fid, client->sid);

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        // Acquire the critical section lock for the current message stream.
        EnterCriticalSection(&client->mstream[i].lock);
        // Call the dedicated cleanup function for the current message stream.
        // This function will free memory, close files, and reset the message stream's state.
        message_cleanup_stream(&client->mstream[i], buffers);
        // Release the critical section lock for the current message stream.
        LeaveCriticalSection(&client->mstream[i].lock);
    }

    // Reset the client's network address structure by filling it with zeros.
    memset(&client->client_addr, 0, sizeof(struct sockaddr_in));
    // Reset the client's IP address string by filling it with zeros.
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    // Reset the client's port number to zero.
    client->port = 0;
    // Reset the client's unique identifier to zero.
    client->cid = 0;
    // Reset the client's name buffer by filling it with zeros.
    memset(&client->name, 0, MAX_NAME_SIZE);
    // Reset any client-specific flags to zero.
    client->flags = 0;
    // Set the client's connection status to disconnected.
    client->connection_status = CLIENT_DISCONNECTED;
    // Update the last activity time to the current time. This can be useful for
    // tracking when the client slot became free or was last active.
    client->last_activity_time = time(NULL);
    // Reset the client's assigned slot number to zero.
    client->client_index = 0;
    // Mark the client slot as free, indicating it's available for a new connection.
    client->status_index = SLOT_FREE;
    LeaveCriticalSection(&client->lock);
    return;
}
// Compare received hash with calculated hash
BOOL validate_file_hash(ServerFileStream *fstream){

    fprintf(stdout, "File hash received: ");
    for(int i = 0; i < 32; i++){
        fprintf(stdout, "%02x", (uint8_t)fstream->received_sha256[i]);
    }

    fprintf(stdout, "File hash calculated: ");
    for(int i = 0; i < 32; i++){
        fprintf(stdout, "%02x", (uint8_t)fstream->calculated_sha256[i]);
    }

    for(int i = 0; i < 32; i++){
        if((uint8_t)fstream->calculated_sha256[i] != (uint8_t)fstream->received_sha256[i]){
            fprintf(stdout, "ERROR: SHA256 MISSMATCH\n");
            return FALSE;
        }
    }
    return TRUE;
}
// Check for any open file streams across all clients.
void check_open_file_stream(ServerBuffers *buffers){
    for(int i = 0; i < MAX_ACTIVE_FSTREAMS; i++){
        if(buffers->fstream[i].fstream_busy == TRUE){
            fprintf(stdout, "File stream still open: %d\n", i);
        }
    }
    fprintf(stdout, "Completed checking opened file streams\n");
    return;
}

int main() {
    //get_network_config();
    init_server_session();
    init_server_config();
    init_server_buffers();
    start_threads();
    // Main server loop for general management, timeouts, and state updates
    while (server.server_status == STATUS_READY) {

        Sleep(250); // Prevent busy-waiting
   
        printf("\r\033[2K--  Free Blocks: %llu; Total: %llu", buffers.pool_file_chunk.free_blocks, buffers.pool_file_chunk.block_count);
        fflush(stdout);

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}

