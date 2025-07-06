// --- udp_client.c ---
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
#include "hash.h"
#include "mem_pool.h"


// --- Constants ---
#define SERVER_PORT                     12345       // Port the server listens on
#define FRAME_DELIMITER                 0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS                 100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_ID                       0xAA        // Example client ID, can be set dynamically
#define CLIENT_NAME                     "lkdc UDP Text/File Transfer Client"

#define TEXT_CHUNK_SIZE                 (TEXT_FRAGMENT_SIZE * 128)
#define FILE_CHUNK_SIZE                 (FILE_FRAGMENT_SIZE * 128)

#define RESEND_TIMEOUT                  10           //seconds
#define RESEND_TIME_TRANSFER            1000        //miliseconds
#define RESEND_TIME_IDLE                10          //miliseconds

#define MAX_MESSAGE_SIZE                (INT32_MAX - 1)         // Max size of a long text message

#define HASH_FRAME_HIGH_WATERMARK       (HASH_SIZE_FRAME * 0.5)
#define HASH_FRAME_LOW_WATERMARK        (HASH_SIZE_FRAME * 0.25)

#define BLOCK_SIZE_FRAME                ((uint64_t)(sizeof(AckHashNode)))
#define BLOCK_COUNT_FRAME               ((uint64_t)(HASH_FRAME_HIGH_WATERMARK * 2))

typedef uint8_t SessionStatus;
enum SessionStatus{
    SESSION_DISCONNECTED = 0,
    SESSION_CONNECTING = 1,
    SESSION_CONNECTED = 2
};

typedef uint8_t ClientStatus;
enum ClientStatus {
    CLIENT_STOP = 0,
    CLIENT_BUSY = 1,
    CLIENT_READY = 2,
    CLIENT_ERROR = 3
};


typedef struct{
    SOCKET socket;
    struct sockaddr_in client_addr;
    struct sockaddr_in server_addr;

    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];
    ClientStatus client_status;         
    SessionStatus session_status;     // 0-DISCONNECTED; 1-CONNECTED
    time_t last_active_time;
   
    volatile uint64_t frame_count;       // this will be sent as seq_num
    CRITICAL_SECTION frame_count_mutex;

    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[NAME_SIZE];       // Human readable server name
    
    //uint32_t file_id_count;  
    uint64_t file_size[MAX_CLIENT_FILE_STREAMS];
    uint64_t file_bytes_to_send[MAX_CLIENT_FILE_STREAMS];

    //uint32_t message_id_count;
    long message_len[MAX_CLIENT_MESSAGE_STREAMS];
    uint32_t message_bytes_to_send[MAX_CLIENT_MESSAGE_STREAMS];

    volatile long uid;

    long hash_count;
    BOOL file_throttle;

    char log_path[PATH_SIZE];
 
} ClientData;

ClientData client;

QueueFrame queue_frame;
QueueFrame queue_frame_ctrl;

HANDLE recieve_frame_thread;
HANDLE process_frame_thread;
HANDLE resend_frame_thread;
HANDLE keep_alive_thread;
HANDLE command_thread;


HANDLE file_transfer_thread[MAX_CLIENT_FILE_STREAMS];
HANDLE file_transfer_event[MAX_CLIENT_FILE_STREAMS];
HANDLE file_metadata_event[MAX_CLIENT_FILE_STREAMS];
uint64_t pending_metadata_seq_num[MAX_CLIENT_FILE_STREAMS];

HANDLE message_send_thread[MAX_CLIENT_MESSAGE_STREAMS];
HANDLE message_send_event[MAX_CLIENT_MESSAGE_STREAMS];

HANDLE connection_successfull;
//HANDLE send_file_event;
//HANDLE metadata_confirmed_event;



AckHashNode *frame_hash_table[HASH_SIZE_FRAME] = {NULL};
CRITICAL_SECTION frame_hash_table_mutex;

MemPool frame_mem_pool;

const char *server_ip = "10.10.10.3"; // loopback address
const char *client_ip = "10.10.10.1";


unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI resend_frame_thread_func(LPVOID lpParam);
unsigned int WINAPI keep_alive_thread_func(LPVOID lpParam);
unsigned int WINAPI file_transfer_thread_func(LPVOID lpParam);
unsigned int WINAPI message_send_thread_func(LPVOID lpParam);
unsigned int WINAPI command_thread_func(LPVOID lpParam);

long get_text_file_size(const char *filepath){
    FILE *fp = fopen(filepath, "rb"); // Open in binary mode
    if (fp == NULL) {
         fprintf(stderr, "Error: Could not open file!\n");
        return RET_VAL_ERROR;
    }
    // Seek to end to determine file size
    if(fseek(fp, 0, SEEK_END)){
       fprintf(stderr, "Failed to seek");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    long size = ftell(fp);
    if(size == RET_VAL_ERROR){
        fprintf(stdout, "Error reading text file size! ftell()\n");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    return(size);
}
// read file size (big files)
long long get_file_size(const char *filepath){

    FILE *fp = fopen(filepath, "rb"); // Open in binary mode
    if (fp == NULL) {
        fprintf(stderr, "Error: Could not open file!\n");
        return RET_VAL_ERROR;
    }
    // Seek to end to determine file size
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    long long size = _ftelli64(fp);
    if(size == RET_VAL_ERROR){
        fprintf(stderr, "Error reading file size! _ftelli64()\n");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    return(size);
}
int read_text_file(const char *filepath, char *buffer, long size) {
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        printf("Error: Could not open file.\n");
        return -1;
    }
   if (buffer == NULL) {
        printf("Buffer error.\n");
        return -1;
    }
    memset(buffer, 0, size);
    // Read file into buffer
    fread(buffer, size, 1, file);
    buffer[size] = '\0'; // Null-terminate the buffer

    fclose(file);
    return 0;
}
int read_file(const char *filepath, char *buffer, long size) {
    FILE *file = fopen(filepath, "rb"); // Open in binary mode
    if (file == NULL) {
        fprintf(stderr,"Error: Could not open file!!!\n");
        return -1;
    }
   if (buffer == NULL) {
        fprintf(stderr, "Buffer error!!!\n");
        return -1;
    }
    memset(buffer, 0, size);
    // Read file into buffer
    fread(buffer, size, 1, file);
    
    fclose(file);
    return 0;
}


// get new sequence num
uint64_t get_new_seq_num(){
    return InterlockedIncrement64(&client.frame_count);
}
// initialize client
int init_client(){
    
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", iResult);
        exit(EXIT_FAILURE);
        return RET_VAL_ERROR;
    }

    memset(&client, 0, sizeof(ClientData));

    client.client_status = CLIENT_BUSY;

    client.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client.socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    client.client_addr.sin_family = AF_INET;
    client.client_addr.sin_port = htons(0); // Let OS choose port
    client.client_addr.sin_addr.s_addr = inet_addr(client_ip);

    if (bind(client.socket, (struct sockaddr *)&client.client_addr, sizeof(client.client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        WSACleanup();
        return RET_VAL_ERROR;
    }
   
    // Define server address
    memset(&client.server_addr, 0, sizeof(client.server_addr));
    client.server_addr.sin_family = AF_INET;
    client.server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        WSACleanup();
        return RET_VAL_ERROR;
    };

    //Initialize frame buffers (queue)
    queue_frame.head = 0;
    queue_frame.tail = 0;
    InitializeCriticalSection(&queue_frame.mutex);

    queue_frame_ctrl.head = 0;
    queue_frame_ctrl.tail = 0;
    InitializeCriticalSection(&queue_frame_ctrl.mutex);
    
    client.frame_count = (uint64_t)1;//(uint64_t)UINT32_MAX;
    client.uid = 0xCC;
    snprintf(client.client_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, CLIENT_NAME);
    client.client_id = CLIENT_ID;
    client.client_status = CLIENT_READY;
    client.session_status = SESSION_DISCONNECTED;
    client.log_path[0] = '\0';
    InitializeCriticalSection(&client.frame_count_mutex);
    InitializeCriticalSection(&frame_hash_table_mutex); 

    connection_successfull = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (connection_successfull == NULL) {
        fprintf(stdout, "CreateEvent failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }

    frame_mem_pool.block_size = BLOCK_SIZE_FRAME;
    frame_mem_pool.block_count = BLOCK_COUNT_FRAME;
    pool_init(&frame_mem_pool);
    
    return RET_VAL_SUCCESS;
}
// start client threads
void start_threads(){

    process_frame_thread = (HANDLE)_beginthreadex(NULL, 0, process_frame_thread_func, NULL, 0, NULL);
    if (process_frame_thread == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    resend_frame_thread = (HANDLE)_beginthreadex(NULL, 0, resend_frame_thread_func, NULL, 0, NULL);
    if (resend_frame_thread == NULL) {
        fprintf(stderr, "Failed to create resend frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    command_thread = (HANDLE)_beginthreadex(NULL, 0, command_thread_func, NULL, 0, NULL);
    if (command_thread == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STOP; // Signal immediate shutdown
    }
    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        file_transfer_event[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        file_metadata_event[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        file_transfer_thread[index] = (HANDLE)_beginthreadex(NULL, 0, file_transfer_thread_func, (LPVOID)(intptr_t)index, 0, NULL);

        if (file_transfer_event[index] == NULL || file_transfer_thread[index] == NULL){
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STOP; // Signal immediate shutdown
        }
    }
   for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        message_send_event[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        message_send_thread[index] = (HANDLE)_beginthreadex(NULL, 0, message_send_thread_func, (LPVOID)(intptr_t)index, 0, NULL);

        if (message_send_event[index] == NULL || message_send_thread[index] == NULL){
            fprintf(stderr, "Failed to create message send thread. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STOP; // Signal immediate shutdown
        }
    }
}
// shutdown client
void shutdown_client(){

    client.client_status = CLIENT_STOP;

    if (recieve_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(recieve_frame_thread, INFINITE);
        CloseHandle(recieve_frame_thread);
    }
    fprintf(stdout,"receive frame thread closed...\n");
    if (process_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(process_frame_thread, INFINITE);
        CloseHandle(process_frame_thread);
    }
    fprintf(stdout,"process frame thread closed...\n");
    if (resend_frame_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(resend_frame_thread, INFINITE);
        CloseHandle(resend_frame_thread);
    }
    fprintf(stdout,"resend frame thread closed...\n");
    if (command_thread) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(command_thread, INFINITE);
        CloseHandle(command_thread);
    }   
    fprintf(stdout,"command thread closed...\n");


    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        if (file_transfer_thread[index]) {
            // Signal the receive thread to stop and wait for it to finish
            WaitForSingleObject(file_transfer_thread[index], INFINITE);
            CloseHandle(file_transfer_thread[index]);
        }
        if (file_transfer_event[index]) {
            CloseHandle(file_transfer_event[index]);
        }
        if (file_metadata_event[index]) {
            CloseHandle(file_metadata_event[index]);
        }
    }
    fprintf(stdout,"file transfer threads closed...\n");


    for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        if (message_send_thread[index]) {
            // Signal the receive thread to stop and wait for it to finish
            WaitForSingleObject(message_send_thread[index], INFINITE);
            CloseHandle(message_send_thread[index]);
        }
        if (message_send_event[index]) {
            CloseHandle(message_send_event[index]);
        }
    }
    fprintf(stdout,"message send threads closed...\n");



    DeleteCriticalSection(&queue_frame.mutex);
    DeleteCriticalSection(&queue_frame_ctrl.mutex);
    DeleteCriticalSection(&client.frame_count_mutex);
    CloseHandle(connection_successfull);
    CloseHandle(message_send_event);
    closesocket(client.socket);
    WSACleanup();
}
// --- Main function ---
int main() {

    init_client();   
    start_threads();

        while(client.client_status == CLIENT_BUSY || client.client_status == CLIENT_READY){
        fprintf(stdout, "\rProgress File: %.2f, Progress Text: %.2f, Hash queue frames: %d", (float)(client.file_size[0] - client.file_bytes_to_send[0]) / (float)client.file_size[0] * 100.0, 
                                                                                                                (float)(client.message_len[0] - client.message_bytes_to_send[0]) / (float)client.message_len[0] * 100.0,
                                                                                                                    client.hash_count);
        fflush(stdout);

        if(client.session_status == SESSION_DISCONNECTED){
            EnterCriticalSection(&client.frame_count_mutex);
            client.frame_count = UINT32_MAX;       // this will be sent as seq_num
            LeaveCriticalSection(&client.frame_count_mutex);
            client.uid = 0xCC;
            client.log_path[0] = '\0'; 
            client.session_id = 0;
            client.server_status = 0;
            client.session_timeout = 0;           
        }     
        Sleep(100); // Simulate some delay between messages        
    }
    fprintf(stdout, "Client shutting down!!!\n");
    shutdown_client(&client);
    
    return 0;
}
// --- Function implementations ---
// Send file metadata frame
int send_file_metadata(const uint64_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint64_t file_size, const uint32_t file_fragment_size, 
                                        const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;

    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = htonl(file_id);
    frame.payload.file_metadata.file_size = htonll(file_size);
       
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));  
    
    if(insert_frame(frame_hash_table, &frame_hash_table_mutex, &frame, &client.hash_count, &frame_mem_pool) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_path);
    #endif
    return bytes_sent;
}
// Send file fragment frame
int send_file_fragment(const uint64_t seq_num, const uint32_t session_id, const uint32_t file_id, const uint64_t fragment_offset, 
                                        const char* fragment_buffer, const uint32_t fragment_size, const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "\nInvalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_FRAGMENT;
    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.file_fragment.file_id = htonl(file_id);
    frame.payload.file_fragment.size = htonl(fragment_size);
    frame.payload.file_fragment.offset = htonll(fragment_offset);
    memcpy(frame.payload.file_fragment.bytes, fragment_buffer, fragment_size);
    
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileFragmentPayload)));  
    if(insert_frame(frame_hash_table, &frame_hash_table_mutex, &frame, &client.hash_count, &frame_mem_pool) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    #ifdef ENABLE_FRAME_LOG
        log_frame(LOG_FRAME_SENT, &frame, dest_addr, client.log_path);
    #endif
    return bytes_sent;
}
// --- Send long text message fragment ---
int send_long_text_fragment(const uint64_t seq_num, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len, 
                                        const uint32_t fragment_offset, const char* fragment_buffer, const uint32_t fragment_len, const SOCKET src_socket, const struct sockaddr_in *dest_addr){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "Invalid text parsed!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_LONG_TEXT_MESSAGE;
    frame.header.seq_num = htonll(seq_num);
    frame.header.session_id = htonl(session_id);
    // Set the payload fields
    frame.payload.long_text_msg.message_id = htonl(message_id);
    frame.payload.long_text_msg.message_len = htonl(message_len);
    frame.payload.long_text_msg.fragment_len = htonl(fragment_len);
    frame.payload.long_text_msg.fragment_offset = htonl(fragment_offset);
    
    memcpy(frame.payload.long_text_msg.fragment_text, fragment_buffer, fragment_len);
    
    // Calculate the checksum for the frame
    frame.header.checksum = htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  
    if(insert_frame(frame_hash_table, &frame_hash_table_mutex, &frame, &client.hash_count, &frame_mem_pool) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is fool, failed to allocate!\n");
        return RET_VAL_ERROR;
    }
    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}
// --- Receive frame ---
unsigned int WINAPI receive_frame_thread_func(LPVOID lpParam) {

    UdpFrame recv_frame;
    QueueFrameEntry frame_entry;
    DWORD timeout = RECV_TIMEOUT_MS;
    int bytes_received;

    struct sockaddr_in src_addr;
    int src_addr_len = sizeof(src_addr);
    int error_code;

    if (setsockopt(client.socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "receive_thread_func: setsockopt SO_RCVTIMEO failed with error: %d\n", WSAGetLastError());
        // Do not exit, but log the error
    }
    
    while (client.session_status == SESSION_CONNECTING || client.session_status == SESSION_CONNECTED) {

        memset(&recv_frame, 0, sizeof(UdpFrame));
        memset(&src_addr, 0, sizeof(src_addr));
        
        bytes_received = recvfrom(client.socket, (char*)&recv_frame, sizeof(UdpFrame), 0, (SOCKADDR*)&src_addr, &src_addr_len);
        if (bytes_received == SOCKET_ERROR) {
            error_code = WSAGetLastError();
            if (error_code != WSAETIMEDOUT) { // WSAETIMEDOUT is expected if no data for RECV_TIMEOUT_MS
                fprintf(stderr, "recvfrom failed with error: %d\n", error_code);
                continue;
            }
        } else if (bytes_received > 0) {
            // Push the received frame to the frame queue           
            memset(&frame_entry, 0, sizeof(QueueFrameEntry));
            memcpy(&frame_entry.frame, &recv_frame, sizeof(UdpFrame));
            memcpy(&frame_entry.src_addr, &src_addr, sizeof(struct sockaddr_in));          
            frame_entry.frame_size = bytes_received;
            if(frame_entry.frame_size > sizeof(UdpFrame)){
                fprintf(stdout, "Frame received with bytes > max frame size!\n");
                continue;
            }

            uint8_t frame_type = frame_entry.frame.header.frame_type;
            uint8_t op_code = frame_entry.frame.payload.ack.op_code;
            
            BOOL is_high_priority_frame = (frame_type == FRAME_TYPE_CONNECT_RESPONSE ||
                                            frame_type == FRAME_TYPE_DISCONNECT ||
                                            (frame_type == FRAME_TYPE_ACK && op_code == STS_KEEP_ALIVE) ||
                                            (frame_type == FRAME_TYPE_ACK && op_code == ERR_DUPLICATE_FRAME) ||
                                            (frame_type == FRAME_TYPE_ACK && op_code == STS_TRANSFER_COMPLETE));
  
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
unsigned int WINAPI process_frame_thread_func(LPVOID lpParam) {

    QueueFrameEntry frame_entry;
    UdpFrame *frame;
    struct sockaddr_in *src_addr;
    char src_ip[INET_ADDRSTRLEN];
    uint16_t src_port;
    uint32_t recvfrom_bytes_received;

    uint16_t received_delimiter;
    uint8_t  received_frame_type;
    uint64_t received_seq_num;
    uint32_t received_session_id;    

    uint32_t received_session_timeout;
    uint8_t received_server_status;

    while(client.client_status == CLIENT_READY){
        // Pop a frame from the queue (prioritize control queue)
        if (pop_frame(&queue_frame_ctrl, &frame_entry) == RET_VAL_SUCCESS) {
            //fprintf(stdout, "Processing control frame type: %d, opcode: %d, seq num: %llu\n", frame_entry.frame.header.frame_type, frame_entry.frame.payload.ack.op_code, ntohll(frame_entry.frame.header.seq_num));
            // Successfully popped from queue_frame_ctrl
        } else if (pop_frame(&queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            //fprintf(stdout, "Processing frame type: %d, opcode: %d, seq num: %llu\n", frame_entry.frame.header.frame_type, frame_entry.frame.payload.ack.op_code, ntohll(frame_entry.frame.header.seq_num));
            // Successfully popped from queue_frame
        } else {
            Sleep(100); // No frames to process, yield CPU
            continue;
        }     

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        recvfrom_bytes_received = frame_entry.frame_size;

        // Extract header fields   
        received_delimiter = ntohs(frame->header.start_delimiter);
        received_frame_type = frame->header.frame_type;
        received_seq_num = ntohll(frame->header.seq_num);
        received_session_id = ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        src_port = ntohs(src_addr->sin_port);
       
        if (received_delimiter != FRAME_DELIMITER) {
            fprintf(stderr, "Received frame from %s:%d with invalid delimiter: 0x%X. Discarding.\n", src_ip, src_port, received_delimiter);
            continue;
        }        
        if (!is_checksum_valid(frame, recvfrom_bytes_received)) {
            fprintf(stderr, "Received frame from %s:%d with checksum mismatch. Discarding.\n", src_ip, src_port);
            // Optionally send ACK for checksum mismatch if this is part of a reliable stream
            // For individual datagrams, retransmission is often handled by higher layers or ignored.
            continue;
        }
        switch (received_frame_type) {
            case FRAME_TYPE_CONNECT_RESPONSE:
                received_server_status = frame->payload.response.server_status;
                received_session_timeout = ntohl(frame->payload.response.session_timeout);               
                if(received_session_id == 0 || received_server_status == 0){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                if(received_session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }                
                client.server_status = received_server_status;
                client.session_timeout = received_session_timeout;
                client.session_id = received_session_id;
                snprintf(client.server_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, frame->payload.response.server_name);
                client.last_active_time = time(NULL);
                client.session_status = SESSION_CONNECTED;

                SetEvent(connection_successfull);
                
                keep_alive_thread = (HANDLE)_beginthreadex(NULL, 0, keep_alive_thread_func, NULL, 0, NULL);
                if (keep_alive_thread == NULL) {
                    fprintf(stderr, "Failed to create keep alive thread. Error: %d\n", GetLastError());
                    client.session_status = SESSION_DISCONNECTED;
                    client.client_status = CLIENT_STOP; // Signal immediate shutdown
                }
                break; 

            case FRAME_TYPE_ACK:
                //fprintf(stdout, "received ack frame %llu\n", received_seq_num);
                if(received_session_id != client.session_id){
                    fprintf(stderr, "Received ACK frame with invalid session ID: %d", received_session_id);
                    //TODO - send ACK frame with error code for invalid session ID
                    break;
                }
                client.last_active_time = time(NULL);
                uint8_t op_code = frame->payload.ack.op_code;
                for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
                    if(received_seq_num == pending_metadata_seq_num[i] && op_code == STS_ACK){
                        SetEvent(file_metadata_event[i]);
                    }
                }
                
                if(op_code == STS_ACK || op_code == STS_KEEP_ALIVE || op_code == ERR_DUPLICATE_FRAME || op_code == STS_TRANSFER_COMPLETE){
                    remove_frame(frame_hash_table, &frame_hash_table_mutex, received_seq_num, &client.hash_count, &frame_mem_pool);
                }
                break;

            case FRAME_TYPE_DISCONNECT:
                if(received_session_id == client.session_id){
                    client.session_status = SESSION_DISCONNECTED;
                    fprintf(stdout, "Session closed by server...\n");
                }
                break;

            case FRAME_TYPE_CONNECT_REQUEST:
                break;
                
            case FRAME_TYPE_KEEP_ALIVE:
                break;
            default:
                break;
        }
        #ifdef ENABLE_FRAME_LOG
            log_frame(LOG_FRAME_RECV, frame, src_addr, client.log_path);
        #endif
    }
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Send keep alive ---
unsigned int WINAPI keep_alive_thread_func(LPVOID lpParam){

    time_t now;

    while(client.session_status == SESSION_CONNECTED){
        DWORD keep_alive_clock = (DWORD)((DWORD)client.session_timeout / 5 * 1000);
        uint64_t seq_num = get_new_seq_num();
        send_keep_alive(seq_num, client.session_id, client.socket, &client.server_addr);
        fprintf(stdout, "\nSending keep alive frame seq num: %llu\n", seq_num);
        if(time(NULL) > (time_t)(client.last_active_time + client.session_timeout * 2)){
            client.session_status = SESSION_DISCONNECTED;
        }
        Sleep(keep_alive_clock);
    }
    _endthreadex(0);
    return 0;
}
// --- Re-send frames that ack time expired ---
unsigned int WINAPI resend_frame_thread_func(LPVOID lpParam){
   
    while(client.client_status == CLIENT_READY){
        if(client.session_status != SESSION_CONNECTED){
            clean_frame_hash_table(frame_hash_table, &frame_hash_table_mutex, &client.hash_count, &frame_mem_pool);
            Sleep(1000);
            continue;
        }
        time_t current_time = time(NULL);
        EnterCriticalSection(&frame_hash_table_mutex);      
        for (int i = 0; i < HASH_SIZE_FRAME; i++) {                           
            if(frame_hash_table[i]){                       
                AckHashNode *ptr = frame_hash_table[i];
                while (ptr) {     
                    if(current_time - ptr->time > (time_t)RESEND_TIMEOUT){                        
                        send_frame(&ptr->frame, client.socket, &client.server_addr);
                        ptr->time = current_time;
                    }
                    ptr = ptr->next;
                }
            }                                                
        }
        LeaveCriticalSection(&frame_hash_table_mutex);
        //fprintf(stdout, "Bytes to send: %d\n", client.total_bytes_to_send);
        uint64_t sleep_time = RESEND_TIME_IDLE;
        for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
            if(client.file_bytes_to_send[i] > 0){
                sleep_time = RESEND_TIME_TRANSFER;
            }
            Sleep(sleep_time);
        }
        
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- File transfer thread function ---
unsigned int WINAPI file_transfer_thread_func(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;

    char *file_path = "E:\\test_file.txt";
    FILE *file = NULL;

    long long file_size;

    uint8_t chunk_buffer[FILE_CHUNK_SIZE];
    uint64_t remaining_bytes_to_send;
    uint32_t chunk_bytes_to_send;
    uint32_t chunk_fragment_offset;

    uint32_t frame_fragment_size;
    uint64_t frame_fragment_offset;

    uint32_t file_id;

    BOOL throttle;

    while(client.client_status == CLIENT_READY){
        
        WaitForSingleObject(file_transfer_event[index], INFINITE);

        if(client.session_status != SESSION_CONNECTED){
            fprintf(stdout, "Session with server closed!!!\n");
            continue;           
        }
        file = fopen(file_path, "rb");
        if(file == NULL){
            fprintf(stdout, "Error opening file!!!\n");
            continue;
        }
        file_size = get_file_size(file_path);       
        if(file_size == RET_VAL_ERROR){
            client.file_size[index] = 0;
            continue;
        }
        client.file_size[index] = file_size;

        file_id = (uint32_t)InterlockedIncrement(&client.uid);

        pending_metadata_seq_num[index] = get_new_seq_num();
        int metadata_bytes_sent = send_file_metadata(pending_metadata_seq_num[index], client.session_id, file_id, file_size, FILE_FRAGMENT_SIZE, client.socket, &client.server_addr);
        if(metadata_bytes_sent == RET_VAL_ERROR){
            ResetEvent(file_transfer_event[index]);
            fprintf(stderr, "Failed to send file metadata frame. Cancelling transfe\n");
            continue;
        }

        WaitForSingleObject(file_metadata_event[index], INFINITE);
        ResetEvent(file_metadata_event[index]);

        frame_fragment_offset = 0;
        remaining_bytes_to_send = file_size;

        while(remaining_bytes_to_send > 0){
            if(client.session_status != SESSION_CONNECTED){
                fprintf(stdout, "Session with server closed!!!\n");
                remaining_bytes_to_send = 0;
                continue;
            }
            if(client.hash_count > HASH_FRAME_HIGH_WATERMARK){
                throttle = TRUE;
            }
            if(client.hash_count < HASH_FRAME_LOW_WATERMARK){
                throttle = FALSE;
            }
            if(throttle){
                Sleep(10);
                continue;
            }
            chunk_bytes_to_send = fread(chunk_buffer, 1, FILE_CHUNK_SIZE, file);
            if (chunk_bytes_to_send == 0 && ferror(file)) {
                fprintf(stdout, "Error reading file\n");
                remaining_bytes_to_send = 0;
                continue;
            }           

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
                                                                client.session_id, 
                                                                file_id, 
                                                                frame_fragment_offset, 
                                                                buffer, 
                                                                frame_fragment_size, 
                                                                client.socket, &client.server_addr
                                                            );
                if(fragment_bytes_sent == RET_VAL_ERROR){
                    Sleep(250);
                    continue;
                }

                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                remaining_bytes_to_send -= frame_fragment_size;
                client.file_bytes_to_send[index] = remaining_bytes_to_send;
            }     
        }                  
        fprintf(stdout, "Sent bytes: %llu\n", file_size);
        fclose(file);
        ResetEvent(file_transfer_event[index]);
    }
    _endthreadex(0);
    return 0;               
}
// --- Send message thread function ---
unsigned int WINAPI message_send_thread_func(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;
    
    char *file_path = "E:\\test_file.txt";
    FILE *file = NULL;
    char *message_buffer = NULL;

    uint32_t message_id;
    uint32_t message_len;
    uint32_t remaining_bytes_to_send;

    uint32_t frame_fragment_offset;
    uint32_t frame_fragment_len;

    BOOL throttle = FALSE;

    while(client.client_status == CLIENT_READY){
        
        WaitForSingleObject(message_send_event[index], INFINITE);
        
        message_len = (uint32_t)get_file_size(file_path);
        if(message_len == 0){
            fprintf(stdout, "Message file is empty!\n");
            continue;
        }
        if(message_len > MAX_MESSAGE_SIZE){
            fprintf(stdout, "Message file is too large! Max size: %d\n", MAX_MESSAGE_SIZE);
            continue;
        }
        fprintf(stdout, "Message file size: %d\n", message_len);
        client.message_len[index] = message_len;
        if(message_len == RET_VAL_ERROR){
            fprintf(stdout, "Error getting text file size!\n");
            continue;
        }
        file = fopen(file_path, "rb");
        if(file == NULL){
            fprintf(stdout, "Error opening file!\n");
            continue;
        }

        message_id = (uint32_t)InterlockedIncrement(&client.uid);
        frame_fragment_offset = 0;

        message_buffer = malloc(message_len + 1);
        if(message_buffer == NULL){
            fprintf(stdout, "Error allocating memeory buffer for message!\n");
            continue;
        }

        remaining_bytes_to_send = fread(message_buffer, 1, message_len, file);
        message_buffer[message_len] = '\0';
        if (remaining_bytes_to_send == 0 && ferror(file)) {
            fprintf(stdout, "Error reading message file!\n");
            continue;
        }

        while(remaining_bytes_to_send > 0){ 
            if(remaining_bytes_to_send > TEXT_FRAGMENT_SIZE){
                frame_fragment_len = TEXT_FRAGMENT_SIZE;
            } else {
                frame_fragment_len = remaining_bytes_to_send;
            }

            if(client.hash_count > HASH_FRAME_HIGH_WATERMARK){
                throttle = TRUE;
            }
            if(client.hash_count < HASH_FRAME_LOW_WATERMARK){
                throttle = FALSE;
            }
            if(throttle){
                Sleep(100);
                continue;
            }

            char buffer[TEXT_FRAGMENT_SIZE];

            const char *offset = message_buffer + frame_fragment_offset;
            memcpy(buffer, offset, frame_fragment_len);
            if(frame_fragment_len < TEXT_FRAGMENT_SIZE){
                buffer[frame_fragment_len] = '\0';
            }

            int fragment_bytes_sent = send_long_text_fragment(get_new_seq_num(), 
                                                                client.session_id, 
                                                                message_id, 
                                                                message_len, 
                                                                frame_fragment_offset, 
                                                                buffer, 
                                                                frame_fragment_len, 
                                                                client.socket, &client.server_addr
                                                            );
            if(fragment_bytes_sent == RET_VAL_ERROR){
                Sleep(100);
                continue;
            }
            frame_fragment_offset += frame_fragment_len;                       
            remaining_bytes_to_send -= frame_fragment_len;
            client.message_bytes_to_send[index] = remaining_bytes_to_send;
        }
        free(message_buffer);
        message_buffer = NULL;
        fclose(file);
        fprintf(stdout, "Sent message bytes: %d\n", message_len);
        ResetEvent(message_send_event[index]);
    }
   _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0; 
}

// --- Process command ---
unsigned int WINAPI command_thread_func(LPVOID lpParam) {

    char cmd;
     
    while(client.client_status == CLIENT_READY){

            fprintf(stdout,"Waiting for command...\n");

            cmd = getchar();
            switch(cmd) {
                //--------------------------------------------------------------------------------------------------------------------------
                case 'c':
                case 'C':
                    // if(client.session_status == SESSION_CONNECTED){
                    //     fprintf(stdout, "Already connected to server...\n");
                    //     break;
                    // }
                    send_connect_request(get_new_seq_num(), client.session_id, client.client_id, client.flags, client.client_name, client.socket, &client.server_addr);
                    client.session_status = SESSION_DISCONNECTED;
                    if (recieve_frame_thread) {
                        WaitForSingleObject(recieve_frame_thread, INFINITE);
                        CloseHandle(recieve_frame_thread);
                    }
                    client.session_status = SESSION_CONNECTING;
                    printf("Attempting to connect to server...\n");
                    Sleep(100);
                    recieve_frame_thread = (HANDLE)_beginthreadex(NULL, 0, receive_frame_thread_func, NULL, 0, NULL);
                    if (recieve_frame_thread == NULL) {
                        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
                        client.session_status = SESSION_DISCONNECTED;
                        client.client_status = CLIENT_STOP; // Signal immediate shutdown
                    }
                    WaitForSingleObject(connection_successfull, 2500);
                    ResetEvent(connection_successfull);
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Connection to server failed...\n");
                        client.session_status = SESSION_DISCONNECTED;
                    } else {
                        fprintf(stdout, "Connection to server success...\n");
                    }                    
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'd':
                case 'D':
                    send_disconnect(client.session_id, client.socket, &client.server_addr);
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Disconnecting from server...\n");
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'q':
                case 'Q':
                    client.client_status = CLIENT_STOP;
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Shutting down...\n");
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'f':
                case 'F':                   
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Not connected to server\n");
                        break;
                    }
                    int index;
                    for(index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
                        DWORD result = WaitForSingleObject(file_transfer_event[index], 0);
                        if (result == WAIT_OBJECT_0) {
                            // Event is signaled
                            continue;
                        } else if (result == WAIT_TIMEOUT) {
                            // Event is not signaled
                            SetEvent(file_transfer_event[index]);
                            break;
                        }
                    }
                    if(index == MAX_CLIENT_FILE_STREAMS){
                        fprintf(stderr, "Max threads reached!\n");
                    }
                
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 't':
                case 'T':
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Not connected to server\n");
                        break;
                    }
                    int msg_stream_index;
                    for(msg_stream_index = 0; msg_stream_index < MAX_CLIENT_MESSAGE_STREAMS; msg_stream_index++){
                        DWORD result = WaitForSingleObject(message_send_event[msg_stream_index], 0);
                        if (result == WAIT_OBJECT_0) {
                            // Event is signaled
                            continue;
                        } else if (result == WAIT_TIMEOUT) {
                            // Event is not signaled
                            SetEvent(message_send_event[msg_stream_index]);
                            fprintf(stdout, "Message thread %d started...\n", msg_stream_index);
                            break;
                        }
                    }
                    if(msg_stream_index == MAX_CLIENT_MESSAGE_STREAMS){
                        fprintf(stderr, "Max message threads reached!\n");
                    }
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
    fprintf(stdout, "Send command exiting...\n");
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    
    return 0;
}













