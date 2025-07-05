
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>   // For modern IP address functions (inet_pton, inet_ntop)
#include <time.h>
#include <process.h>    // For _beginthreadex
#include <windows.h>    // For Windows-specific functions like CreateThread, Sleep
#include <iphlpapi.h>

#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library
#pragma comment(lib, "iphlpapi.lib")

#include "frames.h"
#include "checksum.h"
#include "queue.h"
#include "bitmap.h"
#include "mem_pool.h"
#include "safefileio.h"
#include "hash.h"

// --- Constants 
#define SERVER_PORT                     12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS             100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC      120       // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME                     "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                     20
#define FILE_PATH                       "E:\\out_file.txt"

#define FRAGMENTS_PER_CHUNK             (64ULL)
#define CHUNK_TRAILING                  (1u << 7) // 0b10000000
#define CHUNK_BODY                      (1u << 6) // 0b01000000
#define CHUNK_WRITTEN                   (1u << 0) // 0b00000001
#define CHUNK_NONE                      (0)       // 0b00000000

#define BLOCK_SIZE_CHUNK                ((uint64_t)(FILE_FRAGMENT_SIZE * 64))
#define BLOCK_COUNT_CHUNK               ((uint64_t)(2048))

typedef uint8_t ServerStatus;
enum ServerStatus {
    SERVER_STOP = 0,
    SERVER_BUSY = 2,
    SERVER_READY = 3,
    SERVER_ERROR = 4    
};

typedef uint8_t ClientConnection;
enum ClientConnection {
    CLIENT_DISCONNECTED = 0,
    CLIENT_CONNECTED = 1
};

typedef uint8_t StreamChannelError;
enum StreamChannelError{
    STREAM_ERR_NONE = 0,
    STREAM_ERR_FP = 1,
    STREAM_ERR_FSEEK = 2,
    STREAM_ERR_FWRITE = 3,

    STREAM_ERR_BITMAP_MALLOC = 10,
    STREAM_ERR_FLAG_MALLOC = 11,
    STREAM_ERR_CHUNK_PTR_MALLOC = 12,

    STREAM_ERR_FILENAME = 20
};

typedef uint8_t ClientSlotStatus;
enum ClientSlotStatus {
    SLOT_FREE = 0,
    SLOT_BUSY = 1
};

typedef struct{
    SOCKET socket;
    struct sockaddr_in addr;            // Server address structure
    ServerStatus status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    volatile long session_id_counter;   // Global counter for unique session IDs
    char name[NAME_SIZE];               // Human-readable server name
}ServerData;

typedef struct{
    FILETIME ft;
    ULARGE_INTEGER prev_uli;
    unsigned long long prev_microseconds;
    ULARGE_INTEGER crt_uli;
    unsigned long long crt_microseconds;
    float file_transfer_speed;
    float file_transfer_progress;

    float crt_bytes_received;
    float prev_bytes_received;
}Statistics;

typedef struct{
    char *buffer;
    uint64_t *bitmap;
    uint32_t s_id;
    uint32_t m_id;
    uint32_t m_len;
    uint32_t chars_received;
    uint64_t fragment_count;
    uint64_t bitmap_entries_count;

    char file_name[PATH_SIZE];

    CRITICAL_SECTION lock;              // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.
}MsgStream;

typedef struct{
    BOOL busy;                          // Indicates if this stream channel is currently in use for a transfer.
    BOOL file_complete;                 // True if the entire file has been received and written.
    uint8_t stream_err;                 // Stores an error code if something goes wrong with the stream.

    uint64_t *bitmap;                   // Pointer to an array of uint64_t, where each bit represents a file fragment.
                                        // A bit set to 1 means the fragment has been received.
    uint8_t* flag;                      // Pointer to an array of uint8_t, used for custom flags per chunk/bitmap entry.
                                        // (e.g., 0b10000000 for last partial, 0b00000001 for written, etc.)
    char** pool_block_file_chunk;             // Pointer to an array of char pointers, where each char* points to a
                                        // memory block holding a complete chunk of data (64 fragments).
    uint32_t s_id;                       // Session ID associated with this file stream.
    uint32_t f_id;                       // File ID, unique identifier for the file associated with this file stream.
    uint64_t f_size;                     // Total size of the file being transferred.
    uint64_t fragment_count;            // Total number of fragments in the entire file.
    uint64_t bytes_received;            // Total bytes received for this file so far.
    uint64_t bytes_written;             // Total bytes written to disk for this file so far.
    uint64_t bitmap_entries_count;      // Number of uint64_t entries in the bitmap array.
                                        // (Total fragments / 64, rounded up)

    BOOL trailing_chunk;                // True if the last bitmap entry represents a partial chunk (less than 64 fragments).
    BOOL trailing_chunk_complete;       // True if all bytes for the last, potentially partial, chunk have been received.
    uint64_t trailing_chunk_size;       // The actual size of the last chunk (if partial).

    char fn[PATH_SIZE];                 // Array to store the file name/path.
    FILE *fp;                           // File pointer for the file being written to disk.

    CRITICAL_SECTION lock;              // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.
}FileStream;

typedef struct {  
    struct sockaddr_in addr;            // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    
    uint32_t client_id;                 // Unique ID received from the client
    char name[NAME_SIZE];               // Optional: human-readable identifier received from the client
    uint8_t flag;                       // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;
 
    uint32_t session_id;                // Unique ID assigned by the server for this clients's session
    volatile time_t last_activity_time; // Last time the client sent a frame (for timeout checks)             

    uint32_t slot_num;                  // Index of the slot the client is connected to [0..MAX_CLIENTS-1]
    uint8_t slot_status;                // 0->FREE; 1->BUSY
 
    MsgStream msg_stream[MAX_CLIENT_MESSAGE_STREAMS];
    FileStream file_stream[MAX_CLIENT_FILE_STREAMS];
     
    Statistics statistics;

    CRITICAL_SECTION lock;

} ClientData;

typedef struct{
    ClientData client[MAX_CLIENTS];      // Array of connected clients
    CRITICAL_SECTION lock;         // For thread-safe access to connected_clients
}ClientList;


ServerData server;
ClientList client_list;

QueueFrame queue_frame;
QueueFrame queue_frame_ctrl;
QueueSeqNum queue_seq_num;
QueueSeqNum queue_seq_num_ctrl;

HANDLE receive_frame_thread;
HANDLE process_frame_thread; 
HANDLE ack_thread; 
HANDLE client_timeout_thread;
HANDLE server_command_thread;
HANDLE check_complete_block_in_bitmap_thread;

UniqueIdentifierNode *uid_hash_table[HASH_SIZE_UID] = {NULL};
MemPool pool_file_chunk;


const char *server_ip = "10.10.10.2"; // IPv4 example

// Client management functions
ClientData* find_client(ClientList *list, const uint32_t session_id);
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
int remove_client(ClientList *list, const uint32_t session_id);

void file_cleanup_stream(FileStream *entry);
void cleanup_client(ClientData *client);

void update_statistics(ClientData *client);

static void register_ack(QueueSeqNum *queue, ClientData *client, UdpFrame *frame, uint8_t op_code);

// Handle message fragment helper functions
static int msg_match_fragment(ClientData *client, UdpFrame *frame);
static int msg_validate_fragment(ClientData *client, const int index, UdpFrame *frame);
static int msg_get_available_stream_channel(ClientData *client);
static int msg_init_stream(MsgStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len);
static void msg_attach_fragment(MsgStream *entry, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len);
static int msg_check_completion_and_record(MsgStream *entry, const uint32_t session_id);

static int file_check_metadata(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id);
static int file_match_fragment(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id);
static int file_get_available_stream_channel(ClientData *client);
static void file_attach_fragment_to_chunk(FileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size);
static int file_stream_init(FileStream *fstream, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size);

int handle_file_metadata(ClientData *client, UdpFrame *frame);
int handle_file_fragment(ClientData *client, UdpFrame *frame);
int handle_message_fragment(ClientData *client, UdpFrame *frame);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI ack_thread_func(LPVOID lpParam);
unsigned int WINAPI client_timeout_thread_func(LPVOID lpParam);
unsigned int WINAPI server_command_thread_func(LPVOID lpParam);
unsigned int WINAPI check_complete_blocks_in_bitmap_func(LPVOID lpParam);


void get_network_config(){
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
// Safe write function to handle errors
int file_fragment_write(const char *path, const void *buffer, size_t size, size_t offset) {
    
    FILE *fp = fopen(path, "ab");

    if (!fp) {
        fprintf(stderr, "Error: Failed to open file\n");
        return RET_VAL_ERROR;
    }

    // Move file pointer to the correct offset
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        fprintf(stderr, "Error: Failed to seek to offset %zu\n", offset);
        return RET_VAL_ERROR;
    }

    // Write data to file
    size_t written = fwrite(buffer, 1, size, fp);
    if (written != size) {
        fclose(fp);
        fprintf(stderr, "Error: Failed to write data (expected %zu, wrote %zu)\n", size, written);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    return RET_VAL_SUCCESS;  // Success
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
// Server initialization and management functions
int init_server(){
    
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
    }
    memset(&client_list, 0, sizeof(ClientList));
    server.status = SERVER_BUSY;

    server.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server.socket == INVALID_SOCKET) {
        fprintf(stderr, "socket failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return RET_VAL_ERROR;
    }
    server.addr.sin_family = AF_INET;
    server.addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server.addr.sin_addr);
 
    if (bind(server.socket, (SOCKADDR*)&server.addr, sizeof(server.addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed with error: %d\n", WSAGetLastError());
        closesocket(server.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);
    queue_frame_ctrl.head = 0;
    queue_frame_ctrl.tail = 0;
    InitializeCriticalSection(&queue_frame_ctrl.mutex);
    queue_seq_num.head = 0;
    queue_seq_num.tail = 0;
    InitializeCriticalSection(&queue_seq_num.mutex);
    queue_seq_num_ctrl.head = 0;
    queue_seq_num_ctrl.tail = 0;
    InitializeCriticalSection(&queue_seq_num_ctrl.mutex);
    
    server.session_timeout = CLIENT_SESSION_TIMEOUT_SEC;
    server.session_id_counter = 0xFF;
    snprintf(server.name, NAME_SIZE, "%.*s", NAME_SIZE - 1, SERVER_NAME);
    server.status = SERVER_READY;

    for(int i = 0; i < MAX_CLIENTS; i++){
        InitializeCriticalSection(&client_list.client[i].lock);
        for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
            InitializeCriticalSection(&client_list.client[i].file_stream[j].lock);
            InitializeCriticalSection(&client_list.client[i].msg_stream[j].lock);
        }
    }
    InitializeCriticalSection(&client_list.lock);

    pool_file_chunk.block_size = BLOCK_SIZE_CHUNK;
    pool_file_chunk.block_count = BLOCK_COUNT_CHUNK;
    pool_init(&pool_file_chunk);

    printf("Server listening on port %d...\n", SERVER_PORT);
    return RET_VAL_SUCCESS;
}
// --- Start server threads ---
int start_threads() {
    // Create threads for receiving and processing frames
    receive_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, NULL, 0, NULL);
    if (receive_frame_thread == NULL) {
        fprintf(stderr, "Failed to create receive thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    ack_thread = (HANDLE)_beginthreadex(NULL, 0, ack_thread_func, NULL, 0, NULL);
    if (ack_thread == NULL) {
        fprintf(stderr, "Failed to create ack thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client_timeout_thread = (HANDLE)_beginthreadex(NULL, 0, client_timeout_thread_func, NULL, 0, NULL);
    if (client_timeout_thread == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    client_timeout_thread = (HANDLE)_beginthreadex(NULL, 0, client_timeout_thread_func, NULL, 0, NULL);
    if (client_timeout_thread == NULL) {
        fprintf(stderr, "Failed to create client timeout thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    check_complete_block_in_bitmap_thread = (HANDLE)_beginthreadex(NULL, 0, check_complete_blocks_in_bitmap_func, NULL, 0, NULL);
    if (check_complete_block_in_bitmap_thread == NULL) {
        fprintf(stderr, "Failed to create bitmap check thread. Error: %d\n", GetLastError());
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}
// --- Server shutdown ---
void shutdown_server() {

    server.status = SERVER_STOP;

    if (ack_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(ack_thread, INFINITE);
        CloseHandle(ack_thread);
    }
    fprintf(stdout,"ack thread closed...\n");
    if (process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(process_frame_thread, INFINITE);
        CloseHandle(process_frame_thread);
    }
    if (receive_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(receive_frame_thread, INFINITE);
        CloseHandle(receive_frame_thread);
    }
    fprintf(stdout,"receive frame thread closed...\n");
    fprintf(stdout,"process thread closed...\n");
    if (client_timeout_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(client_timeout_thread, INFINITE);
        CloseHandle(client_timeout_thread);
    }
    fprintf(stdout,"client timeout thread closed...\n");
    if (server_command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(server_command_thread, INFINITE);
        CloseHandle(server_command_thread);
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
    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_frame_ctrl.mutex);
    DeleteCriticalSection(&queue_seq_num.mutex);
    DeleteCriticalSection(&queue_seq_num_ctrl.mutex);

    closesocket(server.socket);
    WSACleanup();
    printf("Server shut down!\n");
}
// --- MAIN FUNCTION ---
int main() {
//    get_network_config();
    init_server();
    start_threads();
    // Main server loop for general management, timeouts, and state updates
    while (server.status == SERVER_READY) {

        Sleep(250); // Prevent busy-waiting
        update_statistics(&client_list.client[0]);

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}

// Find client by session ID
ClientData* find_client(ClientList *list, const uint32_t session_id) {
    // Search for a client within the provided ClientList that matches the given session ID.

    // Iterate through each possible client slot in the list.
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        
        // Acquire the critical section lock for the current client slot.
        EnterCriticalSection(&list->client[slot].lock);

        // Check if the current client slot is marked as SLOT_FREE.
        // A free slot indicates no active client, so it cannot be the one we are searching for.
        if(list->client[slot].slot_status == SLOT_FREE){
            // If the slot is free, release its lock before moving to the next slot.
            LeaveCriticalSection(&list->client[slot].lock);
            continue; // Move to the next slot in the loop.
        }
        // If the slot is not free, compare its stored session ID with the target session_id.
        if(list->client[slot].session_id == session_id){
            // If a client with a matching session ID is found, release its lock.
            LeaveCriticalSection(&list->client[slot].lock);
            // Return a pointer to the found ClientData structure.
            return &list->client[slot];
        }
        // If the slot is not free, but its session ID does not match,
        // release the lock for this slot and continue the search.
        LeaveCriticalSection(&list->client[slot].lock);
    }
    // If the loop completes without finding any client matching the session ID,
    // return NULL to indicate that no such client is currently active.
    return NULL;
}
// Add a new client
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    
    uint32_t free_slot = 0; // Initializes a counter to search for an available client slot.
    // Loop through the client list to find the first available (free) slot.
    while(free_slot < MAX_CLIENTS){ // Continues as long as the current slot index is within the maximum allowed clients.
        if(list->client[free_slot].slot_status == SLOT_FREE) { // Checks if the current slot is marked as free.
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
    ClientData *new_client = &list->client[free_slot]; 
    
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
    new_client->client_id = ntohl(recv_frame->payload.request.client_id); 
    // Assigns a unique session ID to the new client by atomically incrementing a global counter.
    new_client->session_id = (uint32_t)InterlockedIncrement(&server.session_id_counter); 
    // Copies the flag from the received frame's request payload to the new client's data.
    new_client->flag = recv_frame->payload.request.flag;
    
    // Formats and copies the client's name from the received frame's payload into the new client's structure.
    // 'snprintf' is used for safe string copying, preventing buffer overflows.
    snprintf(new_client->name, NAME_SIZE, "%.*s", NAME_SIZE - 1, recv_frame->payload.request.client_name);

    // Converts the client's IP address from binary form to a human-readable string (IPv4).
    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    // Converts the client's port number from network byte order to host byte order.
    new_client->port = ntohs(client_addr->sin_port);

    // Prints a log message to standard output, announcing the addition of a new client with their IP, port, and assigned session ID.
    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->session_id);

    // Leaves the critical section, releasing the lock on the 'new_client' data structure.
    // This makes the newly initialized client data accessible to other threads.
    LeaveCriticalSection(&new_client->lock);

    return new_client; // Returns a pointer to the newly added and initialized ClientData structure.
}
// Remove a client
int remove_client(ClientList *list, const uint32_t slot) {
    // Search for the client with the given session ID
    // Checks if the provided ClientList pointer is NULL, indicating an invalid list.
    if(list == NULL){
        fprintf(stderr, "\nInvalid client pointer!\n"); // Prints an error message to standard error.
        return RET_VAL_ERROR; // Returns an error value.
    }
    // Validates the 'slot' index to ensure it is within the permissible range [0, MAX_CLIENTS - 1].
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "\nInvalid client slot nr:  %d", slot); // Prints an error message if the slot number is out of bounds.
        return RET_VAL_ERROR; // Returns an error value.
    }
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", list->client[slot].session_id, list->client[slot].slot_num);
    // Enters a critical section associated with the specific client slot.    
    EnterCriticalSection(&list->client[slot].lock);
    // Calls a helper function to perform cleanup operations for the client in the specified slot.
    cleanup_client(&list->client[slot]);
    // Leaves the critical section, releasing the lock on the client's data.
    LeaveCriticalSection(&list->client[slot].lock);
    fprintf(stdout, "\nRemoved client successfully!\n");
    return RET_VAL_SUCCESS; // Returns a success value.
}

// Process received file metadata frame
int handle_file_metadata(ClientData *client, UdpFrame *frame){
    // Check if the provided client context is NULL.
    // Receiving a frame for a non-existent client is a critical error,
    // as all operations on the frame depend on a valid client session.
    if(client == NULL){
        // Log an error message indicating that the frame was received for an unknown client.
        fprintf(stdout, "Received frame for non existing client context!\n");
        // Jump to the `exit_error` label to handle cleanup and return an error status.
        goto exit_error;
    }

    // Enter a critical section. This mutex protects the `client` data structure
    EnterCriticalSection(&client->lock);

    // Update the `last_activity_time` for the client.
    client->last_activity_time = time(NULL);

    uint32_t recv_session_id = ntohl(frame->header.session_id);
    uint32_t recv_file_id = ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = ntohll(frame->payload.file_metadata.file_size);


    if(file_check_metadata(client, frame, recv_session_id, recv_file_id) == RET_VAL_ERROR){
        fprintf(stderr, "Duplicated file metadata received!\n");
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_DUPLICATE_FRAME);
        // If the metadata is invalid or duplicated, register an acknowledgment (ACK)
        goto exit_error;
    }

    // Check if the specific file transfer (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This check prevents redundant processing or re-acknowledgment of already finished transfers.
    if(search_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        // If the transfer is already complete, register an acknowledgment (ACK)
        // with `STS_TRANSFER_COMPLETE` status. This informs the sender that
        // the file is already received and no further retransmissions of metadata are needed.
        // `queue_seq_num_ctrl` is used for control-related ACKs.
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the `exit_error` label, as no further processing is needed for this file metadata.
        goto exit_error;
    }

    // Attempt to get an available slot within the client's file stream management structure.
    // This is typically used when a client can handle multiple concurrent file transfers.
    int slot = file_get_available_stream_channel(client);
    // If `file_get_available_stream_channel` returns `RET_VAL_ERROR`, it means no free slot is available.
    if(slot == RET_VAL_ERROR){
        // Log an error message indicating that the maximum number of concurrent file transfers has been reached.
        fprintf(stderr, "Maximum file transfers reached!\n");
        // Register an ACK with `ERR_RESOURCE_LIMIT` status. This informs the sender
        // that the server is currently unable to accept more file transfers due to resource constraints.
        // `queue_seq_num_ctrl` is used for control-related ACKs.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_RESOURCE_LIMIT);
        // Jump to the `exit_error` label to clean up and return.
        goto exit_error;
    }

    // Log the details of the successfully received file metadata to standard output.
    // This provides valuable debugging and monitoring information.
    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, File Size: %llu, Fragment Size: %d\n", client->session_id, recv_file_id, recv_file_size, FILE_FRAGMENT_SIZE);

    // Initialize the file stream for the new incoming file transfer in the determined `slot`.
    // This function would typically handle opening a file on disk, setting up buffer management,
    // and initializing state for tracking received fragments.
    if(file_stream_init(&client->file_stream[slot], client->session_id, recv_file_id, recv_file_size) == RET_VAL_ERROR){
        // If file stream initialization fails, jump to the error handling block.
        goto exit_error;
    }

    // Add an entry to the unique ID hash table for this new file transfer.
    // It's marked with `UID_WAITING_FRAGMENTS` to indicate that the metadata has been received
    // and the server is now awaiting the actual file data fragments.
    add_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_WAITING_FRAGMENTS);

    // Register a general acknowledgment (ACK) to be sent back to the client,
    // confirming successful receipt and processing of the file metadata frame.
    // `queue_seq_num` is typically for data-related ACKs, but here used for a positive response.
    register_ack(&queue_seq_num, client, frame, STS_ACK);

    // Leave the critical section. This releases the lock on the `client` data structure,
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_SUCCESS` to indicate that the file metadata was handled successfully.
    return RET_VAL_SUCCESS;

exit_error:
    // This label serves as a unified exit point for all error conditions within the function.
    // It ensures that the critical section is always exited, preventing potential deadlocks
    // if an error occurs before the normal `LeaveCriticalSection` call.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_ERROR` to signal that an error occurred during the processing of file metadata.
    return RET_VAL_ERROR;
}
// Process received file fragment frame
int handle_file_fragment(ClientData *client, UdpFrame *frame){
    // Check if the client context is NULL. This is a fundamental check, as all
    // fragment handling logic depends on a valid client session.
    if(client == NULL){
        // Log an informational message indicating a frame was received for an unknown client.
        fprintf(stdout, "Received frame for non existing client context!\n");
        // Jump to the error handling block to release any potential resources and return.
        goto exit_error;
    }

    // Enter a critical section to protect the `client` data structure.
    EnterCriticalSection(&client->lock);

    // Update the client's `last_activity_time`. This timestamp is used for
    // session management and timeout detection, indicating recent communication.
    client->last_activity_time = time(NULL);

    // Extract necessary information from the UDP frame's header and payload.
    // `ntohl` and `ntohll` convert values from network byte order to host byte order
    // to ensure correct interpretation across different system architectures.
    uint32_t recv_session_id = ntohl(frame->header.session_id); // Session ID from frame header
    uint32_t recv_file_id = ntohl(frame->payload.file_fragment.file_id); // File ID from fragment payload
    uint64_t recv_fragment_offset = ntohll(frame->payload.file_fragment.offset); // Fragment offset
    uint32_t recv_fragment_size = ntohl(frame->payload.file_fragment.size); // Fragment size

    // Check if this file (identified by `recv_file_id` and `client->session_id`)
    // has already been marked as `UID_RECV_COMPLETE` in the unique ID hash table.
    // This prevents re-processing or re-acknowledging fragments for already completed transfers.
    if(search_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        // If the transfer is complete, register an ACK with `STS_TRANSFER_COMPLETE` status.
        // This signals to the sender that the file is fully received and no more fragments are needed.
        // `queue_seq_num_ctrl` is used for high-priority control acknowledgments.
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        // Jump to the error handling block to release the lock and exit.
        goto exit_error;
    }

    // Attempt to match the incoming file fragment to an existing file transfer session
    // within the client's context. This function typically searches through `client->file_stream` array.
    int slot = file_match_fragment(client, frame, recv_session_id, recv_file_id);
    // If `file_match_fragment` returns `RET_VAL_ERROR`, it means no active file transfer
    // corresponding to the received file ID and session ID was found. This usually implies
    // that the metadata for this file was not received or processed correctly.
    if(slot == RET_VAL_ERROR){
        // Log an error indicating an unknown file ID or missing metadata for the received fragment.
        fprintf(stderr, "Received frame with unknown file ID: %u Session ID %u - missing metadata\n", recv_file_id, client->session_id);
        // Register an ACK with `ERR_INVALID_FILE_ID` status, informing the sender of the issue.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_INVALID_FILE_ID);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Additional check to ensure the file ID in the selected slot matches the received file ID.
    // This acts as a double-check against `file_match_fragment` and potential race conditions.
    if (client->file_stream[slot].f_id != recv_file_id){
        // This scenario should ideally be caught by `file_match_fragment`, but serves as a safeguard.
        // It indicates an inconsistency or a frame for a file not properly set up in the slot.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_INVALID_FILE_ID);
        fprintf(stderr, "No file transfer in progress (no file buffer allocated) for file ID %u !!!\n", recv_file_id);
        goto exit_error;
    }

    // Check if this specific fragment (identified by its offset) has already been received.
    // This uses a bitmap or similar mechanism to track received fragments and detect duplicates.
    if(check_fragment_received(client->file_stream[slot].bitmap, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        // If it's a duplicate, register an ACK with `ERR_DUPLICATE_FRAME` status.
        // This informs the sender not to retransmit this specific fragment.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_DUPLICATE_FRAME);
        // Log a message indicating the receipt of a duplicate fragment.
        fprintf(stderr, "Received duplicate frame (file ID: %u, offset: %llu)!!!\n", recv_file_id, recv_fragment_offset);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Validate if the received fragment's offset is within the bounds of the expected file size.
    // A fragment starting beyond the file size indicates a malformed or erroneous frame.
    if(recv_fragment_offset >= client->file_stream[slot].f_size){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message providing details about the out-of-bounds offset.
        fprintf(stderr, "Received fragment with offset out of limits. File size: %llu, Received offset: %llu\n", client->file_stream[slot].f_size, recv_fragment_offset);
        // Jump to the error handling block.
        goto exit_error;
    }
    // Validate if the fragment, given its offset and size, extends beyond the file's total size.
    // This catches fragments that start within bounds but are too large for the remaining file.
    if (recv_fragment_offset + recv_fragment_size > client->file_stream[slot].f_size){
        // Register an ACK with `ERR_MALFORMED_FRAME` status.
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        // Log an error message indicating that the fragment extends past the file boundaries.
        fprintf(stderr, "Fragment extends past file bounds (file ID: %u, offset: %llu, size: %u). File size: %llu\n",
                recv_file_id, recv_fragment_offset, recv_fragment_size, client->file_stream[slot].f_size);
        // Jump to the error handling block.
        goto exit_error;
    }

    // Calculate the index of the memory chunk entry in the bitmap based on the fragment offset.
    // Each entry in the bitmap likely corresponds to a larger memory block (chunk) that holds multiple fragments.
    uint64_t entry_index = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL; // Assuming 64 fragments per bitmap entry

    // If the corresponding bitmap entry is 0, it means no memory chunk has been allocated for this section of the file yet.
    if(client->file_stream[slot].bitmap[entry_index] == 0ULL){
        // Allocate a new memory chunk from the `pool_file_chunk` memory pool.
        client->file_stream[slot].pool_block_file_chunk[entry_index] = pool_alloc(&pool_file_chunk);
        // Check if the memory chunk allocation was successful.
        if(client->file_stream[slot].pool_block_file_chunk[entry_index] == NULL){
            // If the memory pool is full and allocation fails, log a message (commented out in original).
            // This suggests a resource limitation that might need to be addressed.
            // fprintf(stdout, "Pool is full! Waiting for free block!\n");
            // Jump to the error handling block, as the fragment cannot be stored.
            goto exit_error;
        }
    }

    // Attach the received fragment's data to the appropriate location within the allocated memory chunk.
    // This function typically involves copying the fragment bytes and updating the fragment's status in the bitmap.
    file_attach_fragment_to_chunk(&client->file_stream[slot], frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size);

    // Register a general acknowledgment (ACK) to be sent back to the client,
    // confirming successful receipt and processing of this file fragment.
    // `queue_seq_num` is typically for data-related ACKs.
    register_ack(&queue_seq_num, client, frame, STS_ACK);

    // Leave the critical section, releasing the client's lock.
    // This must be done before returning from the function to prevent deadlocks.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_SUCCESS` to indicate that the file fragment was handled successfully.
    return RET_VAL_SUCCESS;

exit_error:
    // This label serves as a common exit point for all error conditions within the function.
    // It ensures that the critical section is always exited, regardless of where an error occurred,
    // thereby preventing deadlocks and resource leaks.
    LeaveCriticalSection(&client->lock);
    // Return `RET_VAL_ERROR` to signal that an error occurred during the processing of the file fragment.
    return RET_VAL_ERROR;
}
// HANDLE received message fragment frame
int handle_message_fragment(ClientData *client, UdpFrame *frame){

    int slot; // Declares an integer variable 'slot' to store the index of the message handling slot.
    if(client == NULL){ // Checks if the 'client' pointer is NULL, indicating an invalid or non-existent client context.
        fprintf(stdout, "Received frame for non existing client context!\n"); // Prints an informational message to standard output.
        goto exit_error; // Jumps to the 'exit_error' label for centralized error handling and cleanup.
    }
    update_statistics(client); // Calls an external function to update statistics related to the client's activity.

    EnterCriticalSection(&client->lock); // Acquires a critical section lock associated with the 'client' object. This ensures thread-safe access to 'client' data.

    client->last_activity_time = time(NULL); // Updates the 'last_activity_time' field of the client, typically used for session timeout management.

    // Extracts and converts message-specific fields from the network byte order to host byte order.
    // 'ntohl' converts a 32-bit unsigned integer from network byte order to host byte order.
    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    // Guard against fragments for already completed messages.
    // Checks if the message, identified by its ID and the client's session ID, has already been marked as fully received in the unique ID hash table.
    if(search_uid_hash_table(uid_hash_table, recv_message_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        // If the message is already complete, registers an acknowledgment with a 'STS_TRANSFER_COMPLETE' status.
        // This informs the sender that the message is fully received and no further retransmissions are needed.
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        goto exit_error; // Jumps to the 'exit_error' label.
    }

    // Handle either an existing or a new message stream.
    // Attempts to find an existing message handling slot that matches the incoming fragment.
    slot = msg_match_fragment(client, frame);
    if (slot != RET_VAL_ERROR) {
        // This block is executed if the 'slot' is found, meaning this is a fragment for an existing message.
        // Validates the incoming fragment against the state of the message stream in the identified slot.
        if (msg_validate_fragment(client, slot, frame) == RET_VAL_ERROR) {
            goto exit_error; // If fragment validation fails, jumps to the 'exit_error' label.
        }
        // Attaches the fragment's data to the appropriate position within the message buffer managed by the 'message_stream' structure.
        msg_attach_fragment(&client->msg_stream[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        // Acknowledges the successful receipt of the fragment.
        register_ack(&queue_seq_num, client, frame, STS_ACK);
        // Checks if all fragments for the message have been received and, if so, finalizes the message (e.g., writes to disk).
        if (msg_check_completion_and_record(&client->msg_stream[slot], client->session_id) == RET_VAL_ERROR)
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
    } else {
        // This block is executed if no matching slot is found, suggesting this is the first fragment of a new message.
        // Attempts to obtain an available slot for a new message stream.
        int slot = msg_get_available_stream_channel(client);
        if (slot == RET_VAL_ERROR){ // Checks if an available slot could not be obtained (e.g., maximum streams reached).
            fprintf(stderr, "Maximum message streams reached for client ID: %d\n", client->client_id); // Logs an error message.
            // Registers an ACK with 'ERR_RESOURCE_LIMIT' status, informing the sender of the resource constraint.
            register_ack(&queue_seq_num_ctrl, client, frame, ERR_RESOURCE_LIMIT);
            goto exit_error; // Jumps to the 'exit_error' label.
        }
        // Validates the fragment. This is particularly important for the first fragment, which often contains total message length.
        if (msg_validate_fragment(client, slot, frame) == RET_VAL_ERROR)
            goto exit_error; // If validation fails, jumps to the 'exit_error' label.
        // Initializes the message receiving slot with the message ID and its total expected length.
        if (msg_init_stream(&client->msg_stream[slot], client->session_id, recv_message_id, recv_message_len) == RET_VAL_ERROR)
            goto exit_error; // If slot initialization fails, jumps to the 'exit_error' label.
        // Attaches the first fragment's data to the newly initialized message buffer.
        
        msg_attach_fragment(&client->msg_stream[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        
        // Adds an entry to the unique ID hash table, marking the message as awaiting further fragments.
        add_uid_hash_table(uid_hash_table, recv_message_id, client->session_id, UID_WAITING_FRAGMENTS);
        // Acknowledges the successful receipt of this initial fragment.
        register_ack(&queue_seq_num, client, frame, STS_ACK);
        // Checks if the message is now complete (e.g., if it was a single-fragment message) and finalizes it.
        if (msg_check_completion_and_record(&client->msg_stream[slot], client->session_id) == RET_VAL_ERROR)
            goto exit_error; // If completion check or recording fails, jumps to the 'exit_error' label.
        LeaveCriticalSection(&client->lock); // Releases the critical section lock.
        return RET_VAL_SUCCESS; // Returns success, indicating the message fragment was handled.
    }

exit_error: // Label for centralized error handling.
    LeaveCriticalSection(&client->lock); // Ensures the critical section lock is released before exiting, preventing deadlocks.
    return RET_VAL_ERROR; // Returns an error status.
}
// update file transfer progress and speed in MBs
void update_statistics(ClientData * client){

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
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {
    // Declare a UdpFrame structure to hold the raw incoming UDP datagram.
    UdpFrame received_frame;
    // Declare a QueueFrameEntry structure to prepare the received data for queuing.
    // This structure typically bundles the frame data, its source address, and its size.
    QueueFrameEntry frame_entry;

    // Declare a sockaddr_in structure to store the source (sender's) address and port.
    struct sockaddr_in src_addr;
    // Initialize src_addr_len with the size of the sockaddr_in structure.
    // This is passed to recvfrom to specify the buffer size for the source address
    // and is updated by recvfrom with the actual size of the address returned.
    int src_addr_len = sizeof(src_addr);
    // Variable to store the error code returned by WSAGetLastError() in case of a socket error.
    int recv_error_code;

    // Variable to store the number of bytes successfully received by recvfrom.
    int bytes_received;

    // Set a receive timeout for the thread's socket.
    // This timeout ensures that the `recvfrom` call does not block indefinitely,
    // allowing the thread to periodically check the `server.status` and gracefully
    // terminate if the server is shutting down. `RECVFROM_TIMEOUT_MS` is an application-defined constant.
    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        // If setting the socket option fails, log an error message to standard error.
        // This is a non-critical error for the server's basic operation but might
        // impact the responsiveness of the thread to shutdown signals.
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // The thread will continue running despite this error.
    }

    // Main loop for the receive thread.
    // The thread continues to execute and receive frames as long as the global 'server.status'
    // indicates that the server is in a 'SERVER_READY' state.
    while (server.status == SERVER_READY) {

        // Clear the `received_frame` buffer. This is good practice to ensure that
        // any remnants of previous, smaller packets do not corrupt the data of new incoming packets.
        memset(&received_frame, 0, sizeof(UdpFrame));
        // Clear the `src_addr` structure to reset any previous sender address information.
        memset(&src_addr, 0, sizeof(src_addr));

        // Attempt to receive a UDP datagram from the server's socket.
        // - `server.socket`: The socket file descriptor to receive data from.
        // - `(char*)&received_frame`: A pointer to the buffer where the incoming data will be stored.
        // - `sizeof(UdpFrame)`: The maximum number of bytes to receive into the buffer.
        // - `0`: Flags, typically set to 0 for standard receiving.
        // - `(struct sockaddr*)&src_addr`: A pointer to a generic socket address structure
        //   where the sender's address information will be stored. This is cast from `struct sockaddr_in*`.
        // - `&src_addr_len`: A pointer to an integer holding the size of the `src_addr` buffer.
        //   It's updated by `recvfrom` to indicate the actual size of the sender's address.
        bytes_received = recvfrom(server.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (struct sockaddr*)&src_addr, &src_addr_len);

        // Check if `recvfrom` encountered an error.
        if (bytes_received == SOCKET_ERROR) {
            // Retrieve the specific error code to differentiate between various issues.
            recv_error_code = WSAGetLastError();
            // `WSAETIMEDOUT` is a common and expected error when the `SO_RCVTIMEO` option is set
            // and no data arrives within the specified timeout. It's not a critical error.
            if (recv_error_code != WSAETIMEDOUT) {
                // For any other socket error, print an error message to standard error,
                // as it indicates a more serious problem with the socket or network.
                fprintf(stderr, "recvfrom failed with error: %d\n", recv_error_code);
            }
            // Regardless of the specific error (timeout or other), the loop continues
            // to the next iteration to attempt receiving another frame.
        } else if (bytes_received > 0) {
            // If `bytes_received` is greater than 0, a frame was successfully received.

            // Check if the actual number of bytes received exceeds the maximum expected `UdpFrame` size.
            // This is a crucial validation step to detect potentially malformed or oversized packets
            // before attempting to process them.
            if(bytes_received > sizeof(UdpFrame)){
                // If an oversized frame is detected, print a warning to standard output.
                // It's generally better practice to log such warnings/errors to `stderr` for better separation
                // of informational output from error output, and include source IP/port for debugging.
                fprintf(stdout, "Frame received with bytes > max frame size!\n");
                // Discard this frame immediately by skipping the rest of the current loop iteration.
                // This prevents processing of potentially malicious or erroneous data.
                continue;
            }

            // Clear the `frame_entry` structure to prepare it for new data.
            // This ensures no stale data from previous operations remains in the structure.
            memset(&frame_entry, 0, sizeof(QueueFrameEntry));
            // Copy the actual received frame data from `received_frame` into the `frame` member of `frame_entry`.
            // Since the check `bytes_received > sizeof(UdpFrame)` has already passed, we are
            // guaranteed that `bytes_received` is less than or equal to `sizeof(UdpFrame)`.
            // Copying exactly `bytes_received` ensures that we only copy the valid portion of the packet,
            // preventing out-of-bounds reads if the received packet was smaller than `UdpFrame`.
            memcpy(&frame_entry.frame, &received_frame, bytes_received);

            // Copy the sender's address information into the `src_addr` member of `frame_entry`.
            // This allows the processing thread to know who sent the frame for responding or logging.
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));

            // Store the exact number of bytes that were received for this frame.
            // This information is important for downstream processing (e.g., checksum validation)
            // as it represents the true size of the received payload that was copied.
            frame_entry.frame_size = bytes_received;

            // Extract the frame type from the header of the received frame.
            // This is done after `frame_entry.frame` has been populated with the received data.
            uint8_t frame_type = frame_entry.frame.header.frame_type;

            // Determine if the current frame is considered a high-priority control frame.
            // These frames are often crucial for maintaining connection state or initiating transfers,
            // and are routed to a dedicated queue for more immediate processing by `process_frame_thread_func`.
            BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_KEEP_ALIVE ||
                                           frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                           frame_type == FRAME_TYPE_FILE_METADATA ||
                                           frame_type == FRAME_TYPE_DISCONNECT);

            // Declare a pointer to the target queue. This will point to either the control queue
            // or the general data queue based on the frame's priority.
            QueueFrame *target_queue = NULL;

            // Assign the appropriate queue based on the frame's priority.
            if (is_high_priority_frame == TRUE) {
                target_queue = &queue_frame_ctrl; // High-priority frames go to `queue_frame_ctrl`.
            } else {
                target_queue = &queue_frame;      // Other frames go to the general `queue_frame`.
            }

            // Attempt to push the prepared `frame_entry` into the determined target queue.
            // The `push_frame` function is assumed to handle its own internal thread safety (e.g., using a mutex for the queue).
            if (push_frame(target_queue, &frame_entry) != RET_VAL_SUCCESS) {
                // If pushing to the queue fails (e.g., the queue is full), log an error.
                // This indicates a potential bottleneck or capacity issue in the queuing system.
                fprintf(stderr, "Failed to push frame to queue. Queue full?\n");
                continue; // Discard this frame (as it couldn't be queued) and proceed to the next receive attempt.
            }
        }
    }
    // The `while` loop terminates when `server.status` is no longer `SERVER_READY`,
    // which signifies that the server is shutting down.
    // Properly terminate the thread created by `_beginthreadex`. This ensures all
    // thread-specific resources are cleaned up correctly by the operating system.
    _endthreadex(0);
    return 0; // Return 0 to indicate successful thread termination.
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam) {

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

    ClientData *client;             // A pointer to the ClientData structure associated with the current frame's session.

    // Main thread loop: This thread continuously runs as long as the server's global status
    // is set to SERVER_READY. Its primary responsibility is to dequeue incoming UDP frames
    // and dispatch them for processing based on their type.
    while(server.status == SERVER_READY) {
        // Clear the frame_entry structure at the beginning of each iteration.
        // This ensures that any data from a previous frame is not accidentally processed again.
        memset(&frame_entry, 0, sizeof(QueueFrameEntry));

        // Attempt to pop a frame from the control frame queue first.
        // The control queue typically handles high-priority messages like connection requests or disconnects.
        if (pop_frame(&queue_frame_ctrl, &frame_entry) == RET_VAL_SUCCESS) {
            // Frame successfully retrieved from the control queue.
        }
        // If the control queue is empty, attempt to pop a frame from the general data frame queue.
        else if (pop_frame(&queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Frame successfully retrieved from the general data queue.
        }
        // If both queues are empty, there are currently no frames awaiting processing.
        else {
            Sleep(100); // Pause the thread for 100 milliseconds. This prevents busy-waiting,
                        // reducing CPU utilization, and allows other threads to run.
            continue;   // Skip the rest of the current loop iteration and start a new one.
        }

        // Assign local pointers to the frame data and source address within the popped entry.
        // This makes subsequent access to these details more convenient.
        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        // Extract and convert header fields from network byte order to host byte order.
        // This ensures the values are correctly interpreted on the local machine.
        header_delimiter = ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = ntohll(frame->header.seq_num);
        header_session_id = ntohl(frame->header.session_id);

        // Convert the binary IP address from the source sockaddr_in structure to a human-readable string.
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        // Convert the source port number from network byte order to host byte order.
        src_port = ntohs(src_addr->sin_port);

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
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr);
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
                fprintf(stdout, "Received frame (type: 0x%X, seq: %llu) from unknown/disconnected client %s:%d. Ignoring...\n",
                        header_frame_type, header_seq_num, src_ip, src_port);
                continue; // Discard this frame and move to the next.
            }
        }
        // At this point, the 'client' pointer is guaranteed to be non-NULL and refers to an active client.

        // --- Process Payload based on Frame Type ---
        // Dispatch control to specific handlers based on the frame type.
        switch (header_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST:
                // This block is primarily reached if a *new client* was just added by `add_client` above.
                // (The case of an existing client re-sending CONNECT_REQUEST is handled before the switch).
                // Acquire the client's lock before accessing or modifying its data.
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL); // Update client activity time.
                // Send the connection response. The client's address (client->addr) is accessed.
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr);
                // Release the client's lock.
                LeaveCriticalSection(&client->lock);
                break; // End of FRAME_TYPE_CONNECT_REQUEST case.

            case FRAME_TYPE_ACK:
                // When an ACK (Acknowledgment) frame is received from a client.
                // Acquire the client's lock to safely update its activity time.
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL); // Update last activity time to keep the session alive.
                LeaveCriticalSection(&client->lock);
                // TODO: Implement the full ACK processing logic here. This typically involves:
                //   - Removing acknowledged packets from the sender's retransmission queue.
                //   - Updating window sizes for flow and congestion control.
                //   - Advancing sequence numbers to indicate successfully received data.
                break; // End of FRAME_TYPE_ACK case.

            case FRAME_TYPE_KEEP_ALIVE:
                // When a Keep-Alive frame is received from a client.
                // Acquire the client's lock to safely update its activity time.
                EnterCriticalSection(&client->lock);
                client->last_activity_time = time(NULL); // Update last activity time to confirm client's liveness.
                // Register an ACK to be sent back for this Keep-Alive frame.
                // The `register_ack` function is responsible for its own internal locking if it modifies the client object beyond reading `client->addr`.
                register_ack(&queue_seq_num_ctrl, client, frame, STS_KEEP_ALIVE);
                // Release the client's lock.
                LeaveCriticalSection(&client->lock);
                // TODO: Further processing if Keep-Alive frames carry additional state information.
                break; // End of FRAME_TYPE_KEEP_ALIVE case.

            case FRAME_TYPE_FILE_METADATA:
                // This frame type indicates that the client is sending file transfer metadata.
                // The `handle_file_metadata` function is called to process this.
                // It is assumed that `handle_file_metadata` will acquire necessary locks (e.g., client's lock
                // if it updates `last_activity_time`, or specific file stream locks) internally.
                handle_file_metadata(client, frame);
                break; // End of FRAME_TYPE_FILE_METADATA case.

            case FRAME_TYPE_FILE_FRAGMENT:
                // This frame type indicates a data fragment of a file being transferred.
                // The `handle_file_fragment` function is called to process this.
                // It is assumed that `handle_file_fragment` will acquire necessary locks (e.g., client's lock
                // for `last_activity_time` or specific file stream locks) internally.
                handle_file_fragment(client, frame);
                break; // End of FRAME_TYPE_FILE_FRAGMENT case.

            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                // This frame type indicates a fragment of a long text message.
                // The `handle_message_fragment` function is called to process this.
                // It is assumed that `handle_message_fragment` will acquire necessary locks internally.
                handle_message_fragment(client, frame);
                break; // End of FRAME_TYPE_LONG_TEXT_MESSAGE case.

            case FRAME_TYPE_DISCONNECT:
                // This frame type indicates that the client is requesting to disconnect gracefully.
                // Log the client's disconnection request.
                fprintf(stdout, "Client %s:%d with session ID %d requested disconnect...\n", client->ip, src_port, client->session_id);
                // Call `remove_client` to clean up all resources associated with this client.
                // `remove_client` is expected to handle its own internal locking for the client slot being modified.
                remove_client(&client_list, client->slot_num);
                break; // End of FRAME_TYPE_DISCONNECT case.

            default:
                // For any other unexpected or unhandled frame types, log an error message.
                fprintf(stderr, "Received unknown frame type 0x%X from %s:%d (Session ID: %u). Discarding.\n",
                        header_frame_type, src_ip, src_port, header_session_id);
                break; // End of default case.
        }
    }
    // The thread loop terminates when `server.status` is no longer `SERVER_READY`.
    // The `_endthreadex(0)` function is the proper way to exit a thread created with `_beginthreadex`.
    return 0;
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(LPVOID lpParam){

    time_t time_now; // Declares a variable to store the current timestamp.

    // Main loop of the thread. This loop continuously runs as long as the server's status
    // is set to SERVER_READY. It is responsible for periodically checking client activity
    // and disconnecting inactive clients.
    while(server.status == SERVER_READY) {

        // Iterate through each possible client slot, from 0 up to MAX_CLIENTS - 1.
        // Each 'slot' represents a potential connection slot for a client.
        for(int slot = 0; slot < MAX_CLIENTS; slot++){
            time_now = time(NULL); // Get the current time. This is done for each client check
                                   // to ensure the most up-to-date time is used for timeout calculation.

            EnterCriticalSection(&client_list.client[slot].lock);

            // Check if the current client slot is marked as SLOT_FREE.
            // If a slot is free, it means there's no active client in it, so no timeout check is needed.
            if(client_list.client[slot].slot_status == SLOT_FREE){
                LeaveCriticalSection(&client_list.client[slot].lock);
                continue; // Skip to the next client slot.
            }

            // Calculate the duration of inactivity for the client in this slot.
            // Compare this inactivity duration with the configured server.session_timeout.
            // If the client's last activity was more recent than the timeout period, it's still considered active.
            if(time_now - (time_t)client_list.client[slot].last_activity_time < (time_t)server.session_timeout){
                LeaveCriticalSection(&client_list.client[slot].lock);
                continue; // Skip to the next client slot.
            }

            // If the code reaches this point, it means the client in the current slot has timed out.
            LeaveCriticalSection(&client_list.client[slot].lock);

            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", client_list.client[slot].session_id);

            // Send a disconnect control message to the timed-out client's address.
            // This is a best-effort attempt to inform the client that it has been disconnected by the server.
            send_disconnect(client_list.client[slot].session_id, server.socket, &client_list.client[slot].addr);

            // Call the `remove_client` function to clean up all resources associated with this client slot
            // and mark the slot as free. This function is expected to be thread-safe in its own implementation.
            remove_client(&client_list, slot);
        }
        // Pause the thread's execution for 1000 milliseconds (1 second).
        // This prevents the thread from consuming 100% CPU by continuously looping and polling.
        // It sets the frequency at which client timeouts are checked.
        Sleep(1000);
    }

    // After the `while` loop condition (`server.status == SERVER_READY`) becomes false,
    _endthreadex(0);
    return 0; // Return 0 to indicate that the thread terminated successfully.
}
// --- SendAck Thread Function ---
unsigned int WINAPI ack_thread_func(LPVOID lpParam){

    QueueSeqNumEntry entry;

    while (server.status == SERVER_READY) {
        memset(&entry, 0, sizeof(QueueSeqNumEntry));

        if(pop_seq_num(&queue_seq_num_ctrl, &entry) == RET_VAL_SUCCESS){
            // Attempt to pop a sequence number entry from the control queue first.
        } else if(pop_seq_num(&queue_seq_num, &entry) == RET_VAL_SUCCESS){
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
        send_ack_nak(entry.seq_num, entry.session_id, entry.op_code, server.socket, &entry.addr);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Process server command ---
unsigned int WINAPI server_command_thread_func(LPVOID lpParam){

    _endthreadex(0);
    return 0;
}

unsigned int WINAPI check_complete_blocks_in_bitmap_func(LPVOID lpParam){

    // Main loop: Continue as long as the server is in a READY state.
    // This thread continuously polls for completed file blocks to write to disk.
    while (server.status == SERVER_READY) {
        // Iterate through all possible client slots.
        for(int i = 0; i < MAX_CLIENTS; i++){

            ClientData *client = &client_list.client[i];
            // Acquire the critical section lock for the current client.
            // This prevents other threads (e.g., connection handler) from modifying
            // client data while this thread is inspecting it.
            EnterCriticalSection(&client->lock);

            // Check if the client slot is currently connected.
            if(client->connection_status == CLIENT_CONNECTED){
                // Iterate through all possible file stream slots for the current client.
                for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){

                    FileStream *fstream = &client_list.client[i].file_stream[j];
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
                            uint8_t new_flag_value; // Variable to store the new flag status after writing.

                            // Case 1: This is the trailing (last, potentially partial) chunk.
                            // Check if:
                            //   a) The fstream is marked as having a trailing chunk.
                            //   b) The trailing chunk's expected bytes have all been received.
                            //   c) The current chunk's flag indicates it's the trailing chunk AND it hasn't been written yet.
                            if (fstream->trailing_chunk && fstream->trailing_chunk_complete && (fstream->flag[k] == CHUNK_TRAILING)) {
                                buffer_size = fstream->trailing_chunk_size; // Use the specific calculated size for the trailing chunk.
                                new_flag_value = CHUNK_TRAILING | CHUNK_WRITTEN; // Mark it as trailing and now written.
                                fprintf(stdout, "Writing trailing chunk bytes: %llu, chunk index: %llu\n", buffer_size, k);

                            // Case 2: This is a full-sized chunk (not trailing).
                            // Check if:
                            //   a) All fragments within this 64-bit bitmap entry have been received (~0ULL means all bits set).
                            //   b) The current chunk's flag indicates it's a body chunk AND it hasn't been written yet.
                            } else if(fstream->bitmap[k] == ~0ULL && (fstream->flag[k] == CHUNK_BODY)) {
                                buffer_size = FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK; // Full chunk size.
                                new_flag_value = CHUNK_BODY | CHUNK_WRITTEN; // Mark it as a body chunk and now written.
                                // fprintf(stdout, "Writing complete chunk bytes: %llu, chunk index: %llu\n", buffer_size, k); // Optional for full chunks
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
                                fprintf(stderr, "Error: FILE pointer is null for chunk %llu. Session ID: %u, File ID: %u\n", k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FP; // Set a specific error code.
                                file_cleanup_stream(fstream); // Clean up the entire file stream due to the error.
                                break; // Exit the 'k' loop (current file's chunks) and move to the next file stream.
                            }

                            // Attempt to seek to the correct offset in the file.
                            if (_fseeki64(fstream->fp, file_offset, SEEK_SET) != 0) {
                                fprintf(stderr, "Error: Failed to seek to offset %llu for chunk %llu. Session ID: %u, File ID: %u\n", file_offset, k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FSEEK; // Set a specific error code.
                                file_cleanup_stream(fstream); // Clean up the entire file stream.
                                break; // Exit the 'k' loop.
                            }

                            // Write the chunk data from the buffer to the file.
                            size_t written = fwrite(buffer, 1, buffer_size, fstream->fp);
                            // Check if the number of bytes written matches the expected buffer size.
                            if (written != buffer_size) {
                                fprintf(stderr, "Error: Failed to write data (expected %llu, wrote %llu) for chunk %llu. Session ID: %u, File ID: %u\n", buffer_size, written, k, fstream->s_id, fstream->f_id);
                                fstream->stream_err = STREAM_ERR_FWRITE; // Set a specific error code.
                                file_cleanup_stream(fstream); // Clean up the entire file stream.
                                break; // Exit the 'k' loop.
                            }
                            fstream->bytes_written += written; // Accumulate the total bytes written to disk.
                            fstream->flag[k] = new_flag_value; // Update the chunk's flag to reflect it has been written.

                            // Return the memory buffer for this chunk back to the pre-allocated pool.
                            pool_free(&pool_file_chunk, fstream->pool_block_file_chunk[k]);
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
                                update_uid_status_hash_table(uid_hash_table, fstream->s_id, fstream->f_id, UID_RECV_COMPLETE);
                                fprintf(stdout, "[INFO] Transfer finished, created file: %s, bytes: %llu\n", fstream->fn, fstream->bytes_written);
                                file_cleanup_stream(fstream); // Perform final cleanup for the completed file stream.
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
        Sleep(100); // Pause execution for 100 milliseconds to avoid busy-waiting and reduce CPU usage.
                    // Consider event-based signaling (e.g., condition variables) for more efficient
                    // and responsive operation in high-performance scenarios.
    }
    _endthreadex(0); // Properly exit the thread, returning its control to the system.
    return 0; // Return 0 indicating successful thread termination.
}







static void register_ack(QueueSeqNum *queue, ClientData *client, UdpFrame *frame, uint8_t op_code) {
    // Create a new QueueSeqNumEntry structure to store the ACK/NAK details.
    QueueSeqNumEntry entry = {
        // Extract the sequence number from the frame header, converting from network to host byte order.
        .seq_num = ntohll(frame->header.seq_num),
        // Set the operation code (e.g., ACK or NAK) as provided by the caller.
        .op_code = op_code,
        // Extract the session ID from the frame header, converting from network to host byte order.
        .session_id = ntohl(frame->header.session_id)
    };
    // Copy the client's address (destination for the ACK/NAK) into the entry.
    // This is where the client's `addr` member is accessed.
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
    // Push the constructed entry into the specified sequence number queue.
    // This function (push_seq_num) is expected to handle its own locking for the queue.
    push_seq_num(queue, &entry);
}
// Handle message fragment helper functions
static int msg_match_fragment(ClientData *client, UdpFrame *frame){
    
    uint32_t message_id = ntohl(frame->payload.long_text_msg.message_id);

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].m_id == message_id && client->msg_stream[i].buffer != NULL && client->msg_stream[i].bitmap != NULL){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);        
    }
    return RET_VAL_ERROR;
}
static int msg_validate_fragment(ClientData *client, const int index, UdpFrame *frame){

    EnterCriticalSection(&client->msg_stream[index].lock);

    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    BOOL is_duplicate_fragment = client->msg_stream[index].bitmap && client->msg_stream[index].buffer &&
                                    check_fragment_received(client->msg_stream[index].bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);
    if (is_duplicate_fragment == TRUE) {
        //Client already has bitmap and message buffer allocated so fragment can be processed
        //if the message was already received send duplicate frame ack op_code
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate text message fragment! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    if(recv_fragment_offset >= recv_message_len){
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    if ((recv_fragment_offset + recv_fragment_len) > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code 
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment len past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        goto exit_error;
    }
    //Success path
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&client->msg_stream[index].lock);
    return RET_VAL_ERROR;

}
static int msg_get_available_stream_channel(ClientData *client){
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        EnterCriticalSection(&client->msg_stream[i].lock);
        if(client->msg_stream[i].m_id == 0 && client->msg_stream[i].buffer == NULL && client->msg_stream[i].bitmap == NULL){
            LeaveCriticalSection(&client->msg_stream[i].lock);
            return i;
        }
        LeaveCriticalSection(&client->msg_stream[i].lock);
    }
    return RET_VAL_ERROR;
}
static int msg_init_stream(MsgStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len){

    EnterCriticalSection(&mstream->lock);

    mstream->s_id = session_id;
    mstream->m_id = message_id;
    mstream->m_len = message_len;

    // Calculate total fragments
    mstream->fragment_count = (mstream->m_len + (uint64_t)TEXT_FRAGMENT_SIZE - 1ULL) / (uint64_t)TEXT_FRAGMENT_SIZE;
    fprintf(stdout, "Fragments count for message: %llu\n", mstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    mstream->bitmap_entries_count = (mstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;  
    fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", mstream->bitmap_entries_count);

    mstream->bitmap = malloc(mstream->bitmap_entries_count * sizeof(uint64_t));
    if(mstream->bitmap == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        goto exit_error;
    }
    memset(mstream->bitmap, 0, mstream->bitmap_entries_count * sizeof(uint64_t));
    
    //copy the received fragment text to the buffer            
    mstream->m_id = message_id;
    mstream->buffer = malloc(message_len);
    if(mstream->buffer == NULL){
        fprintf(stdout, "Error allocating memory!!!\n");
        goto exit_error;
    }
    memset(mstream->buffer, 0, message_len);

    // Constructs a filename for storing the received message, incorporating session and message IDs for uniqueness.
    snprintf(mstream->file_name, PATH_SIZE, "E:\\msg_SID_%d_UID%d.txt", session_id, message_id);

    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_ERROR;

}
static void msg_attach_fragment(MsgStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len){
    EnterCriticalSection(&mstream->lock);
    char *dest = mstream->buffer + fragment_offset;
    char *src = fragment_buffer;                                              
    memcpy(dest, src, fragment_len);
    mstream->chars_received += fragment_len;       
    mark_fragment_received(mstream->bitmap, fragment_offset, TEXT_FRAGMENT_SIZE);
    LeaveCriticalSection(&mstream->lock);
}
static int msg_check_completion_and_record(MsgStream *mstream, const uint32_t session_id){
    // Check if the message is fully received by verifying total bytes and the fragment bitmap.
    EnterCriticalSection(&mstream->lock);

    BOOL message_is_complete = (mstream->chars_received == mstream->m_len) && check_bitmap(mstream->bitmap, mstream->fragment_count);

    if (message_is_complete == FALSE) {
        // The message is not yet complete. No action needed for now.
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_SUCCESS;
    }

    // --- Null terminate the message ---
    mstream->buffer[mstream->m_len] = '\0';
    // Attempt to write the in-memory buffer to a file on disk.
    int msg_creation_status = create_output_file(mstream->buffer, mstream->chars_received, mstream->file_name);
    
    // Update the file status in the hash table to mark it as complete.
    // This is done regardless of the file save success, as we won't be receiving more fragments.
    update_uid_status_hash_table(uid_hash_table, session_id, mstream->m_id, UID_RECV_COMPLETE);

    // Clean up all dynamically allocated resources for the transfer entry.
    // This block is executed in both success and failure cases of file creation.
    mstream->s_id = 0;
    mstream->m_id = 0;
    mstream->chars_received = 0;
    mstream->fragment_count = 0;
    mstream->bitmap_entries_count = 0;
    
    if(mstream->buffer != NULL){
        free(mstream->buffer);
        mstream->buffer = NULL;
    }
    if(mstream->bitmap != NULL){
        free(mstream->bitmap);
        mstream->bitmap = NULL;
    }
    if (msg_creation_status != RET_VAL_SUCCESS) {
        // If file creation failed, return an error.
        fprintf(stderr, "Error: Failed to create output message for file_id %d\n", mstream->m_id);
        remove(mstream->file_name);
        LeaveCriticalSection(&mstream->lock);
        return RET_VAL_ERROR;
    }
    // File was successfully created and saved.
    LeaveCriticalSection(&mstream->lock);
    return RET_VAL_SUCCESS;
}

// Handle file fragment helper functions
static int file_match_fragment(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){
    // Iterate through all possible file stream slots associated with this client.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        // This prevents other threads from modifying or accessing this specific file stream's data
        // (like its busy status, session ID, or file ID) while this function is checking it.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Check if the current file stream slot matches the incoming fragment:
        // 1. client->file_stream[i].busy == TRUE: Ensure the slot is currently active and in use for a transfer.
        // 2. client->file_stream[i].s_id == session_id: Match the session ID from the incoming fragment.
        // 3. client->file_stream[i].fid == file_id: Match the file ID from the incoming fragment.
        if(client->file_stream[i].busy == TRUE && client->file_stream[i].s_id == session_id && client->file_stream[i].f_id == file_id){
            // If a matching file stream slot is found, release its lock.
            LeaveCriticalSection(&client->file_stream[i].lock);
            // Return the index of the matching slot.
            return i;
        }
        // If the current file stream slot does not match the fragment, release its lock
        // before proceeding to check the next slot. This is crucial to avoid deadlocks.
        LeaveCriticalSection(&client->file_stream[i].lock);
    }
    // If the loop completes without finding any file stream slot that matches the session_id and file_id,
    // it means no active transfer for this client corresponds to the incoming fragment.
    return RET_VAL_ERROR; // Return an error value to indicate no match was found.
}
static int file_check_metadata(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){
    // This function checks if a file stream with the given session_id and file_id already exists for the client. 
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){      
        EnterCriticalSection(&client->file_stream[i].lock);       
        if(client->file_stream[i].s_id == session_id && client->file_stream[i].f_id == file_id){          
            LeaveCriticalSection(&client->file_stream[i].lock);          
            return RET_VAL_ERROR;
        }      
        LeaveCriticalSection(&client->file_stream[i].lock);
    }  
    return RET_VAL_SUCCESS; 
}
static int file_get_available_stream_channel(ClientData *client){
    // Iterate through all possible file stream slots allocated for this client.
    // MAX_CLIENT_FILE_STREAMS defines the maximum number of concurrent file transfers a single client can handle.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream slot.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Check if the current file stream slot is not busy.
        // A 'busy' status of FALSE indicates that this slot is currently free and available.
        if(client->file_stream[i].busy == FALSE){
            // If an available slot is found, immediately mark it as busy.
            // This claims the slot for the current operation and prevents other threads
            // from simultaneously claiming the same slot.
            client->file_stream[i].busy = TRUE;
            // Release the critical section lock for this file stream, as its state has been updated.
            LeaveCriticalSection(&client->file_stream[i].lock);
            // Return the index of the newly claimed (and now busy) slot.
            return i;
        }
        // If the current file stream slot is busy, release its lock before checking the next slot.
        // It's important to release the lock for a busy slot so other threads can still access it.
        LeaveCriticalSection(&client->file_stream[i].lock);
    }
    // If the loop completes without finding any available (non-busy) file stream slots,
    // it means all slots are currently in use.
    return RET_VAL_ERROR; // Return an error value to indicate no slot was found.
}
static void file_attach_fragment_to_chunk(FileStream *fstream, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size){
    // Calculate the index of the 64-bit bitmap entry (which corresponds to a "chunk" of 64 fragments)
    // to which this incoming fragment belongs.
    // fragment_offset / FILE_FRAGMENT_SIZE gives the fragment number.
    // Dividing by 64ULL (FRAGMENTS_PER_CHUNK) then gives the chunk index.
    uint64_t fstream_index = (fragment_offset / FILE_FRAGMENT_SIZE) / FRAGMENTS_PER_CHUNK;

    // Acquire the critical section lock for the specific FileStream.
    // This ensures thread-safe access to the fstream's data members (bitmap, flags, counters, pool_block_file_chunk)
    // while this fragment is being processed and attached.
    EnterCriticalSection(&fstream->lock);

    // Calculate the destination address within the target memory chunk where the fragment data will be copied.
    // fstream->pool_block_file_chunk[fstream_index] gives the base address of the chunk.
    // (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)) calculates the offset *within that chunk*.
    // This accounts for multiple fragments being part of one chunk.
    char *dest = fstream->pool_block_file_chunk[fstream_index] + (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK));
    char *src = fragment_buffer; // The source buffer containing the incoming fragment data.

    // Copy the fragment data from the source buffer to its calculated destination within the chunk's memory block.
    memcpy(dest, src, fragment_size);
    // Mark the flag for this chunk as CHUNK_BODY.
    // This assumes that by default, chunks are considered 'body' chunks unless specifically identified as 'trailing'.
    fstream->flag[fstream_index] = CHUNK_BODY;
    // fprintf(stdout, "Attaching fragment with offset: %llu to chunk: %llu at offset: %llu\n", fragment_offset, fstream_index, (fragment_offset % (FILE_FRAGMENT_SIZE * FRAGMENTS_PER_CHUNK)));
    // Increment the total number of bytes received for this file stream.
    fstream->bytes_received += fragment_size;

    // Check if this file stream is expected to have a trailing (potentially partial) chunk.
    if(fstream->trailing_chunk == TRUE){
        // Special handling for the last chunk, which might be the trailing chunk.
        // Check if the current fragment belongs to the last bitmap entry (chunk).
        if(fstream_index == fstream->bitmap_entries_count - 1ULL){
            // If it's the last chunk, accumulate the size of the received fragments for this chunk.
            fstream->trailing_chunk_size += fragment_size;
            // Mark the flag for this last chunk specifically as CHUNK_TRAILING.
            // This flag differentiates it from regular CHUNK_BODY entries for writing logic.
            fstream->flag[fstream_index] = CHUNK_TRAILING;
            // Note: The `CHUNK_BODY` set above will be immediately overwritten if this condition is true.
            //fprintf(stdout, "Receiving last chunk bytes: %llu\n", entry->last_chunk_size); // Original comment used 'entry', should be 'fstream'
        }
        // Check if all expected bytes for the entire file have been received.
        if(fstream->bytes_received == fstream->f_size){
            // If all bytes are received, mark the trailing chunk as complete.
            // This signals that the last chunk is fully assembled (even if partial in size).
            fstream->trailing_chunk_complete = TRUE;
            fprintf(stdout, "\nReceived all bytes. Last chunk size is: %llu\n", fstream->trailing_chunk_size);
        }
    }

    // Update the bitmap to mark this specific fragment as received.
    // This function manipulates the bits within the uint64_t bitmap entry.
    mark_fragment_received(fstream->bitmap, fragment_offset, FILE_FRAGMENT_SIZE);

    // Release the critical section lock for the file stream.
    LeaveCriticalSection(&fstream->lock);

}
static int file_stream_init(FileStream *fstream, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size){

    // Acquire stream channel lock
    EnterCriticalSection(&fstream->lock);

    // Initialize stream channel context data
    fstream->busy = TRUE;
    fstream->file_complete = FALSE;
    fstream->stream_err = 0;

    fstream->s_id = session_id;
    fstream->f_id = file_id;
    fstream->f_size = file_size;
    fstream->bytes_received = 0;
    fstream->bytes_written = 0;

    // Calculate total fragments
    fstream->fragment_count = (fstream->f_size + (uint64_t)FILE_FRAGMENT_SIZE - 1ULL) / (uint64_t)FILE_FRAGMENT_SIZE;
    fprintf(stdout, "Fragments count: %llu\n", fstream->fragment_count);

    // Calculate number of 64-bit bitmap entries (chunks)
    fstream->bitmap_entries_count = (fstream->fragment_count + FRAGMENTS_PER_CHUNK - 1ULL) / FRAGMENTS_PER_CHUNK;
    fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", fstream->bitmap_entries_count);

 
    //the file size needs to be a multiple of FILE_FRAGMENT_SIZE and nr of fragments needs to be a multiple of 64 due to bitmap entries being 64 bits
    //otherwise the trailing bitmap entry will not be full of fragments (the mask will not be ~0ULL) and this needs to be treated separately 

    // Determine if there's a trailing chunk
    // A trailing chunk exists if:
    // 1. The total file size is not a perfect multiple of FILE_FRAGMENT_SIZE
    // OR
    // 2. The total number of fragments is not a perfect multiple of FRAGMENTS_PER_CHUNK (64)
    fstream->trailing_chunk = ((fstream->f_size % FILE_FRAGMENT_SIZE) != 0) || (((fstream->f_size / FILE_FRAGMENT_SIZE) % 64ULL) != 0);

    // Initialize trailing chunk status
    fstream->trailing_chunk_complete = FALSE;
    fstream->trailing_chunk_size = 0;

    // Allocate memory for bitmap
    fstream->bitmap = malloc(fstream->bitmap_entries_count * sizeof(uint64_t));
    if(fstream->bitmap == NULL){
        fstream->stream_err = STREAM_ERR_BITMAP_MALLOC;
        fprintf(stderr, "Memory allocation fail for file bitmap mem!!!\n");
        goto exit_error;
    }
    memset(fstream->bitmap, 0, fstream->bitmap_entries_count * sizeof(uint64_t));

    // Allocate memory for flags
    fstream->flag = malloc(fstream->bitmap_entries_count * sizeof(uint8_t));
    if(fstream->flag == NULL){
        fstream->stream_err = STREAM_ERR_FLAG_MALLOC;
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        goto exit_error;
    }
    memset(fstream->flag, CHUNK_NONE, fstream->bitmap_entries_count * sizeof(uint8_t));

    // Allocate memory for chunk memory block pointers
    fstream->pool_block_file_chunk = malloc(fstream->bitmap_entries_count * sizeof(char*));
    if(fstream->pool_block_file_chunk == NULL){
        fstream->stream_err = STREAM_ERR_CHUNK_PTR_MALLOC;
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        goto exit_error;
    }   
    memset(fstream->pool_block_file_chunk, 0, fstream->bitmap_entries_count * sizeof(char*));

    // Construct file name and open file
    // Using snprintf for buffer overflow safety
    int snprintf_res = snprintf(fstream->fn, PATH_SIZE, "E:\\test_file_SID_%d_UID_%d.txt", session_id, file_id);
    if (snprintf_res < 0 || snprintf_res >= PATH_SIZE) {
        fprintf(stderr, "Error: File name construction failed or buffer too small.\n");
        fstream->stream_err = STREAM_ERR_FILENAME; // Suggest adding this error code
        goto exit_error;
    }
    //creating output file
    fstream->fp = fopen(fstream->fn, "wb+"); // "wb+" allows writing and reading, creates or truncates
    if(fstream->fp == NULL){
        fprintf(stderr, "Error creating/opening file for write: %s (errno: %d)\n", fstream->fn, errno);
        fstream->stream_err = STREAM_ERR_FP;
        goto exit_error;
    }

    // Success path
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_SUCCESS;

exit_error:
    //Call file_cleanup_stream to free any partially allocated resources
    file_cleanup_stream(fstream);
    LeaveCriticalSection(&fstream->lock);
    return RET_VAL_ERROR;

}
void file_cleanup_stream(FileStream *fstream){

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
            } else {
                fprintf(stdout, "File stream closed successfully: %s\n", fstream->fn);
            }
            fstream->fp = NULL; // Set the file pointer to NULL after closing.
        }
    }

    // Check if the bitmap array was allocated.
    if(fstream->bitmap != NULL){
        free(fstream->bitmap); // Free the memory allocated for the bitmap.
        fstream->bitmap = NULL; // Set the pointer to NULL to prevent dangling pointers.
    }
    // Check if the flag array was allocated.
    if(fstream->flag != NULL){
        fprintf(stdout, "Freeing flags memory block for file stream: %s\n", fstream->fn);
        free(fstream->flag); // Free the memory allocated for the flags.
    }
    fstream->flag = NULL; // Set the pointer to NULL to prevent dangling pointers.
    // Loop through all possible chunk memory blocks that might have been allocated.
    // The `fstream->bitmap_entries_count` determines the valid range of indices.
    for(long long k = 0; k < fstream->bitmap_entries_count; k++){
        // Return each individual chunk memory block back to the global memory pool.
        // The pool_free function will handle checking for NULL pointers internally.
        if(fstream->pool_block_file_chunk[k] != NULL){
            // Free the memory allocated for each chunk back to the pool.
            // This is necessary to avoid memory leaks and to reuse memory efficiently.
            fprintf(stdout, "Freeing chunk memory block at index: %llu\n", k);
            pool_free(&pool_file_chunk, fstream->pool_block_file_chunk[k]);
        }
        fstream->pool_block_file_chunk[k] = NULL;
    }
    // Check if the array of chunk memory block pointers was allocated.
    if(fstream->pool_block_file_chunk != NULL){
        // Free the memory allocated for the array of char* pointers itself.
        // This must be done AFTER all individual chunks pointed to by this array have been freed to the pool.
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
}


// Clean client resources
void cleanup_client(ClientData *client){

    // Iterate through all possible file stream slots associated with this client.
    // MAX_CLIENT_FILE_STREAMS defines the maximum number of concurrent file transfers a single client can have.
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        // Acquire the critical section lock for the current file stream.
        EnterCriticalSection(&client->file_stream[i].lock);
        // Call the dedicated cleanup function for the current file stream.
        // This function will free memory, close files, and reset the file stream's state.
        file_cleanup_stream(&client->file_stream[i]);
        // Release the critical section lock for the current file stream.
        LeaveCriticalSection(&client->file_stream[i].lock);
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
}