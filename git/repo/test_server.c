
#include <stdio.h>                      // For printf, fprintf
#include <string.h>                     // For memset, memcpy
#include <stdint.h>                     // For fixed-width integer types    
//#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <time.h>                       // For time functions
#include <process.h>                    // For _beginthreadex
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep
#include <iphlpapi.h>                   // For IP Helper API functions

#pragma comment(lib, "Ws2_32.lib")      // Link against Winsock library
#pragma comment(lib, "iphlpapi.lib")    // Link against IP Helper API library

#include "include/server.h"             // For server data structures and definitions
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/netendians.h"         // For network byte order conversions
#include "include/checksum.h"           // For checksum validation
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management
#include "include/file_handler.h"       // For frame handling functions
#include "include/message_handler.h"       // For frame handling functions

ServerData server;
ServerIOManager io_manager;
ClientList client_list;

HANDLE hthread_receive_frame;
HANDLE hthread_process_frame;
HANDLE hthread_ack_frame; 
HANDLE hthread_client_timeout;
HANDLE hthread_file_stream_io;
HANDLE hthread_server_command;

const char *server_ip = "127.0.0.1"; // IPv4 example

// Client management functions
static Client* find_client(ClientList *client_list, const uint32_t session_id);
static Client* add_client(ClientList *client_list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
static int remove_client(ClientList *client_list, ServerIOManager* io_manager, const uint32_t slot);
void check_open_file_stream(ClientList *client_list);

static void update_statistics(Client *client);

// Thread functions
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_ack_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_client_timeout(LPVOID lpParam);
DWORD WINAPI thread_proc_file_stream_io(LPVOID lpParam);
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

    server.server_status = SERVER_STATUS_NONE;
    server.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC;
    server.session_id_counter = 0;
    snprintf(server.name, NAME_SIZE, "%.*s", NAME_SIZE - 1, SERVER_NAME);

    return RET_VAL_SUCCESS;
}
static int init_server_config(){
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }

    server.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server.socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return RET_VAL_ERROR;
    }
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
    return RET_VAL_SUCCESS;

}
static int init_server_buffers(){
    io_manager.queue_frame.head = 0;
    io_manager.queue_frame.tail = 0;
    InitializeCriticalSection(&io_manager.queue_frame.mutex);
    io_manager.queue_priority_frame.head = 0;
    io_manager.queue_priority_frame.tail = 0;
    InitializeCriticalSection(&io_manager.queue_priority_frame.mutex);
    io_manager.queue_seq_num.head = 0;
    io_manager.queue_seq_num.tail = 0;
    InitializeCriticalSection(&io_manager.queue_seq_num.mutex);
    io_manager.queue_priority_seq_num.head = 0;
    io_manager.queue_priority_seq_num.tail = 0;
    InitializeCriticalSection(&io_manager.queue_priority_seq_num.mutex);
 
    for(int i = 0; i < MAX_CLIENTS; i++){
        InitializeCriticalSection(&client_list.client[i].lock);
        for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
            InitializeCriticalSection(&client_list.client[i].file_stream[j].lock);
            InitializeCriticalSection(&client_list.client[i].msg_stream[j].lock);
        }
    }
    InitializeCriticalSection(&client_list.lock);

    io_manager.pool_file_chunk.block_size = BLOCK_SIZE_CHUNK;
    io_manager.pool_file_chunk.block_count = BLOCK_COUNT_CHUNK;
    pool_init(&io_manager.pool_file_chunk);

    for(int i = 0; i < HASH_SIZE_UID; i++){
        io_manager.uid_hash_table[i] = NULL;
    }
    InitializeCriticalSection(&io_manager.uid_ht_mutex);
    server.server_status = SERVER_STATUS_READY;
    return RET_VAL_SUCCESS;

}
static int start_threads() {
    // Create threads for receiving and processing frames
    hthread_receive_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_receive_frame, NULL, 0, NULL);
    if (hthread_receive_frame == NULL) {
        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_process_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_process_frame, NULL, 0, NULL);
    if (hthread_process_frame == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_ack_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_ack_frame, NULL, 0, NULL);
    if (hthread_ack_frame == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_client_timeout = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_timeout, &client_list, 0, NULL);
    if (hthread_client_timeout == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_file_stream_io = (HANDLE)_beginthreadex(NULL, 0, thread_proc_file_stream_io, &client_list, 0, NULL);
    if (hthread_file_stream_io == NULL) {
        fprintf(stderr, "Failed to file stream io thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }

    hthread_server_command = (HANDLE)_beginthreadex(NULL, 0, thread_proc_server_command, NULL, 0, NULL);
    if (hthread_server_command == NULL) {
        fprintf(stderr, "Failed to create server command thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    server.server_status = SERVER_STATUS_READY;
    return RET_VAL_SUCCESS;
}
static void shutdown_server() {

    server.server_status = SERVER_STATUS_NONE;

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

    if (hthread_ack_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_ack_frame, INFINITE);
        CloseHandle(hthread_ack_frame);
    }
    fprintf(stdout,"ack thread closed...\n");

    if (hthread_client_timeout) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_client_timeout, INFINITE);
        CloseHandle(hthread_client_timeout);
    }
    fprintf(stdout,"client timeout thread closed...\n");

    if (hthread_file_stream_io) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_file_stream_io, INFINITE);
        CloseHandle(hthread_file_stream_io);
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
        for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
            DeleteCriticalSection(&client_list.client[i].file_stream[j].lock);
            DeleteCriticalSection(&client_list.client[i].msg_stream[j].lock);
        }
    }
    DeleteCriticalSection(&client_list.lock);
    DeleteCriticalSection(&io_manager.queue_frame.mutex);
    DeleteCriticalSection(&io_manager.queue_priority_frame.mutex);
    DeleteCriticalSection(&io_manager.queue_seq_num.mutex);
    DeleteCriticalSection(&io_manager.queue_priority_seq_num.mutex);

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
        if(client_list->client[slot].slot_status == SLOT_FREE){
            // If the slot is free, release its lock before moving to the next slot.
            LeaveCriticalSection(&client_list->client[slot].lock);
            LeaveCriticalSection(&client_list->lock);
            continue; // Move to the next slot in the loop.
        }
        // If the slot is not free, compare its stored session ID with the target session_id.
        if(client_list->client[slot].session_id == session_id){
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
static Client* add_client(ClientList *client_list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    
    uint32_t free_slot = 0; // Initializes a counter to search for an available client slot.
    // Loop through the client list to find the first available (free) slot.
    EnterCriticalSection(&client_list->lock);
    while(free_slot < MAX_CLIENTS){ // Continues as long as the current slot index is within the maximum allowed clients.
        if(client_list->client[free_slot].slot_status == SLOT_FREE) { // Checks if the current slot is marked as free.
            break; // If a free slot is found, exit the loop.
        }
        free_slot++; // Move to the next slot if the current one is busy.
    }
    // After the loop, check if all slots were iterated through without finding a free one.
    if(free_slot >= MAX_CLIENTS){ // If 'free_slot' is equal to or greater than MAX_CLIENTS, it means no free slot was found.
        fprintf(stderr, "\nMax clients reached. Cannot add new client.\n"); // Prints an error message to standard error.
        return NULL; // Returns NULL, indicating that a new client could not be added.
    }
    // A free slot has been found; obtain a pointer to the ClientData structure at this slot.
    Client *new_client = &client_list->client[free_slot]; 
    
    // Enter a critical section to protect the 'new_client' data structure.
    // This lock ensures that the client's data is initialized safely without race conditions.
    EnterCriticalSection(&new_client->lock);

    new_client->slot_num = free_slot; // Assigns the found slot number to the new client's data structure.
    new_client->slot_status = SLOT_BUSY; // Marks the slot as busy, indicating it's now in use.
    // Copies the client's network address information (IP and port) from 'client_addr' to the new client's structure.
    memcpy(&new_client->addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED; // Sets the connection status to 'CLIENT_CONNECTED'.
    new_client->last_activity_time = time(NULL); // Records the current time as the last activity time for the new client.

    // Extracts the client ID from the received frame's payload, converting it from network byte order.
    new_client->client_id = _ntohl(recv_frame->payload.request.client_id); 
    // Assigns a unique session ID to the new client by atomically incrementing a global counter.
    new_client->session_id = InterlockedIncrement(&server.session_id_counter); 
    // Copies the flag from the received frame's request payload to the new client's data.
    new_client->flag = recv_frame->payload.request.flag;
    
    // Formats and copies the client's name from the received frame's payload into the new client's structure.
    // 'snprintf' is used for safe string copying, preventing buffer overflows.
    snprintf(new_client->name, NAME_SIZE, "%.*s", NAME_SIZE - 1, recv_frame->payload.request.client_name);

    // Converts the client's IP address from binary form to a human-readable string (IPv4).
    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    // Converts the client's port number from network byte order to host byte order.
    new_client->port = _ntohs(client_addr->sin_port);

    // Prints a log message to standard output, announcing the addition of a new client with their IP, port, and assigned session ID.
    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->session_id);

    // Leaves the critical section, releasing the lock on the 'new_client' data structure.
    // This makes the newly initialized client data accessible to other threads.
    LeaveCriticalSection(&new_client->lock);
    LeaveCriticalSection(&client_list->lock);
    return new_client; // Returns a pointer to the newly added and initialized ClientData structure.
}
// Remove a client
static int remove_client(ClientList *client_list, ServerIOManager* io_manager, const uint32_t slot) {
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
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", client_list->client[slot].session_id, client_list->client[slot].slot_num);
    // Enters a critical section associated with the specific client slot.    
    EnterCriticalSection(&client_list->lock);  
    // Calls a helper function to perform cleanup operations for the client in the specified slot.
    cleanup_client(&client_list->client[slot], io_manager);
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

    //update file transfer speed in MB/s
    GetSystemTimePreciseAsFileTime(&client->statistics.ft);
    client->statistics.crt_uli.LowPart = client->statistics.ft.dwLowDateTime;
    client->statistics.crt_uli.HighPart = client->statistics.ft.dwHighDateTime;
    client->statistics.crt_microseconds = client->statistics.crt_uli.QuadPart / 10;

    client->statistics.crt_bytes_received = (float)client->file_stream[0].bytes_received;

    //TRANSFER SPEED
    //current speed (1 cycle)
    client->statistics.file_transfer_speed = (client->statistics.crt_bytes_received - client->statistics.prev_bytes_received) / (float)((client->statistics.crt_microseconds - client->statistics.prev_microseconds));
    client->statistics.prev_bytes_received = client->statistics.crt_bytes_received;
    client->statistics.prev_microseconds = client->statistics.crt_microseconds;
    //PROGRESS - update file transfer progress percentage
    client->statistics.file_transfer_progress = (float)client->file_stream[0].bytes_received / (float)client->file_stream[0].f_size * 100.0;

    fprintf(stdout, "\rFile transfer progress: %.2f %% - Speed: %.2f MB/s", client->statistics.file_transfer_progress, client->statistics.file_transfer_speed);
    fflush(stdout);
}
// --- Receive Thread Function ---
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam) {

    UdpFrame received_frame;
    QueueFrameEntry frame_entry;

    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);
    int recv_error_code;

    int bytes_received;

    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
    }

    while (server.server_status == SERVER_STATUS_READY) {

        memset(&received_frame, 0, sizeof(UdpFrame));
        memset(&src_addr, 0, sizeof(src_addr));

        bytes_received = recvfrom(server.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (struct sockaddr*)&src_addr, &src_addr_len);

        if (bytes_received == SOCKET_ERROR) {
            recv_error_code = WSAGetLastError();
            if (recv_error_code != WSAETIMEDOUT) {
                fprintf(stderr, "recvfrom failed with error: %d\n", recv_error_code);
            }
        } else if (bytes_received > 0) {
            if(bytes_received > sizeof(UdpFrame)){
                fprintf(stdout, "Frame received with bytes > max frame size!\n");
                continue;
            }

            memset(&frame_entry, 0, sizeof(QueueFrameEntry));
            memcpy(&frame_entry.frame, &received_frame, bytes_received);
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));
            frame_entry.frame_size = bytes_received;

            uint8_t frame_type = frame_entry.frame.header.frame_type;
            BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                           frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                           frame_type == FRAME_TYPE_FILE_METADATA ||
                                           frame_type == FRAME_TYPE_DISCONNECT);

            QueueFrame *target_queue = NULL;

            if (is_high_priority_frame == TRUE) {
                target_queue = &io_manager.queue_priority_frame; // High-priority frames
            } else {
                target_queue = &io_manager.queue_frame;      // Other frames go to the general `queue_frame`.
            }

            if (push_frame(target_queue, &frame_entry) != RET_VAL_SUCCESS) {
                fprintf(stderr, "Failed to push frame to queue. Queue full?\n");
                continue; // Discard this frame (as it couldn't be queued) and proceed to the next receive attempt.
            }
        }
    }
    _endthreadex(0);
    return 0; // Return 0 to indicate successful thread termination.
}
// --- Processes a received frame ---
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam) {

    uint16_t header_delimiter;      // Stores the extracted start delimiter from the frame header.
    uint8_t  header_frame_type;     // Stores the extracted frame type from the frame header.
    uint64_t header_seq_num;        // Stores the extracted sequence number from the frame header.
    uint32_t header_session_id;     // Stores the extracted session ID from the frame header.

    QueueFrameEntry frame_entry;    // A structure to temporarily hold a frame popped from a queue, along with its source address and size.
    QueueSeqNumEntry ack_entry;     // A structure to hold details for an ACK/NAK to be sent (declared but not directly used in this snippet's logic).

    UdpFrame *frame;                // A pointer to the UDP frame data within frame_entry.
    struct sockaddr_in *src_addr;   // A pointer to the source address of the received UDP frame.
    uint32_t frame_bytes_received;  // The actual number of bytes received for the current UDP frame.

    char src_ip[INET_ADDRSTRLEN];   // Buffer to store the human-readable string representation of the source IP address.
    uint16_t src_port;              // Stores the source port number.

    Client *client;                 // A pointer to the Client structure associated with the current frame's session.

    // Main thread loop: This thread continuously runs as long as the server's global status
    // is set to SERVER_READY. Its primary responsibility is to dequeue incoming UDP frames
    // and dispatch them for processing based on their type.
    while(server.server_status == SERVER_STATUS_READY) {
        // Clear the frame_entry structure at the beginning of each iteration.
        // This ensures that any data from a previous frame is not accidentally processed again.
        memset(&frame_entry, 0, sizeof(QueueFrameEntry));

        // Attempt to pop a frame from the control frame queue first.
        // The control queue typically handles high-priority messages like connection requests or disconnects.
        if (pop_frame(&io_manager.queue_priority_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Frame successfully retrieved from the control queue.
        }
        // If the control queue is empty, attempt to pop a frame from the general data frame queue.
        else if (pop_frame(&io_manager.queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Frame successfully retrieved from the general data queue.
        }
        else {
            Sleep(100);
            continue; 
        }

        // Assign local pointers to the frame data and source address within the popped entry.
        // This makes subsequent access to these details more convenient.
        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        // Extract and convert header fields from network byte order to host byte order.
        // This ensures the values are correctly interpreted on the local machine.
        header_delimiter = _ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = _ntohll(frame->header.seq_num);
        header_session_id = _ntohl(frame->header.session_id);

        // Convert the binary IP address from the source sockaddr_in structure to a human-readable string.
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        // Convert the source port number from network byte order to host byte order.
        src_port = _ntohs(src_addr->sin_port);

        // --- Frame Validation Checks ---
        // 1. Check for a valid start delimiter. Frames with an incorrect delimiter are considered corrupt or invalid.
        if (header_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, header_delimiter);
            continue; // Discard this frame and move to the next.
        }

        // 2. Check the frame's checksum to ensure data integrity.
        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // In a reliable transport protocol, a Negative Acknowledgment (NAK) might be sent here
            // to request retransmission of the corrupted frame. For basic datagrams, it's often discarded.
            continue; // Discard this frame and move to the next.
        }

        // Initialize the client pointer to NULL before attempting to find or add a client.
        client = NULL;

        // --- Client Association Logic ---
        // If the incoming frame is a CONNECT_REQUEST:
        if(header_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            // Attempt to find if a client with this session ID already exists.
            // The find_client function itself handles locking within its scope for client list traversal.
            // It returns an UNLOCKED pointer to the ClientData structure if found.
            client = find_client(&client_list, header_session_id);
            if(client != NULL){
                // If a client is found, it means this is likely a re-transmitted connection request from an already connected client.
                // Acquire the critical section lock for this specific client before modifying its state.
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL); // Update the client's last activity timestamp to prevent timeout.
                // Log that the client is already connected.
                fprintf(stdout, "Client %s:%d (Session ID: %d) already connected. Responding to re-connect request.\n", client->ip, src_port, client->session_id);
                // Send a connection response back to the client. The client's address (client->addr) is accessed here.
                send_connect_response(header_seq_num, 
                                        client->session_id, 
                                        server.session_timeout, 
                                        server.server_status, 
                                        server.name, 
                                        server.socket, 
                                        &client->addr
                                    );
                // Release the client's lock.
                LeaveCriticalSection(&client->lock);
                continue; // This frame has been processed; move to the next.
            }
            // If no existing client was found with that session ID, attempt to add a new client.
            // The add_client function is responsible for finding a free slot, initializing the ClientData,
            // setting up its lock, and populating initial details like session_id, IP/port, and last_activity_time.
            client = add_client(&client_list, frame, src_addr);
            if (client == NULL) {
                // If add_client returns NULL, it indicates a failure (e.g., maximum clients reached).
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached or server error.\n", src_ip, src_port);
                // TODO -> Optionally, a negative acknowledgment or server-full response could be sent here.
                continue; // Do not process this frame further as no client association was established.
            }
            // If a new client was successfully added, the 'client' pointer is now valid.
            // Processing will continue to the switch statement to handle the CONNECT_REQUEST.
        }
        // For all other frame types (i.e., not a CONNECT_REQUEST):
        else {
            // Find the client associated with the incoming frame's session ID.
            // As with the CONNECT_REQUEST path, find_client returns an UNLOCKED pointer.
            client = find_client(&client_list, header_session_id);
            if(client == NULL){
                // If no client is found for a non-connect frame, it indicates an unexpected frame.
                // This could be from a client that disconnected, an old frame, or potentially a malicious one.
                // Log and ignore such frames.
                fprintf(stdout, "Received frame (type: %u, seq: %llu) from unknown/disconnected client %s:%d. Ignoring...\n",
                        header_frame_type, header_seq_num, src_ip, src_port);
                continue; // Discard this frame and move to the next.
            }
        }
        // At this point, the 'client' pointer is guaranteed to be non-NULL and refers to an active client.

        // --- Process Payload based on Frame Type ---
        // Dispatch control to specific handlers based on the frame type.
        switch (header_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST:
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL);
                send_connect_response(header_seq_num, 
                                        client->session_id, 
                                        server.session_timeout, 
                                        server.server_status, 
                                        server.name, 
                                        server.socket, 
                                        &client->addr
                                    );
                // Release the client's lock.
                LeaveCriticalSection(&client->lock);
                break;

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
                register_ack(&io_manager.queue_priority_seq_num, client, frame, STS_KEEP_ALIVE);
                // TODO: Further processing if Keep-Alive frames carry additional state information.
                break;

            case FRAME_TYPE_FILE_METADATA:
                fprintf(stdout, "\n   FRAME_TYPE_FILE_METADATA\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %u\n   File Size: %llu\n   File Name: %s\n   File sha256: ", 
                                                    _ntohll(frame->header.seq_num), 
                                                    _ntohl(frame->header.session_id), 
                                                    _ntohl(frame->header.checksum),
                                                    _ntohl(frame->payload.file_metadata.file_id),
                                                    _ntohll(frame->payload.file_metadata.file_size),    
                                                    frame->payload.file_metadata.filename                
                );
                for(int i = 0; i < 32; i++){
                    fprintf(stdout, "%02x", (unsigned char)frame->payload.file_metadata.file_hash[i]);
                }
                fprintf(stdout,"\n");
                handle_file_metadata(client, frame, &io_manager);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                handle_file_fragment(client, frame, &io_manager);
                break;

            case FRAME_TYPE_FILE_END:
                fprintf(stdout, "\n   FRAME_TYPE_FILE_END\n   Seq Num: %llu\n   Session ID: %d\n   Checksum: %d\n   File ID: %u\n   File Size: %llu\n   File sha256: ", 
                                                    _ntohll(frame->header.seq_num), 
                                                    _ntohl(frame->header.session_id), 
                                                    _ntohl(frame->header.checksum),
                                                    _ntohl(frame->payload.file_end.file_id),
                                                    _ntohll(frame->payload.file_end.file_size)         
                );
                for(int i = 0; i < 32; i++){
                    fprintf(stdout, "%02x", (unsigned char)frame->payload.file_end.file_hash[i]);
                }
                fprintf(stdout,"\n");

            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                handle_message_fragment(client, frame, &io_manager);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "Client %s:%d with session ID: %d requested disconnect...\n", client->ip, src_port, client->session_id);
                remove_client(&client_list, &io_manager, client->slot_num);
                break;

            default:
                // For any other unexpected or unhandled frame types, log an error message.
                fprintf(stderr, "Received unknown frame type: %u from %s:%d (Session ID: %u). Discarding.\n",
                        header_frame_type, src_ip, src_port, header_session_id);
                break; // End of default case.
        }
    }
    // The thread loop terminates when `server.status` is no longer `SERVER_READY`.
    _endthreadex(0);
    return 0;
}
// --- SendAck Thread Function ---
DWORD WINAPI thread_proc_ack_frame(LPVOID lpParam){

    QueueSeqNumEntry entry;

    while (server.server_status == SERVER_STATUS_READY) {
        memset(&entry, 0, sizeof(QueueSeqNumEntry));

        if(pop_seq_num(&io_manager.queue_priority_seq_num, &entry) == RET_VAL_SUCCESS){
            // Attempt to pop a sequence number entry from the control queue first.
        } else if(pop_seq_num(&io_manager.queue_seq_num, &entry) == RET_VAL_SUCCESS){
            // If no control ACK is pending, try to pop from the regular data sequence number queue.
        } else {
            // If both queues are empty, there's no ACK to send right now.
            Sleep(100);
            continue;
        }
        // Send the ACK frame using the details retrieved from the queue entry.
        // - entry.seq_num: The sequence number being acknowledged.
        // - entry.session_id: The session ID to which this ACK belongs.
        // - entry.op_code: The operation code (ACK or NAK).
        // - server.socket: The UDP socket used for sending.
        // - &entry.addr: The destination address of the client.
        send_ack(entry.seq_num, entry.session_id, entry.op_code, server.socket, &entry.addr);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Client timeout thread function ---
DWORD WINAPI thread_proc_client_timeout(LPVOID lpParam){

    time_t time_now; // Declares a variable to store the current timestamp.

    ClientList *lp_client_list = (ClientList*)lpParam; // Cast the parameter to ClientList.

    // Main loop of the thread. This loop continuously runs as long as the server's status
    // is set to SERVER_READY. It is responsible for periodically checking client activity
    // and disconnecting inactive clients.
    while(server.server_status == SERVER_STATUS_READY) {
        EnterCriticalSection(&lp_client_list->lock);
        // Iterate through each possible client slot, from 0 up to MAX_CLIENTS - 1.
        // Each 'slot' represents a potential connection slot for a client.
        for(int slot = 0; slot < MAX_CLIENTS; slot++){
            time_now = time(NULL); // Get the current time. This is done for each client check
                                   // to ensure the most up-to-date time is used for timeout calculation.

            EnterCriticalSection(&lp_client_list->client[slot].lock);

            // Check if the current client slot is marked as SLOT_FREE.
            // If a slot is free, it means there's no active client in it, so no timeout check is needed.
            if(lp_client_list->client[slot].slot_status == SLOT_FREE){
                LeaveCriticalSection(&lp_client_list->client[slot].lock);
                continue; // Skip to the next client slot.
            }

            // Calculate the duration of inactivity for the client in this slot.
            // Compare this inactivity duration with the configured server.session_timeout.
            // If the client's last activity was more recent than the timeout period, it's still considered active.
            if(time_now - (time_t)lp_client_list->client[slot].last_activity_time < (time_t)server.session_timeout){
                LeaveCriticalSection(&lp_client_list->client[slot].lock);
                continue; // Skip to the next client slot.
            }

            // If the code reaches this point, it means the client in the current slot has timed out.
            LeaveCriticalSection(&lp_client_list->client[slot].lock);

            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", lp_client_list->client[slot].session_id);

            // Send a disconnect control message to the timed-out client's address.
            // This is a best-effort attempt to inform the client that it has been disconnected by the server.
            send_disconnect(lp_client_list->client[slot].session_id, server.socket, &lp_client_list->client[slot].addr);

            // Call the `remove_client` function to clean up all resources associated with this client slot
            // and mark the slot as free. This function is expected to be thread-safe in its own implementation.
            remove_client(lp_client_list, &io_manager, slot);
        }
        // Pause the thread's execution for 1000 milliseconds (1 second).
        // This prevents the thread from consuming 100% CPU by continuously looping and polling.
        // It sets the frequency at which client timeouts are checked.
        LeaveCriticalSection(&lp_client_list->lock);
        Sleep(1000);
    }

    // After the `while` loop condition (`server.status == SERVER_READY`) becomes false,
    _endthreadex(0);
    return 0; // Return 0 to indicate that the thread terminated successfully.
}
// --- Thread for writing file streams to disk ---
DWORD WINAPI thread_proc_file_stream_io(LPVOID lpParam) {

    ClientList *client_list = (ClientList*)lpParam; // Cast the parameter to ClientList.

    // Main loop: Continue as long as the server is in a READY state.
    // This thread continuously polls for completed file blocks to write to disk.
    while (server.server_status == SERVER_STATUS_READY) {
        EnterCriticalSection(&client_list->lock);
        // Iterate through all possible client slots.
        for(int i = 0; i < MAX_CLIENTS; i++){

            Client *client = &client_list->client[i];
            // Acquire the critical section lock for the current client.
            // This prevents other threads (e.g., connection handler) from modifying
            // client data while this thread is inspecting it.
            EnterCriticalSection(&client->lock);

            // Check if the client slot is currently connected.
            if(client->connection_status == CLIENT_CONNECTED){
                // Iterate through all possible file stream slots for the current client.
                for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
                    
                    FileStream *fstream = &client->file_stream[j];
                    
                    //FileStream *fstream = &client_list.client[i].file_stream[j];
                    // Acquire the critical section lock for the current file stream.
                    // This protects the file stream's state (bitmap, flags, counters, etc.)
                    // from concurrent access by other threads (e.g., data reception thread).
                    EnterCriticalSection(&fstream->lock);

                    // Check if the file stream is currently active/busy with a transfer.
                    if(fstream->busy){
                        // Iterate through all bitmap entries (chunks) for the current file stream.
                        for(long long k = 0; k < fstream->bitmap_entries_count; k++){

                            // --- CONSOLIDATED FILE WRITING LOGIC ---
                            // Calculate the absolute file offset where this chunk should be written.
                            uint64_t file_offset = k * FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK; // Use constant
                            // Get the memory buffer associated with this chunk.
                            char* buffer = fstream->pool_block_file_chunk[k];
                            uint64_t buffer_size;    // Variable to store the actual size of the chunk to write.
                            //uint8_t new_flag_value; // Variable to store the new flag status after writing.

                            // Case 1: This is the trailing (last, potentially partial) chunk.
                            // Check if:
                            //   a) The fstream is marked as having a trailing chunk.
                            //   b) The trailing chunk's expected bytes have all been received.
                            //   c) The current chunk's flag indicates it's the trailing chunk AND it hasn't been written yet.
                            if (fstream->trailing_chunk && fstream->trailing_chunk_complete && (fstream->flag[k] == CHUNK_TRAILING)) {
                                buffer_size = fstream->trailing_chunk_size; // Use the specific calculated size for the trailing chunk.
                            //    new_flag_value = CHUNK_TRAILING | CHUNK_WRITTEN; // Mark it as trailing and now written.
                                //fprintf(stdout, "Writing trailing chunk bytes: %llu, chunk index: %llu\n", buffer_size, k);

                            // Case 2: This is a full-sized chunk (not trailing).
                            // Check if:
                            //   a) All fragments within this 64-bit bitmap entry have been received (~0ULL means all bits set).
                            //   b) The current chunk's flag indicates it's a body chunk AND it hasn't been written yet.
                            } else if(fstream->bitmap[k] == ~0ULL && (fstream->flag[k] == CHUNK_BODY)) {
                                buffer_size = FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK; // Full chunk size.
                            //    new_flag_value = CHUNK_BODY | CHUNK_WRITTEN; // Mark it as a body chunk and now written.
                                // fprintf(stdout, "Writing complete chunk bytes: %llu, chunk index: %llu\n", buffer_size, k); // Optional for full chunks
                            }
                            //new_flag_value = CHUNK_WRITTEN;
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
                                fprintf(stderr, "Error: FILE pointer is null for chunk %llu. Session ID: %u, File ID: %u\n", k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FP; // Set a specific error code.
                                file_cleanup_stream(fstream, &io_manager); // Clean up the entire file stream due to the error.
                                break; // Exit the 'k' loop (current file's chunks) and move to the next file stream.
                            }

                            // Attempt to seek to the correct offset in the file.
                            if (_fseeki64(fstream->fp, file_offset, SEEK_SET) != 0) {
                                fprintf(stderr, "Error: Failed to seek to offset %llu for chunk %llu. Session ID: %u, File ID: %u\n", file_offset, k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FSEEK; // Set a specific error code.
                                file_cleanup_stream(fstream, &io_manager); // Clean up the entire file stream.
                                break; // Exit the 'k' loop.
                            }

                            // Write the chunk data from the buffer to the file.
                            size_t written = fwrite(buffer, 1, buffer_size, fstream->fp);
                            // Check if the number of bytes written matches the expected buffer size.
                            if (written != buffer_size) {
                                fprintf(stderr, "Error: Failed to write data (expected %llu, wrote %llu) for chunk %llu. Session ID: %u, File ID: %u\n", buffer_size, written, k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FWRITE; // Set a specific error code.
                                file_cleanup_stream(fstream, &io_manager); // Clean up the entire file stream.
                                break; // Exit the 'k' loop.
                            }
                            fstream->bytes_written += written; // Accumulate the total bytes written to disk.
                            fstream->flag[k] |= CHUNK_WRITTEN;//= new_flag_value; // Update the chunk's flag to reflect it has been written.

                            // Return the memory buffer for this chunk back to the pre-allocated pool.
                            pool_free(&io_manager.pool_file_chunk, fstream->pool_block_file_chunk[k]);
                            // Set the pointer in the FileStream structure to NULL to prevent dangling pointers
                            // and to indicate that this chunk's buffer has been released.
                            fstream->pool_block_file_chunk[k] = NULL;

                            // After attempting to write all available chunks for this file stream:
                            // Check if the total bytes received equals the total file size AND
                            // if the total bytes written to disk also equals the total file size.
                            fstream->file_complete = (fstream->bytes_received == fstream->f_size) && (fstream->bytes_written == fstream->f_size);

                            // If the file is now completely received and written:
                            if(fstream->file_complete){
                                // Update the status in the UID hash table (if used for tracking file transfer status).
                                update_uid_status_hash_table(io_manager.uid_hash_table, &io_manager.uid_ht_mutex, fstream->s_id, fstream->f_id, UID_RECV_COMPLETE);
                                // fprintf(stdout, "[INFO] Transfer finished, created file: %s, bytes: %llu\n", fstream->fn, fstream->bytes_written);
                                // fprintf(stdout,"Cleaning up file stream: %d\n", j);
                                file_cleanup_stream(fstream, &io_manager); // Perform final cleanup for the completed file stream.
                                break;
                            }

                        } //end of looping through BITMAP ENTRIES

                    } // check busy
                    // Release the critical section lock for the current file stream.
                    LeaveCriticalSection(&fstream->lock);
                } // END of looping through FILE STREAMS
            } //check client is connected
            // Release the critical section lock for the current client.
            LeaveCriticalSection(&client->lock);
        } // END of looping through CLIENTS
        LeaveCriticalSection(&client_list->lock);
        Sleep(100); // Pause execution for 100 milliseconds to avoid busy-waiting and reduce CPU usage.
                    // Consider event-based signaling (e.g., condition variables) for more efficient
                    // and responsive operation in high-performance scenarios.
    }
    _endthreadex(0); // Properly exit the thread, returning its control to the system.
    return 0; // Return 0 indicating successful thread termination.
}
// --- Process server command ---
DWORD WINAPI thread_proc_server_command(LPVOID lpParam){
    
    char cmd;
    
    while (server.server_status == SERVER_STATUS_READY){

        fprintf(stdout,"Waiting for command...\n");
        cmd = getchar();
        switch(cmd) {
            case 's':
            case 'S':
            //check what file streams are still open
                check_open_file_stream(&client_list);
                break;

            case 'q':
            case 'Q':
                server.server_status = SERVER_STATUS_NONE;
                break;

            default:
                break;
            }
        Sleep(200);
    }

    _endthreadex(0);
    return 0;
}


// Register an acknowledgment (ACK) or negative acknowledgment for a received frame.
void register_ack(QueueSeqNum *queue, Client *client, UdpFrame *frame, uint8_t op_code) {
    // Create a new QueueSeqNumEntry structure to store the ACK/NAK details.
    QueueSeqNumEntry entry;
        // Extract the sequence number from the frame header, converting from network to host byte order.
    entry.seq_num = _ntohll(frame->header.seq_num);
    // Set the operation code (e.g., ACK or NAK) as provided by the caller.
    entry.op_code = op_code;
    // Extract the session ID from the frame header, converting from network to host byte order.
    entry.session_id = _ntohl(frame->header.session_id);
    // Copy the client's address (destination for the ACK/NAK) into the entry.
    // This is where the client's `addr` member is accessed.
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
    // Push the constructed entry into the specified sequence number queue.
    // This function (push_seq_num) is expected to handle its own locking for the queue.
    push_seq_num(queue, &entry);
}
// Clean up the file stream resources after a file transfer is completed or aborted.
void file_cleanup_stream(FileStream *fstream, ServerIOManager* io_manager){

    if(fstream == NULL){
        fprintf(stderr, "ERROR: Trying to clean a NULL pointer file stream\n");
        return;
    }
    // Check if the file pointer is valid (not NULL) AND if the stream was busy
    // AND if the file transfer was NOT completed successfully.
    // This condition implies an abnormal termination of the file transfer.
    if(fstream->fp && fstream->busy && !fstream->file_complete){
        fclose(fstream->fp); // Close the file stream.
        remove(fstream->fn); // Delete the partially written file from disk.
        fstream->fp = NULL;  // Set the file pointer to NULL after closing/removing.
    }

    // Ensure all buffered data is written to disk before closing.
    if(fstream->fp != NULL){
        // If the file pointer is valid and the file transfer was completed successfully,
        // we still need to ensure all data is flushed to disk before closing.
        if(fflush(fstream->fp) != 0){
            fprintf(stderr, "Error flushing the file to disk. File is still in use.\n");
        } else {
            // If the flush was successful, close the file.
            int fclosed = fclose(fstream->fp);
            Sleep(50); // Sleep for 50 milliseconds to ensure the file is properly closed before proceeding.
            if(fclosed != 0){
                fprintf(stderr, "Error closing the file stream: %s (errno: %d)\n", fstream->fn, errno);
            }
            fstream->fp = NULL; // Set the file pointer to NULL after closing.
        }
    }

    // Check if the bitmap array was allocated.
    if(fstream->bitmap != NULL){
        //fprintf(stdout, "Freeing bitmap memory block for file stream: %s\n", fstream->fn);
        //memset(fstream->bitmap, 0, fstream->bitmap_entries_count * sizeof(uint64_t)); // Clear the bitmap.
        free(fstream->bitmap); // Free the memory allocated for the bitmap.
        fstream->bitmap = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    // Check if the flag array was allocated.
    if(fstream->flag != NULL){
        //fprintf(stdout, "Freeing flags memory block for file stream: %s\n", fstream->fn);
        //memset(fstream->flag, 0, fstream->bitmap_entries_count * sizeof(uint8_t)); // Clear the flags.
        free(fstream->flag); // Free the memory allocated for the flags.
        fstream->flag = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }    
    // Loop through all possible chunk memory blocks that might have been allocated.
    // The `fstream->bitmap_entries_count` determines the valid range of indices.
    for(long long k = 0; k < fstream->bitmap_entries_count; k++){
        // Return each individual chunk memory block back to the global memory pool.
        // The pool_free function will handle checking for NULL pointers internally.
        if(fstream->pool_block_file_chunk[k] != NULL){
            // Free the memory allocated for each chunk back to the pool.
            //fprintf(stdout, "Freeing chunk memory block at index: %llu\n", k);
            //memset(fstream->pool_block_file_chunk[k], 0, FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK); // Clear the chunk memory block.
            // Free the chunk memory block back to the pool.
            pool_free(&io_manager->pool_file_chunk, fstream->pool_block_file_chunk[k]);
        }
        fstream->pool_block_file_chunk[k] = NULL;
    }
    // Check if the array of chunk memory block pointers was allocated.
    if(fstream->pool_block_file_chunk != NULL){
        // Free the memory allocated for the array of char* pointers itself.
        free(fstream->pool_block_file_chunk);
    }
    fstream->pool_block_file_chunk = NULL; // Set the pointer to NULL.
    // Reset the stream's state variables to their default/initial values.
    fstream->busy = FALSE;
    fstream->file_complete = FALSE;
    fstream->stream_err = STREAM_ERR_NONE; // Reset error status.
    fstream->s_id = 0; // Reset session ID.
    fstream->f_id = 0; // Reset file ID.
    fstream->f_size = 0; // Reset total file size.
    fstream->fragment_count = 0; // Reset fragment count.
    fstream->bytes_received = 0; // Reset bytes received counter.
    fstream->bytes_written = 0; // Reset bytes written counter.
    fstream->bitmap_entries_count = 0; // Reset bitmap entries count.
    fstream->trailing_chunk = FALSE; // Reset trailing chunk flag.
    fstream->trailing_chunk_complete = FALSE; // Reset trailing chunk completion flag.
    fstream->trailing_chunk_size = 0; // Reset trailing chunk size.
    memset(fstream->fn, 0, PATH_SIZE); // Clear the file name buffer by filling it with zeros.
    return;
}

void message_cleanup_stream(MsgStream *mstream, ServerIOManager* io_manager){

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
    mstream->s_id = 0; // Reset session ID.
    mstream->m_id = 0; // Reset message ID.
    mstream->m_len = 0; // Reset message length.
    mstream->fragment_count = 0; // Reset fragment count.
    mstream->chars_received = 0; // Reset characters received counter.
    mstream->bitmap_entries_count = 0; // Reset bitmap entries count.
    memset(mstream->file_name, 0, PATH_SIZE); // Clear the file name buffer by filling it with zeros.
    return;

}


// Clean client resources
void cleanup_client(Client *client, ServerIOManager* io_manager){
    
    if(client == NULL){
        fprintf(stdout, "Error: Tried to remove null pointer client!\n");
        return;
    }
    EnterCriticalSection(&client->lock);
    // Iterate through all possible file stream slots associated with this client.
    // MAX_CLIENT_FILE_STREAMS defines the maximum number of concurrent file transfers a single client can have.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Call the dedicated cleanup function for the current file stream.
        // This function will free memory, close files, and reset the file stream's state.
        file_cleanup_stream(&client->file_stream[i], io_manager);
        // Release the critical section lock for the current file stream.
        LeaveCriticalSection(&client->file_stream[i].lock);
    }
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        // Acquire the critical section lock for the current message stream.
        EnterCriticalSection(&client->msg_stream[i].lock);
        // Call the dedicated cleanup function for the current message stream.
        // This function will free memory, close files, and reset the message stream's state.
        message_cleanup_stream(&client->msg_stream[i], io_manager);
        // Release the critical section lock for the current message stream.
        LeaveCriticalSection(&client->msg_stream[i].lock);
    }


    // Reset the client's network address structure by filling it with zeros.
    memset(&client->addr, 0, sizeof(struct sockaddr_in));
    // Reset the client's IP address string by filling it with zeros.
    memset(&client->ip, 0, INET_ADDRSTRLEN);
    // Reset the client's port number to zero.
    client->port = 0;
    // Reset the client's unique identifier to zero.
    client->client_id = 0;
    // Reset the client's name buffer by filling it with zeros.
    memset(&client->name, 0, NAME_SIZE);
    // Reset any client-specific flags to zero.
    client->flag = 0;
    // Set the client's connection status to disconnected.
    client->connection_status = CLIENT_DISCONNECTED;
    // Update the last activity time to the current time. This can be useful for
    // tracking when the client slot became free or was last active.
    client->last_activity_time = time(NULL);
    // Reset the client's assigned slot number to zero.
    client->slot_num = 0;
    // Mark the client slot as free, indicating it's available for a new connection.
    client->slot_status = SLOT_FREE;
    LeaveCriticalSection(&client->lock);
    return;
}
// Check for any open file streams across all clients.
void check_open_file_stream(ClientList* client_list){
    for(int i = 0; i < MAX_CLIENTS; i++){
        for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
            FileStream *fstream = &client_list->client[i].file_stream[j];
            if(client_list->client[i].file_stream[j].busy == TRUE){
                fprintf(stdout, "Client: %d - File stream still open: %d\n", i, j);
                fprintf(stdout, "Bytes received: %llu, Bytes written: %llu\n", fstream->bytes_received, fstream->bytes_written);
                for(int k = 0; k < fstream->bitmap_entries_count; k++){
                    if(fstream->bitmap[k] != ~0ULL){
                        fprintf(stdout, "---bitmap[%d] = %llx\n", k, fstream->bitmap[k]);
                    }
                }
            }
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
    while (server.server_status == SERVER_STATUS_READY) {

        Sleep(250); // Prevent busy-waiting
   
        // printf("\033[1A\r\033[2K-- QP Hd: %u - Tl: %u | Q Hd: %u - Tl: %u, Recv F: %llu, Sent F: %llu, Pop F: %llu --\n", 
        //                     io_manager.queue_priority_frame.head, io_manager.queue_priority_frame.tail, 
        //                     io_manager.queue_frame.head, io_manager.queue_frame.tail,
        //                     frame_counters.total_recv,
        //                     frame_counters.total_sent,
        //                     frame_counters.queue_pop
        //                 );
        fflush(stdout);

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}