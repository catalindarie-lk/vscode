#define _CRT_SECURE_NO_WARNINGS // Suppress warnings for strcpy, strncpy, etc.

#include "udp_lib.h"
#include "udp_queue.h"
#include "udp_bitmap.h"
#include "udp_hash.h"
#include "udp_file_chunk_pool.h"
#include "safefileio.h"

// --- Constants 
#define SERVER_PORT                     12345       // Port the server listens on
#define RECVFROM_TIMEOUT_MS             100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_SESSION_TIMEOUT_SEC      120       // Seconds after which an inactive client is considered disconnected
#define SERVER_NAME                     "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                     20
#define FILE_PATH "E:\\out_file.txt"


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
    volatile long session_id_counter;        // Global counter for unique session IDs
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
    uint32_t message_id;
    uint32_t message_len;
    uint32_t chars_received;
    uint32_t fragment_count;
    uint32_t bitmap_entries_count;
    char file_name[PATH_SIZE];
}IncomingMessageEntry;

typedef struct{
    BOOL busy;

    uint64_t *bitmap_mem;
    uint8_t* flag;
    char** chunk_mem_block;
    uint32_t session_id;
    uint32_t file_id;
    uint64_t file_size;
    uint64_t fragment_count;  
    uint64_t bytes_received;
    uint64_t bytes_written;
    uint64_t bitmap_entries_count;
    
    BOOL last_bitmap_is_partial;
    BOOL last_chunk_bytes_received;
    uint64_t last_chunk_size;

    char test_file_name[PATH_SIZE];
    FILE *test_file_fp;

    CRITICAL_SECTION mutex;
}IncomingFileEntry;

typedef struct {  
    struct sockaddr_in addr;                // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    
    uint32_t client_id;                     // Unique ID received from the client
    char name[NAME_SIZE];                   // Optional: human-readable identifier received from the client
    uint8_t flag;                           // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;
 
    uint32_t session_id;                    // Unique ID assigned by the server for this clients's session
    volatile time_t last_activity_time;              // Last time the client sent a frame (for timeout checks)             

    uint32_t slot_num;
    uint8_t slot_status;            //0->FREE; 1->BUSY
 
    IncomingMessageEntry recv_slot_mesg[MAX_CLIENT_MESSAGE_STREAMS];
    IncomingFileEntry recv_slot_file[MAX_CLIENT_FILE_STREAMS];
    
 
    volatile long long uid_count;

    Statistics statistics;

} ClientData;

typedef struct{
    ClientData client[MAX_CLIENTS];      // Array of connected clients
    CRITICAL_SECTION mutex;         // For thread-safe access to connected_clients
}ClientList;


ServerData server;
ClientList list;

QueueFrame queue_frame;
QueueFrame queue_frame_ctrl;
QueueSeqNum queue_seq_num;
QueueSeqNum queue_seq_num_ctrl;

HANDLE receive_frame_thread;
HANDLE ack_thread;
HANDLE process_frame_thread;  
HANDLE client_timeout_thread;
HANDLE server_command_thread;

HANDLE check_complete_block_in_bitmap_thread;

UniqueIdentifierNode *uid_hash_table[HASH_SIZE] = {NULL};
MemPoolFileChunk mem_pool_file_chunk;


const char *server_ip = "127.0.0.1"; // IPv4 example

// Client management functions
ClientData* find_client(ClientList *list, const uint32_t session_id);
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
int remove_client(ClientList *list, const uint32_t session_id);

void update_statistics(ClientData *client);

// Handle message fragment helper functions
static void register_ack(QueueSeqNum *queue, ClientData *client, UdpFrame *frame, uint8_t op_code);
static int mesg_match_fragment(ClientData *client, UdpFrame *frame);
static int mesg_validate_fragment(ClientData *client, const int index, UdpFrame *frame);
static int mesg_get_available_slot(ClientData *client);
static int mesg_init_recv_slot(IncomingMessageEntry *entry, const uint32_t message_id, const uint32_t message_len);
static void mesg_attach_fragment(IncomingMessageEntry *entry, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len);
static int mesg_check_completion_and_record(IncomingMessageEntry *entry, const uint32_t session_id);

static int file_match_fragment(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id);
static int file_get_available_slot(ClientData *client);
static void file_attach_fragment_to_chunk(IncomingFileEntry *entry, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size);
static int file_init_recv_slot(IncomingFileEntry *entry, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size);

int handle_file_metadata(ClientData *client, UdpFrame *frame);
int handle_file_fragment(ClientData *client, UdpFrame *frame);
int handle_message_fragment(ClientData *client, UdpFrame *frame);

// Thread functions
unsigned int WINAPI receive_frame_thread_func(void* ptr);
unsigned int WINAPI process_frame_thread_func(void* ptr);
unsigned int WINAPI ack_thread_func(void* ptr);
unsigned int WINAPI client_timeout_thread_func(void* ptr);
unsigned int WINAPI server_command_thread_func(void* ptr);
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
    memset(&list, 0, sizeof(ClientList));
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
    InitializeCriticalSection(&list.mutex);

    pool_init_chunk(&mem_pool_file_chunk);
    InitializeCriticalSection(&mem_pool_file_chunk.mutex);

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
    DeleteCriticalSection(&list.mutex);
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
        update_statistics(&list.client[0]);

    }
    // --- Server Shutdown Sequence ---
    shutdown_server();
    return 0;
}

// Find client by session ID
ClientData* find_client(ClientList *list, const uint32_t session_id) {    
    // Search for the client with the given session ID
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        if(list->client[slot].slot_status == SLOT_FREE) 
            continue;
        if(list->client[slot].session_id == session_id){
            return &list->client[slot];
        }
    }
    return NULL;
}
// Add a new client
ClientData* add_client(ClientList *list, const UdpFrame *recv_frame, const struct sockaddr_in *client_addr) {
    // Assumes list_mutex is locked by caller

    uint32_t free_slot = 0;
    while(free_slot < MAX_CLIENTS){
        if(list->client[free_slot].slot_status == SLOT_FREE) {
            break;
        }
        free_slot++;
    }
    if(free_slot >= MAX_CLIENTS){
        fprintf(stderr, "\nMax clients reached. Cannot add new client.\n");
        return NULL;
    }
    ClientData *new_client = &list->client[free_slot];
    memset(new_client, 0, sizeof(ClientData));

    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        memset(&new_client->recv_slot_file[i], 0, sizeof(IncomingFileEntry));
    }

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        memset(&new_client->recv_slot_mesg[i], 0, sizeof(IncomingMessageEntry));
    }
    
    new_client->slot_num = free_slot;
    new_client->slot_status = SLOT_BUSY;
    memcpy(&new_client->addr, client_addr, sizeof(struct sockaddr_in));
    new_client->connection_status = CLIENT_CONNECTED;
    new_client->last_activity_time = time(NULL);

    new_client->client_id = ntohl(recv_frame->payload.request.client_id); 
    new_client->session_id = (uint32_t)InterlockedIncrement(&server.session_id_counter); // Assign a unique session ID based on current count
    new_client->flag = recv_frame->payload.request.flag;
 
    snprintf(new_client->name, NAME_SIZE, "%.*s", NAME_SIZE - 1, recv_frame->payload.request.client_name);

    inet_ntop(AF_INET, &client_addr->sin_addr, new_client->ip, INET_ADDRSTRLEN);
    new_client->port = ntohs(client_addr->sin_port);

    fprintf(stdout, "\n[ADDING NEW CLIENT] %s:%d Session ID:%d\n", new_client->ip, new_client->port, new_client->session_id);

    #ifdef ENABLE_FRAME_LOG
        create_log_frame_file(0, new_client->session_id, new_client->log_path);
    #endif
    return new_client;
}
// Remove a client
int remove_client(ClientList *list, const uint32_t slot) {
    // Search for the client with the given session ID
    if(list == NULL){
        fprintf(stderr, "\nInvalid client pointer!\n");
        return RET_VAL_ERROR;
    }
    if (slot < 0 || slot >= MAX_CLIENTS) {
        fprintf(stderr, "\nInvalid client slot nr:  %d", slot);
        return RET_VAL_ERROR; 
    }
    fprintf(stdout, "\nRemoving client with session ID: %d from slot %d\n", list->client[slot].session_id, list->client[slot].slot_num);

    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        free(list->client[slot].recv_slot_file[i].bitmap_mem);
        list->client[slot].recv_slot_file[i].bitmap_mem = NULL;
    }

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        free(list->client[slot].recv_slot_mesg[i].bitmap);
        list->client[slot].recv_slot_mesg[i].bitmap = NULL;
    }


    memset(&list->client[slot], 0, sizeof(ClientData));
    fprintf(stdout, "\nRemoved client successfully!\n");
    return RET_VAL_SUCCESS;
}
// Process received file metadata frame
int handle_file_metadata(ClientData *client, UdpFrame *frame){

     if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        goto exit_error;
    }  

    uint32_t recv_file_id = ntohl(frame->payload.file_metadata.file_id);
    uint64_t recv_file_size = ntohll(frame->payload.file_metadata.file_size);

    EnterCriticalSection(&list.mutex);

    if(search_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        goto exit_error;
    }

    int slot = file_get_available_slot(client);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Maximum file transfers reached!\n");
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_RESOURCE_LIMIT);
        goto exit_error;
    }

    fprintf(stdout, "Received metadata Session ID: %d, File ID: %d, File Size: %llu, Fragment Size: %d\n", client->session_id, recv_file_id, recv_file_size, FILE_FRAGMENT_SIZE);

    if(file_init_recv_slot(&client->recv_slot_file[slot], client->session_id, recv_file_id, recv_file_size) == RET_VAL_ERROR){
        goto exit_error;
    }

    add_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_WAITING_FRAGMENTS);

    //snprintf(client->recv_slot_file[slot].file_name, PATH_SIZE, "E:\\file_SID_%d_UID_%d.txt", client->session_id, recv_file_id);
    snprintf(client->recv_slot_file[slot].test_file_name, PATH_SIZE, "E:\\test_file_SID_%d_UID_%d.txt", client->session_id, recv_file_id);

    client->recv_slot_file[slot].test_file_fp = fopen(client->recv_slot_file[slot].test_file_name, "wb+"); 

    register_ack(&queue_seq_num, client, frame, STS_ACK);
   
    LeaveCriticalSection(&list.mutex);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&list.mutex);
    return RET_VAL_ERROR;
}
// Process received file fragment frame
int handle_file_fragment(ClientData *client, UdpFrame *frame){
    
    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        goto exit_error;
    }
    uint32_t recv_session_id = ntohl(frame->header.session_id);
    uint32_t recv_file_id = ntohl(frame->payload.file_fragment.file_id);
    uint64_t recv_fragment_offset = ntohll(frame->payload.file_fragment.offset);
    uint32_t recv_fragment_size = ntohl(frame->payload.file_fragment.size);
 
    EnterCriticalSection(&list.mutex);

    if(search_uid_hash_table(uid_hash_table, recv_file_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        goto exit_error;
    }

    int slot = file_match_fragment(client, frame, recv_session_id, recv_file_id);
    if(slot == RET_VAL_ERROR){
        fprintf(stderr, "Recived frame with unknown file ID: %d Session ID %d - missing metadata\n", recv_file_id, client->session_id);
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_INVALID_FILE_ID);
        goto exit_error;
    }

    if (client->recv_slot_file[slot].file_id != recv_file_id){
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_INVALID_FILE_ID);
        fprintf(stderr, "No file transfer in progress (no file buffer allocated)!!!\n");
        goto exit_error;
    }
    
    if(check_fragment_received(client->recv_slot_file[slot].bitmap_mem, recv_fragment_offset, FILE_FRAGMENT_SIZE)){
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate frame (offset: %llu)!!!\n", recv_fragment_offset);
        goto exit_error;
    }

    if(recv_fragment_offset >= client->recv_slot_file[slot].file_size){
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);       
        fprintf(stderr, "Received fragment with offset out of limits. File size: %llu, Received offset: %llu\n", client->recv_slot_file[slot].file_size, recv_fragment_offset);
        goto exit_error;
    }
    if (recv_fragment_offset + recv_fragment_size > client->recv_slot_file[slot].file_size){
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME); 
        fprintf(stderr, "Fragment extends past file bounds\n");
        goto exit_error;
    }

    uint64_t entry_index = (recv_fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL;
    if(client->recv_slot_file[slot].bitmap_mem[entry_index] == 0ULL){

        EnterCriticalSection(&mem_pool_file_chunk.mutex);
        
        client->recv_slot_file[slot].chunk_mem_block[entry_index] = pool_alloc_chunk(&mem_pool_file_chunk);
        if(client->recv_slot_file[slot].chunk_mem_block[entry_index] == NULL){
            LeaveCriticalSection(&mem_pool_file_chunk.mutex);
            //fprintf(stdout, "Pool is full! Waiting for free block!\n");
            return RET_VAL_ERROR;
        }
        //fprintf(stdout, "Allocated free pool for index: %llu\n", entry_index);
    }
    LeaveCriticalSection(&mem_pool_file_chunk.mutex);
    file_attach_fragment_to_chunk(&client->recv_slot_file[slot], frame->payload.file_fragment.bytes, recv_fragment_offset, recv_fragment_size);
    
    register_ack(&queue_seq_num, client, frame, STS_ACK);
 
    LeaveCriticalSection(&list.mutex);
    return RET_VAL_SUCCESS;

exit_error:
    LeaveCriticalSection(&list.mutex);
    return RET_VAL_ERROR;
}
// HANDLE received message fragment frame
int handle_message_fragment(ClientData *client, UdpFrame *frame){

    int slot;
    if(client == NULL){
        fprintf(stdout, "Received frame for non existing client context!\n");
        goto exit_error;
    }

    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);
    update_statistics(client);

    EnterCriticalSection(&list.mutex);    

    // Guard against fragments for already completed messages.
    if(search_uid_hash_table(uid_hash_table, recv_message_id, client->session_id, UID_RECV_COMPLETE) == TRUE){
        register_ack(&queue_seq_num_ctrl, client, frame, STS_TRANSFER_COMPLETE);
        goto exit_error;
    }

    // Handle either an existing or a new message stream.

    slot = mesg_match_fragment(client, frame);
    if (slot != RET_VAL_ERROR) {     
        // This is a fragment for an existing message.
        if (mesg_validate_fragment(client, slot, frame) == RET_VAL_ERROR) {
            goto exit_error;
        }
        // Attach the fragment to the buffer.
        mesg_attach_fragment(&client->recv_slot_mesg[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        // Acknowledge the fragment.
        register_ack(&queue_seq_num, client, frame, STS_ACK);
        if (mesg_check_completion_and_record(&client->recv_slot_mesg[slot], client->session_id) == RET_VAL_ERROR) 
            goto exit_error;
        LeaveCriticalSection(&list.mutex);
        return RET_VAL_SUCCESS;
    } else {
        // This is the first fragment of a new message.
        int slot = mesg_get_available_slot(client);
        if (slot == RET_VAL_ERROR){
            fprintf(stderr, "Maximum message streams reached for client ID: %d\n", client->client_id);
            register_ack(&queue_seq_num_ctrl, client, frame, ERR_RESOURCE_LIMIT);
            goto exit_error;
        }
        // Validate the fragment after determining the slot.
        if (mesg_validate_fragment(client, slot, frame) == RET_VAL_ERROR) 
            goto exit_error;
        if (mesg_init_recv_slot(&client->recv_slot_mesg[slot], recv_message_id, recv_message_len) == RET_VAL_ERROR) 
            goto exit_error;
        // Attach the fragment to the buffer.
        mesg_attach_fragment(&client->recv_slot_mesg[slot], frame->payload.long_text_msg.fragment_text, recv_fragment_offset, recv_fragment_len);
        snprintf(client->recv_slot_mesg[slot].file_name, PATH_SIZE, "E:\\msg_SID_%d_UID%d.txt", client->session_id, recv_message_id);
        // Acknowledge the fragment.
        add_uid_hash_table(uid_hash_table, recv_message_id, client->session_id, UID_WAITING_FRAGMENTS);
        register_ack(&queue_seq_num, client, frame, STS_ACK);
        // Check if the message is complete and finalize it.
        if (mesg_check_completion_and_record(&client->recv_slot_mesg[slot], client->session_id) == RET_VAL_ERROR) 
            goto exit_error;
        LeaveCriticalSection(&list.mutex);
        return RET_VAL_SUCCESS;
    }

exit_error:
    LeaveCriticalSection(&list.mutex);
    return RET_VAL_ERROR;
}
// update file transfer progress and speed in MBs
void update_statistics(ClientData * client){

    //update file transfer speed in MB/s
    GetSystemTimePreciseAsFileTime(&client->statistics.ft);
    client->statistics.crt_uli.LowPart = client->statistics.ft.dwLowDateTime;
    client->statistics.crt_uli.HighPart = client->statistics.ft.dwHighDateTime;
    client->statistics.crt_microseconds = client->statistics.crt_uli.QuadPart / 10;

    client->statistics.crt_bytes_received = (float)client->recv_slot_file[0].bytes_received;

    //TRANSFER SPEED
    //current speed (1 cycle)
    client->statistics.file_transfer_speed = (client->statistics.crt_bytes_received - client->statistics.prev_bytes_received) / (float)((client->statistics.crt_microseconds - client->statistics.prev_microseconds));
    client->statistics.prev_bytes_received = client->statistics.crt_bytes_received;
    client->statistics.prev_microseconds = client->statistics.crt_microseconds;
    //PROGRESS - update file transfer progress percentage
    client->statistics.file_transfer_progress = (float)client->recv_slot_file[0].bytes_received / (float)client->recv_slot_file[0].file_size * 100.0;

    fprintf(stdout, "\rFile transfer progress: %.2f %% - Speed: %.2f MB/s", client->statistics.file_transfer_progress, client->statistics.file_transfer_speed);
    fflush(stdout);
}
// --- Receive Thread Function ---
unsigned int WINAPI receive_frame_thread_func(void* ptr) {
    
    UdpFrame received_frame;
    QueueFrameEntry frame_entry;
    
    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);
    int recv_error_code;

    int bytes_received;

    // Set a receive timeout for the thread's socket.
    DWORD timeout = RECVFROM_TIMEOUT_MS;
    if (setsockopt(server.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (server.status == SERVER_READY) {

        memset(&received_frame, 0, sizeof(UdpFrame));
        memset(&src_addr, 0, sizeof(src_addr));

        bytes_received = recvfrom(server.socket, (char*)&received_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            recv_error_code = WSAGetLastError();
            if (recv_error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECVFROM_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", recv_error_code);
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue         
            memset(&frame_entry, 0, sizeof(QueueFrameEntry));
            memcpy(&frame_entry.frame, &received_frame, sizeof(UdpFrame));
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));
            frame_entry.frame_size = bytes_received;
            if(frame_entry.frame_size > sizeof(UdpFrame)){
                fprintf(stdout, "Frame received with bytes > max frame size!\n");
                continue;
            }

            uint8_t frame_type = frame_entry.frame.header.frame_type;
            BOOL is_high_priority_frame = frame_type == FRAME_TYPE_KEEP_ALIVE || frame_type == FRAME_TYPE_CONNECT_REQUEST ||
                                            frame_type == FRAME_TYPE_FILE_METADATA || frame_type == FRAME_TYPE_DISCONNECT;

            QueueFrame *target_queue = NULL;
            if (is_high_priority_frame == TRUE) {
                target_queue = &queue_frame_ctrl;
            } else {
                target_queue = &queue_frame;
            }
            if (push_frame(target_queue, &frame_entry) != RET_VAL_SUCCESS) {
                continue;
            }
        }
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Processes a received frame ---
unsigned int WINAPI process_frame_thread_func(void* ptr) {

    uint16_t header_delimiter;
    uint8_t  header_frame_type;
    uint64_t header_seq_num;
    uint32_t header_session_id;

    QueueFrameEntry frame_entry;
    QueueSeqNumEntry ack_entry;

    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    uint32_t frame_bytes_received;

    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;

    ClientData *client;

    while(server.status == SERVER_READY) {
        // Pop a frame from the queue (prioritize control queue)
        if (pop_frame(&queue_frame_ctrl, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame_ctrl
        } else if (pop_frame(&queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // Successfully popped from queue_frame
        } else {
            Sleep(100); // No frames to process, yield CPU
            continue;
        }

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        frame_bytes_received = frame_entry.frame_size;

        // Extract header fields   
        header_delimiter = ntohs(frame->header.start_delimiter);
        header_frame_type = frame->header.frame_type;
        header_seq_num = ntohll(frame->header.seq_num);
        header_session_id = ntohl(frame->header.session_id);
       
        inet_ntop(AF_INET, &src_addr->sin_addr, src_ip, INET_ADDRSTRLEN);
        src_port = ntohs(src_addr->sin_port);

        if (header_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, header_delimiter);
            continue;
        }

        if (!is_checksum_valid(frame, frame_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }

        // Find or add new client
        client = NULL;
        EnterCriticalSection(&list.mutex);
        if(header_frame_type == FRAME_TYPE_CONNECT_REQUEST){
            client = find_client(&list, header_session_id);
            if(client != NULL){
                client->last_activity_time = time(NULL);
                LeaveCriticalSection(&list.mutex);
                fprintf(stdout, "Client already connected\n");
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr);
                continue;
            }
            client = add_client(&list, frame, src_addr);
            if (client == NULL) {
                LeaveCriticalSection(&list.mutex);
                fprintf(stderr, "Failed to add new client from %s:%d. Max clients reached?\n", src_ip, src_port);
                // Optionally send NACK indicating server full
                continue; // Do not process further if client addition failed
            }            
            
        } else {
            client = find_client(&list, header_session_id);
            if(client == NULL){
                LeaveCriticalSection(&list.mutex);
                //fprintf(stdout, "Received frame from unknown %s:%d. Ignoring...\n", src_ip, src_port);
                continue;
            }
        }
        LeaveCriticalSection(&list.mutex);
        // 3. Process Payload based on Frame Type
        switch (header_frame_type) {
            case FRAME_TYPE_CONNECT_REQUEST:
                client->last_activity_time = time(NULL);
                send_connect_response(header_seq_num, client->session_id, server.session_timeout, server.status, server.name, server.socket, &client->addr);
                break;
            
            case FRAME_TYPE_ACK:
                client->last_activity_time = time(NULL);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues

            case FRAME_TYPE_KEEP_ALIVE:
                client->last_activity_time = time(NULL);
                register_ack(&queue_seq_num_ctrl, client, frame, STS_KEEP_ALIVE);
                break;
                //TODO: Handle ACK processing, e.g., update internal state or queues
                       
            case FRAME_TYPE_FILE_METADATA:
                client->last_activity_time = time(NULL);
                handle_file_metadata(client, frame);
                break;

            case FRAME_TYPE_FILE_FRAGMENT:
                client->last_activity_time = time(NULL);
                handle_file_fragment(client, frame);
                break;

            case FRAME_TYPE_LONG_TEXT_MESSAGE:
                client->last_activity_time = time(NULL);
                handle_message_fragment(client, frame);
                break;

            case FRAME_TYPE_DISCONNECT:
                fprintf(stdout, "Client %s:%d with session ID %d requested disconnect...\n", client->ip, client->port, client->session_id);
                EnterCriticalSection(&list.mutex);
                remove_client(&list, client->slot_num);
                LeaveCriticalSection(&list.mutex);
                break;
            default:
                break;
        }
        #ifdef ENABLE_FRAME_LOG
        if(client != NULL){
            log_frame(LOG_FRAME_RECV, frame, src_addr, client->log_path);
        }           
        #endif      
    }
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Client timeout thread function ---
unsigned int WINAPI client_timeout_thread_func(void* ptr){

    time_t time_now;

    while(server.status == SERVER_READY) {
        
        for(int slot = 0; slot < MAX_CLIENTS; slot++){  
            time_now = time(NULL);
            if(list.client[slot].slot_status == SLOT_FREE){
                continue;
            }                
            if(time_now - (time_t)list.client[slot].last_activity_time < (time_t)server.session_timeout){
                continue;
            }
            fprintf(stdout, "\nClient with Session ID: %d disconnected due to timeout\n", list.client[slot].session_id);
            send_disconnect(list.client[slot].session_id, server.socket, &list.client[slot].addr);
            EnterCriticalSection(&list.mutex);
            remove_client(&list, slot);
            LeaveCriticalSection(&list.mutex);
        }
        Sleep(1000);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- SendAck Thread Function ---
unsigned int WINAPI ack_thread_func(void* ptr){

    ClientData *client = NULL;
    QueueSeqNumEntry entry;

    while (server.status == SERVER_READY) {
        memset(&entry, 0, sizeof(QueueSeqNumEntry));

        if(pop_seq_num(&queue_seq_num_ctrl, &entry) == RET_VAL_SUCCESS){

            fprintf(stdout, "Sending ctrl ack frame for session ID: %d, seq num: %llu, Ack op code: %d\n", entry.session_id, entry.seq_num, entry.op_code);
        } else if(pop_seq_num(&queue_seq_num, &entry) == RET_VAL_SUCCESS){
 
        } else {
            Sleep(100);
            continue;
        }     
        //check if client session still open (could be optional)
        // EnterCriticalSection(&list.mutex);
        // ClientData *client = find_client(&list, seq_num_entry.session_id);
        // LeaveCriticalSection(&list.mutex);
        // if(client == NULL) {
        //     fprintf(stdout, "Client is null\n");
        //     continue; // Nothing to send, skip to next iteration
        // }
        send_ack_nak(entry.seq_num, entry.session_id, entry.op_code, server.socket, &entry.addr);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- Process server command ---
unsigned int WINAPI server_command_thread_func(void* ptr){

    _endthreadex(0);
    return 0;
}

static void register_ack(QueueSeqNum *queue, ClientData *client, UdpFrame *frame, uint8_t op_code) {
    QueueSeqNumEntry entry = {
        .seq_num = ntohll(frame->header.seq_num),
        .op_code = op_code,
        .session_id = ntohl(frame->header.session_id)
    };
    memcpy(&entry.addr, &client->addr, sizeof(struct sockaddr_in));
    push_seq_num(queue, &entry);
}
// Handle message fragment helper functions
static int mesg_match_fragment(ClientData *client, UdpFrame *frame){
    
    uint32_t message_id = ntohl(frame->payload.long_text_msg.message_id);

    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        if(client->recv_slot_mesg[i].message_id == message_id && client->recv_slot_mesg[i].buffer != NULL && client->recv_slot_mesg[i].bitmap != NULL){
            return i;
        }            
    }
    return RET_VAL_ERROR;
}
static int mesg_validate_fragment(ClientData *client, const int index, UdpFrame *frame){

    uint32_t recv_message_id = ntohl(frame->payload.long_text_msg.message_id);
    uint32_t recv_message_len = ntohl(frame->payload.long_text_msg.message_len);
    uint32_t recv_fragment_len = ntohl(frame->payload.long_text_msg.fragment_len);
    uint32_t recv_fragment_offset = ntohl(frame->payload.long_text_msg.fragment_offset);

    BOOL is_duplicate_fragment = client->recv_slot_mesg[index].bitmap && client->recv_slot_mesg[index].buffer &&
                                    check_fragment_received(client->recv_slot_mesg[index].bitmap, recv_fragment_offset, TEXT_FRAGMENT_SIZE);
    if (is_duplicate_fragment == TRUE) {
        //Client already has bitmap and message buffer allocated so fragment can be processed
        //if the message was already received send duplicate frame ack op_code
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_DUPLICATE_FRAME);
        fprintf(stderr, "Received duplicate text message fragment! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }
    if(recv_fragment_offset >= recv_message_len){
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment offset past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }
    if (recv_fragment_offset + recv_fragment_len > recv_message_len || recv_fragment_len > TEXT_FRAGMENT_SIZE) {
        //if the message has invalid payload metadata send ERR_MALFORMED_FRAME ack op code 
        register_ack(&queue_seq_num_ctrl, client, frame, ERR_MALFORMED_FRAME);
        fprintf(stderr, "Fragment len past message bounds! - Session ID: %d, Message ID: %d, Fragment Offset: %d, Fragment Length: %d\n", client->session_id, recv_message_id, recv_fragment_offset, recv_fragment_len);
        return RET_VAL_ERROR;
    }
    return RET_VAL_SUCCESS;
}
static int mesg_get_available_slot(ClientData *client){
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        if(client->recv_slot_mesg[i].message_id == 0 && client->recv_slot_mesg[i].buffer == NULL && client->recv_slot_mesg[i].bitmap == NULL){
            return i;
        }
    }
    return RET_VAL_ERROR;
}
static int mesg_init_recv_slot(IncomingMessageEntry *entry, const uint32_t message_id, const uint32_t message_len){

    entry->message_id = message_id;
    entry->message_len = message_len;

    entry->fragment_count = message_len / TEXT_FRAGMENT_SIZE;
    if((message_len % TEXT_FRAGMENT_SIZE) > 0){
        entry->fragment_count++;
    }
    entry->bitmap_entries_count = entry->fragment_count / 64;  
    if(entry->fragment_count % 64 > 0){
        entry->bitmap_entries_count++;
    }
    entry->bitmap = malloc(entry->bitmap_entries_count * sizeof(uint64_t));
    if(entry->bitmap == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap!!!\n");
        return RET_VAL_ERROR;        
    }
    memset(entry->bitmap, 0, entry->bitmap_entries_count * sizeof(uint64_t));
    
    //copy the received fragment text to the buffer            
    entry->message_id = message_id;
    entry->buffer = malloc(message_len);
    if(entry->buffer == NULL){
        fprintf(stdout, "Error allocating memory!!!\n");
        return RET_VAL_ERROR;
    }
    memset(entry->buffer, 0, message_len);

    return RET_VAL_SUCCESS;

}
static void mesg_attach_fragment(IncomingMessageEntry *entry, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len){
    char *dest = entry->buffer + fragment_offset;
    char *src = fragment_buffer;                                              
    memcpy(dest, src, fragment_len);
    entry->chars_received += fragment_len;       
    mark_fragment_received(entry->bitmap, fragment_offset, TEXT_FRAGMENT_SIZE);
}
static int mesg_check_completion_and_record(IncomingMessageEntry *entry, const uint32_t session_id){
   // Check if the message is fully received by verifying total bytes and the fragment bitmap.
    BOOL message_is_complete = (entry->chars_received == entry->message_len) && check_bitmap(entry->bitmap, entry->fragment_count);

    if (message_is_complete == FALSE) {
        // The message is not yet complete. No action needed for now.
        return RET_VAL_SUCCESS;
    }

    // --- Null terminate the message ---
    entry->buffer[entry->message_len] = '\0';
    // Attempt to write the in-memory buffer to a file on disk.
    int mesg_creation_status = create_output_file(entry->buffer, entry->chars_received, entry->file_name);
    
    // Update the file status in the hash table to mark it as complete.
    // This is done regardless of the file save success, as we won't be receiving more fragments.
    update_uid_status_hash_table(uid_hash_table, session_id, entry->message_id, UID_RECV_COMPLETE);

    // Clean up all dynamically allocated resources for the transfer entry.
    // This block is executed in both success and failure cases of file creation.
    entry->message_id = 0;
    entry->chars_received = 0;
    entry->fragment_count = 0;
    entry->bitmap_entries_count = 0;
    
    free(entry->buffer);
    entry->buffer = NULL;
    
    free(entry->bitmap);
    entry->bitmap = NULL;

    if (mesg_creation_status != RET_VAL_SUCCESS) {
        // If file creation failed, return an error.
        fprintf(stderr, "Error: Failed to create output message for file_id %d\n", entry->message_id);
        return RET_VAL_ERROR;
    }
    
    // File was successfully created and saved.
    return RET_VAL_SUCCESS;
}

// Handle file fragment helper functions
static int file_match_fragment(ClientData *client, UdpFrame *frame, const uint32_t session_id, const uint32_t file_id){   
    for(int i = 0; i < MAX_CLIENT_MESSAGE_STREAMS; i++){
        if(client->recv_slot_file[i].busy == TRUE && client->recv_slot_file[i].session_id == session_id && client->recv_slot_file[i].file_id == file_id){
            return i;
        }            
    }
    return RET_VAL_ERROR;
}
static int file_get_available_slot(ClientData *client){
    for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
        if(client->recv_slot_file[i].busy == FALSE){
            client->recv_slot_file[i].busy = TRUE;
            return i;
        }
    }
    return RET_VAL_ERROR;
}
static void file_attach_fragment_to_chunk(IncomingFileEntry *entry, char *fragment_buffer, const uint64_t fragment_offset, const uint32_t fragment_size){
    
    uint64_t entry_index = (fragment_offset / FILE_FRAGMENT_SIZE) / 64ULL;

    EnterCriticalSection(&entry->mutex);

    char *dest = entry->chunk_mem_block[entry_index] + (fragment_offset % (FILE_FRAGMENT_SIZE * 64ULL));
    char *src = fragment_buffer;

    memcpy(dest, src, fragment_size);
    //fprintf(stdout, "Attaching fragment with offset: %llu to chunk: %llu at offset: %llu\n", fragment_offset, entry_index, (fragment_offset % (FILE_FRAGMENT_SIZE * 64ULL)));
    entry->bytes_received += fragment_size;

    if(entry->last_bitmap_is_partial == TRUE){
        // mark flags for edge case last chunk
        if(entry_index == entry->bitmap_entries_count - 1ULL){
            entry->last_chunk_size += fragment_size;
            entry->flag[entry_index] |= 0b10000000;
            //fprintf(stdout, "Receiving last chunk bytes: %llu\n", entry->last_chunk_size);
        }
        if(entry->bytes_received == entry->file_size){
            entry->last_chunk_bytes_received = TRUE;
            fprintf(stdout, "\nReceived all bytes. Last chunk size is: %llu\n", entry->last_chunk_size);
        }
    }

    mark_fragment_received(entry->bitmap_mem, fragment_offset, FILE_FRAGMENT_SIZE);

    LeaveCriticalSection(&entry->mutex);

}
static int file_init_recv_slot(IncomingFileEntry *entry, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size){

    entry->session_id = session_id;
    entry->file_id = file_id;
    entry->file_size = file_size;

    entry->fragment_count = (entry->file_size + (uint64_t)FILE_FRAGMENT_SIZE - 1ULL) / (uint64_t)FILE_FRAGMENT_SIZE;
    fprintf(stdout, "Fragments count: %llu\n", entry->fragment_count);

    entry->bitmap_entries_count = (entry->fragment_count + 64ULL - 1ULL) / 64ULL;
    fprintf(stdout, "Bitmap 64bits entries needed: %llu\n", entry->bitmap_entries_count);

    //the file size needs to be a multiple of FILE_FRAGMENT_SIZE and nr of fragments needs to be a multiple of 64 due to bitmap entries being 64 bits
    //otherwise the last bitmap entry will not be full of fragments (the mask will not be ~0ULL) and this needs to be treated separately 
    entry->last_bitmap_is_partial = ((entry->file_size % FILE_FRAGMENT_SIZE) > 0ULL) || (((entry->file_size / FILE_FRAGMENT_SIZE) % 64ULL) > 0ULL);
    entry->last_chunk_bytes_received = 0;
    entry->last_chunk_size = 0;

    fprintf(stdout, "Remainder FILE_SIZE modulo FILE_FRAGMENT_SIZE: %llu and FILE_FRAGMENTS modulo 64: %llu\n", entry->file_size % FILE_FRAGMENT_SIZE,(entry->file_size / FILE_FRAGMENT_SIZE) % 64ULL);

    entry->bitmap_mem = malloc(entry->bitmap_entries_count * sizeof(uint64_t));
    if(entry->bitmap_mem == NULL){
        fprintf(stderr, "Memory allocation fail for file bitmap mem!!!\n");
        return RET_VAL_ERROR;
    }
    memset(entry->bitmap_mem, 0, entry->bitmap_entries_count * sizeof(uint64_t));

    entry->flag = malloc(entry->bitmap_entries_count * sizeof(uint8_t));
    if(entry->flag == NULL){
        fprintf(stderr, "Memory allocation fail for file entry flag!!!\n");
        return RET_VAL_ERROR;
    }
    memset(entry->flag, 0, entry->bitmap_entries_count * sizeof(uint8_t));

    entry->chunk_mem_block = malloc(entry->bitmap_entries_count * sizeof(char*));
    if(entry->chunk_mem_block == NULL){
        fprintf(stderr, "Memory allocation fail for chunk mem blocks!!!\n");
        return RET_VAL_ERROR;
    }   
    memset(entry->chunk_mem_block, 0, entry->bitmap_entries_count * sizeof(char*));

    entry->bytes_received = 0;
    entry->bytes_written = 0;
    InitializeCriticalSection(&entry->mutex);
    
    return RET_VAL_SUCCESS;
}

unsigned int WINAPI check_complete_blocks_in_bitmap_func(LPVOID lpParam){

    while (server.status == SERVER_READY) {
        
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(list.client[i].connection_status == CLIENT_CONNECTED){
                for(int j = 0; j < MAX_CLIENT_FILE_STREAMS; j++){
                    if(list.client[i].recv_slot_file[j].file_id != 0){
                        for(long long k = 0; k < list.client[i].recv_slot_file[j].bitmap_entries_count; k++){

                            IncomingFileEntry *entry = &list.client[i].recv_slot_file[j];
                            EnterCriticalSection(&entry->mutex);
                            FILE *fp = entry->test_file_fp;
                            uint64_t file_offset = k * FILE_FRAGMENT_SIZE * 64;

                            if (!fp) {
                                LeaveCriticalSection(&entry->mutex);
                                fprintf(stderr, "Error: FILE pointer is null for chunk %llu\n", k);
                                return RET_VAL_ERROR; 
                            }

                            // edge case for last chunk if the chunk has less fragments - check the flags
                            if (entry->last_bitmap_is_partial && entry->last_chunk_bytes_received && entry->flag[k] == 0b10000000){                              
                                // (bit 7) - 128 - this bit is set if entry is last incomplete bitmap entry (last partial chunk)
                                // (bit 0) - this bit is set after the chunk bytes are written to the file
                                
                                if (_fseeki64(fp, file_offset, SEEK_SET) != 0) {
                                    LeaveCriticalSection(&entry->mutex);
                                    fclose(fp);
                                    fprintf(stderr, "Error: Failed to seek to offset %llu\n", file_offset);
                                    return RET_VAL_ERROR;
                                }

                                // Write chunk to file
                                char* buffer = entry->chunk_mem_block[k];
                                uint64_t buffer_size = entry->last_chunk_size;
                                
                                fprintf(stdout, "Writing last chunk bytes: %llu, chunk index: %llu\n", buffer_size, k);

                                size_t written = fwrite(buffer, 1, buffer_size, fp);
                                if (written != buffer_size) {
                                    LeaveCriticalSection(&entry->mutex);
                                    fclose(fp);
                                    fprintf(stderr, "Error: Failed to write data (expected %llu, wrote %llu)\n", buffer_size, written);
                                    return RET_VAL_ERROR;
                                }
                                entry->bytes_written += written;
                                entry->flag[k] = 0b10000001;

                                LeaveCriticalSection(&entry->mutex);

                                EnterCriticalSection(&mem_pool_file_chunk.mutex);
                                pool_free_chunk(&mem_pool_file_chunk, entry->chunk_mem_block[k]);
                                LeaveCriticalSection(&mem_pool_file_chunk.mutex);

                                BOOL file_is_complete = (entry->bytes_received == entry->file_size) && (entry->bytes_written == entry->file_size);
                                if(file_is_complete){
                                    LeaveCriticalSection(&entry->mutex);
                                    fclose(fp);
                                    update_uid_status_hash_table(uid_hash_table, entry->session_id, entry->file_id, UID_RECV_COMPLETE);
                                    fprintf(stdout, "Transfer finished created file: %s, bytes: %llu\n", entry->test_file_name, entry->bytes_written);
                                    memset(entry, 0, sizeof(IncomingFileEntry));
                                }

                            // if all 64 bits are set in the bitmap entry then this chunk is complete and can be written to disk
                            } else if(entry->bitmap_mem[k] == ~0ULL && entry->flag[k] == 0b00000000){
 
                                if (_fseeki64(fp, file_offset, SEEK_SET) != 0) {
                                    LeaveCriticalSection(&entry->mutex);
                                    fclose(fp);
                                    fprintf(stderr, "Error: Failed to seek to offset %llu\n", file_offset);
                                    return RET_VAL_ERROR;
                                }

                                // Write chunk to file
                                char* buffer = entry->chunk_mem_block[k];
                                uint64_t buffer_size = FILE_FRAGMENT_SIZE * 64;

                                size_t written = fwrite(buffer, 1, buffer_size, fp);
                                if (written != buffer_size) {
                                    LeaveCriticalSection(&entry->mutex);
                                    fclose(fp);
                                    fprintf(stderr, "Error: Failed to write data (expected %llu, wrote %llu)\n", buffer_size, written);
                                    return RET_VAL_ERROR;
                                }
                                entry->bytes_written += written;
                                entry->flag[k] = 1;

                                LeaveCriticalSection(&entry->mutex);

                                EnterCriticalSection(&mem_pool_file_chunk.mutex);
                                pool_free_chunk(&mem_pool_file_chunk, entry->chunk_mem_block[k]);
                                LeaveCriticalSection(&mem_pool_file_chunk.mutex);
                                BOOL file_is_complete = (entry->bytes_received == entry->file_size) && (entry->bytes_written == entry->file_size);
                                if(file_is_complete){
                                    fclose(fp);
                                    update_uid_status_hash_table(uid_hash_table, entry->session_id, entry->file_id, UID_RECV_COMPLETE);
                                    fprintf(stdout, "Transfer finished created file: %s, bytes: %llu\n", entry->test_file_name, entry->bytes_written);
                                    memset(entry, 0, sizeof(IncomingFileEntry));
                                }                     
                            } else {
                                LeaveCriticalSection(&entry->mutex);
                            }
                        }
                    }
                }
            }
        }
        Sleep(200);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}


