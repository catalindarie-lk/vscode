// --- udp_client.c ---
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

#include "include/client.h"
#include "include/protocol_frames.h"    // For protocol frame definitions
#include "include/netendians.h"         // For network byte order conversions
#include "include/checksum.h"           // For checksum validation
#include "include/mem_pool.h"           // For memory pool management
#include "include/fileio.h"             // For file transfer functions
#include "include/queue.h"              // For queue management
#include "include/bitmap.h"             // For bitmap management
#include "include/hash.h"               // For hash table management
#include "include/sha256.h"

ClientData client;
ClientIOManager io_manager;

HANDLE hthread_recieve_frame;
HANDLE hthread_process_frame;
HANDLE hthread_resend_frame;
HANDLE hthread_keep_alive;
HANDLE hthread_client_command;


HANDLE hthread_file_transfer[MAX_CLIENT_FILE_STREAMS];
HANDLE hthread_message_send[MAX_CLIENT_MESSAGE_STREAMS];

HANDLE hevent_start_file_transfer[MAX_CLIENT_FILE_STREAMS];
HANDLE hevent_stop_file_transfer[MAX_CLIENT_FILE_STREAMS];
HANDLE hevent_file_metadata[MAX_CLIENT_FILE_STREAMS];
HANDLE hevent_start_message_send[MAX_CLIENT_MESSAGE_STREAMS];
HANDLE hevent_stop_message_send[MAX_CLIENT_MESSAGE_STREAMS];

HANDLE hevent_connection_successfull;

const char *server_ip = "127.0.0.1"; // loopback address
const char *client_ip = "127.0.0.1";


DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam);
DWORD WINAPI thread_proc_resend_frame(LPVOID lpParam);
DWORD WINAPI thread_proc_file_transfer(LPVOID lpParam);
DWORD WINAPI thread_proc_message_send(LPVOID lpParam);
DWORD WINAPI thread_proc_client_command(LPVOID lpParam);


int send_file_metadata(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *file_name,
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager
                        );

int send_file_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager
                        );

int send_long_text_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t message_id, 
                            const uint32_t message_len, 
                            const uint32_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_len, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr, 
                            ClientIOManager* io_manager
                        );



static int init_client_session(){

    memset(&client, 0, sizeof(ClientData));
    
    client.client_status = CLIENT_STATUS_BUSY;
    client.session_status = SESSION_DISCONNECTED; // Initial status is disconnected

    client.client_id = CLIENT_ID;
    
    client.flags = 0; // No special flags set
    snprintf(client.client_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, CLIENT_NAME);
    client.last_active_time = time(NULL); // Set current time as last active time

    client.frame_count = (uint64_t)UINT32_MAX;
    client.uid_count = 0;

    client.session_id = 0; // Session ID will be assigned by the server
    client.server_status = SERVER_STATUS_NONE; // Initial server status is ready
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC; // Default session timeout
    // Initialize client data
    memset(client.server_name, 0, NAME_SIZE);
    
    return RET_VAL_SUCCESS;     
}
static int reset_client_session(){
    
    client.session_status = SESSION_DISCONNECTED; // Initial status is disconnected
    
    client.frame_count = (uint64_t)UINT32_MAX;
    client.uid_count = 0;

    client.session_id = 0; // Session ID will be assigned by the server
    client.server_status = SERVER_STATUS_NONE; // Initial server status is ready
    client.session_timeout = DEFAULT_SESSION_TIMEOUT_SEC; // Default session timeout
    // Initialize client data
    memset(client.server_name, 0, NAME_SIZE);
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

    client.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client.socket == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        closesocket(client.socket);
        WSACleanup();
        return RET_VAL_ERROR;
    }

    client.client_addr.sin_family = AF_INET;
    client.client_addr.sin_port = _htons(0); // Let OS choose port
    client.client_addr.sin_addr.s_addr = inet_addr(client_ip);

    if (bind(client.socket, (struct sockaddr *)&client.client_addr, sizeof(client.client_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        WSACleanup();
        return RET_VAL_ERROR;
    }
   
    // Define server address
    memset(&client.server_addr, 0, sizeof(client.server_addr));
    client.server_addr.sin_family = AF_INET;
    client.server_addr.sin_port = _htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &client.server_addr.sin_addr) <= 0){
        fprintf(stderr, "Invalid address or address not supported.\n");
        WSACleanup();
        return RET_VAL_ERROR;
    };

    return RET_VAL_SUCCESS;
}
static int init_client_buffers(){

    // Initialize queues
    io_manager.queue_frame.head = 0;
    io_manager.queue_frame.tail = 0;
    InitializeCriticalSection(&io_manager.queue_frame.mutex);
    io_manager.queue_priority_frame.head = 0;
    io_manager.queue_priority_frame.tail = 0;
    InitializeCriticalSection(&io_manager.queue_priority_frame.mutex);

    // Initialize frame hash table
    for(int i = 0; i < HASH_SIZE_FRAME; i++){
        io_manager.ht_frame.entry[i] = NULL;
    }
    InitializeCriticalSection(&io_manager.ht_frame.mutex); 
    io_manager.ht_frame.count = 0;

    // Initialize memory pool for frames
    io_manager.ht_frame.pool.block_size = BLOCK_SIZE_FRAME;
    io_manager.ht_frame.pool.block_count = BLOCK_COUNT_FRAME;
    pool_init(&io_manager.ht_frame.pool);

    return RET_VAL_SUCCESS;
}
static int create_client_events(){

    hevent_connection_successfull = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (hevent_connection_successfull == NULL) {
        fprintf(stdout, "CreateEvent failed (%lu)\n", GetLastError());
        return RET_VAL_ERROR;
    }

    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        hevent_start_file_transfer[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        hevent_stop_file_transfer[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        hevent_file_metadata[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (hevent_start_file_transfer[index] == NULL || hevent_stop_file_transfer[index] == NULL || hevent_file_metadata[index] == NULL) {
            fprintf(stderr, "Failed to create file transfer events. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
            return RET_VAL_ERROR;
        }
    }
    for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        hevent_start_message_send[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        hevent_stop_message_send[index] = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (hevent_start_message_send[index] == NULL || hevent_stop_message_send[index] == NULL) {
            fprintf(stderr, "Failed to create message send events. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
            return RET_VAL_ERROR;
        }
    }
    
    client.client_status = CLIENT_STATUS_READY;
    return RET_VAL_SUCCESS;
}   
static void start_threads(){

    hthread_process_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_process_frame, NULL, 0, NULL);
    if (hthread_process_frame == NULL) {
        fprintf(stderr, "Failed to create process frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
    }
    hthread_resend_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_resend_frame, NULL, 0, NULL);
    if (hthread_resend_frame == NULL) {
        fprintf(stderr, "Failed to create resend frame thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
    }
    hthread_client_command = (HANDLE)_beginthreadex(NULL, 0, thread_proc_client_command, NULL, 0, NULL);
    if (hthread_client_command == NULL) {
        fprintf(stderr, "Failed to create command thread. Error: %d\n", GetLastError());
        client.session_status = SESSION_DISCONNECTED;
        client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
    }
    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        hthread_file_transfer[index] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_file_transfer, (LPVOID)(intptr_t)index, 0, NULL);
        if (hthread_file_transfer[index] == NULL){
            fprintf(stderr, "Failed to create file send thread. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
        }
    }
   for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        hthread_message_send[index] = (HANDLE)_beginthreadex(NULL, 0, thread_proc_message_send, (LPVOID)(intptr_t)index, 0, NULL);
        if (hthread_message_send[index] == NULL){
            fprintf(stderr, "Failed to create message send thread. Error: %d\n", GetLastError());
            client.session_status = SESSION_DISCONNECTED;
            client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
        }
    }
}
static void shutdown_client(){

    client.client_status = CLIENT_STATUS_NONE;

    if (hthread_recieve_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_recieve_frame, INFINITE);
        CloseHandle(hthread_recieve_frame);
    }
    fprintf(stdout,"receive frame thread closed...\n");
    if (hthread_process_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_process_frame, INFINITE);
        CloseHandle(hthread_process_frame);
    }
    fprintf(stdout,"process frame thread closed...\n");
    if (hthread_resend_frame) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_resend_frame, INFINITE);
        CloseHandle(hthread_resend_frame);
    }
    fprintf(stdout,"resend frame thread closed...\n");
    if (hthread_client_command) {
        // Signal the receive thread to stop and wait for it to finish
        WaitForSingleObject(hthread_client_command, INFINITE);
        CloseHandle(hthread_client_command);
    }   
    fprintf(stdout,"command thread closed...\n");


    for(int index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
        if (hthread_file_transfer[index]) {
            // Signal the receive thread to stop and wait for it to finish
            WaitForSingleObject(hthread_file_transfer[index], INFINITE);
            CloseHandle(hthread_file_transfer[index]);
        }
        if (hevent_start_file_transfer[index]) {
            CloseHandle(hevent_start_file_transfer[index]);
        }
        if (hevent_stop_file_transfer[index]) {
            CloseHandle(hevent_start_file_transfer[index]);
        }
        if (hevent_file_metadata[index]) {
            CloseHandle(hevent_file_metadata[index]);
        }
    }
    fprintf(stdout,"file transfer threads closed...\n");


    for(int index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
        if (hthread_message_send[index]) {
            // Signal the receive thread to stop and wait for it to finish
            WaitForSingleObject(hthread_message_send[index], INFINITE);
            CloseHandle(hthread_message_send[index]);
        }
        if (hevent_start_message_send[index]) {
            CloseHandle(hevent_start_message_send[index]);
        }
        if (hevent_stop_message_send[index]) {
            CloseHandle(hevent_start_message_send[index]);
        }
    }
    fprintf(stdout,"message send threads closed...\n");



    DeleteCriticalSection(&io_manager.queue_frame.mutex);
    DeleteCriticalSection(&io_manager.queue_priority_frame.mutex);
    CloseHandle(hevent_connection_successfull);
    CloseHandle(hevent_start_message_send);
    closesocket(client.socket);
    WSACleanup();
}


// --- Receive frame ---
DWORD WINAPI thread_proc_receive_frame(LPVOID lpParam) {

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
                target_queue = &io_manager.queue_priority_frame;
            } else {
                target_queue = &io_manager.queue_frame;
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
DWORD WINAPI thread_proc_process_frame(LPVOID lpParam) {

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

    while(client.client_status == CLIENT_STATUS_READY){
        // Pop a frame from the queue (prioritize control queue)
        if (pop_frame(&io_manager.queue_priority_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // fprintf(stdout, "Processing control frame type: %d, opcode: %d, seq num: %llu\n", frame_entry.frame.header.frame_type, frame_entry.frame.payload.ack.op_code, _ntohll(frame_entry.frame.header.seq_num));
            // Successfully popped from queue_frame_ctrl
        } else if (pop_frame(&io_manager.queue_frame, &frame_entry) == RET_VAL_SUCCESS) {
            // fprintf(stdout, "Processing frame type: %d, opcode: %d, seq num: %llu\n", frame_entry.frame.header.frame_type, frame_entry.frame.payload.ack.op_code, _ntohll(frame_entry.frame.header.seq_num));
            // Successfully popped from queue_frame
        } else {
            Sleep(100); // No frames to process, yield CPU
            continue;
        }     

        frame = &frame_entry.frame;
        src_addr = &frame_entry.src_addr;
        recvfrom_bytes_received = frame_entry.frame_size;

        // Extract header fields   
        received_delimiter = _ntohs(frame->header.start_delimiter);
        received_frame_type = frame->header.frame_type;
        received_seq_num = _ntohll(frame->header.seq_num);
        received_session_id = _ntohl(frame->header.session_id);

        inet_ntop(AF_INET, &(src_addr->sin_addr), src_ip, INET_ADDRSTRLEN);
        src_port = _ntohs(src_addr->sin_port);
       
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
                received_session_timeout = _ntohl(frame->payload.response.session_timeout);               
                if(received_session_id == 0 || received_server_status != SERVER_STATUS_READY){
                    fprintf(stderr, "Session ID invalid or server not ready. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                if(received_session_timeout <= 10){
                    fprintf(stderr, "Session timeout invalid. Connection not established!\n");
                    client.session_status = SESSION_DISCONNECTED;
                    break;
                }
                fprintf(stdout, "Received connect response from %s:%d with session ID: %d, timeout: %d seconds, server status: %d\n", 
                        src_ip, src_port, received_session_id, received_session_timeout, received_server_status);
                client.server_status = received_server_status;
                client.session_timeout = received_session_timeout;
                client.session_id = received_session_id;
                snprintf(client.server_name, NAME_SIZE, "%.*s", NAME_SIZE - 1, frame->payload.response.server_name);
                client.last_active_time = time(NULL);
                client.session_status = SESSION_CONNECTED;

                SetEvent(hevent_connection_successfull);
                
                hthread_keep_alive = (HANDLE)_beginthreadex(NULL, 0, thread_proc_keep_alive, NULL, 0, NULL);
                if (hthread_keep_alive == NULL) {
                    fprintf(stderr, "Failed to create keep alive thread. Error: %d\n", GetLastError());
                    client.session_status = SESSION_DISCONNECTED;
                    client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
                }
                break; 

            case FRAME_TYPE_ACK:
                if(received_session_id != client.session_id){
                    fprintf(stderr, "Received ACK frame with invalid session ID: %d", received_session_id);
                    //TODO - send ACK frame with error code for invalid session ID
                    break;
                }
                client.last_active_time = time(NULL);
                uint8_t op_code = frame->payload.ack.op_code;
                for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
                    if(received_seq_num == client.pending_metadata_seq_num[i] && (op_code == STS_ACK || op_code == ERR_DUPLICATE_FRAME)){
                        client.pending_metadata_seq_num[i] = UINT64_MAX; // Reset pending metadata sequence number  
                        SetEvent(hevent_file_metadata[i]);
                    }
                }
                
                if(op_code == STS_KEEP_ALIVE){
                    fprintf(stdout, "Received keep alive ACK for seq num: %llu\n", received_seq_num);
                }

                if(op_code == STS_ACK || op_code == STS_KEEP_ALIVE || op_code == ERR_DUPLICATE_FRAME || op_code == STS_TRANSFER_COMPLETE){
                    //remove_frame(io_manager.frame_ht, &io_manager.frame_ht_mutex, received_seq_num, &io_manager.frame_ht_count, &io_manager.frame_mem_pool);
                    ht_remove_frame(&io_manager.ht_frame, received_seq_num);
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
    }
    return 0; // Properly exit the thread created by _beginthreadex
}
// --- Send keep alive ---
DWORD WINAPI thread_proc_keep_alive(LPVOID lpParam){

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
DWORD WINAPI thread_proc_resend_frame(LPVOID lpParam){
   
    while(client.client_status == CLIENT_STATUS_READY){
        if(client.session_status != SESSION_CONNECTED){
            //clean_frame_hash_table(io_manager.frame_ht, &io_manager.frame_ht_mutex, &io_manager.frame_ht_count, &io_manager.frame_mem_pool);
            ht_clean(&io_manager.ht_frame);
            
            Sleep(1000);
            continue;
        }
        time_t current_time = time(NULL);
        EnterCriticalSection(&io_manager.ht_frame.mutex);
        for (int i = 0; i < HASH_SIZE_FRAME; i++) {
            FramePendingAck *ptr = io_manager.ht_frame.entry[i];
            while (ptr) {
                if(current_time - ptr->time > (time_t)RESEND_TIMEOUT){
                    send_frame(&ptr->frame, client.socket, &client.server_addr);
                    ptr->time = current_time;
                }
                ptr = ptr->next;
            }                                         
        }
        LeaveCriticalSection(&io_manager.ht_frame.mutex);
        //fprintf(stdout, "Bytes to send: %d\n", client.total_bytes_to_send);
        uint64_t sleep_time = RESEND_TIME_IDLE;
        for(int i = 0; i < MAX_CLIENT_FILE_STREAMS; i++){
            if(client.file_bytes_to_send[i] > 0){
                sleep_time = RESEND_TIME_TRANSFER;
            }
            Sleep(sleep_time);
        }
        Sleep(100);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0;
}
// --- File transfer thread function ---
DWORD WINAPI thread_proc_file_transfer(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;

    char *file_path = "D:\\E\\test_file.txt";
    FILE *file = NULL;
    long long file_size = 0;
    uint8_t chunk_buffer[FILE_CHUNK_SIZE] = {0};
    uint64_t remaining_bytes_to_send = 0;
    uint32_t chunk_bytes_to_send = 0;
    uint32_t chunk_fragment_offset = 0;
    uint32_t frame_fragment_size = 0;
    uint64_t frame_fragment_offset = 0;
    uint32_t file_id = 0;
    BOOL throttle = 0;

    SHA256_CTX sha256_ctx;

    while(client.client_status == CLIENT_STATUS_READY){
        
        WaitForSingleObject(hevent_start_file_transfer[index], INFINITE);
        
        sha256_init(&sha256_ctx);
        file = NULL;
        
        file_size = get_file_size(file_path);       
        if(file_size == RET_VAL_ERROR){
            client.file_size[index] = 0;
            goto clean;
        }

        file = fopen(file_path, "rb");
        if(file == NULL){
            fprintf(stdout, "Error opening file!!!\n");
            goto clean;
        }
 
        client.file_size[index] = file_size;

        file_id = InterlockedIncrement(&client.uid_count);

        client.pending_metadata_seq_num[index] = get_new_seq_num();
        int metadata_bytes_sent = send_file_metadata(client.pending_metadata_seq_num[index], 
                                                        client.session_id, 
                                                        file_id, 
                                                        file_size, 
                                                        file_path, 
                                                        FILE_FRAGMENT_SIZE, 
                                                        client.socket, 
                                                        &client.server_addr,
                                                        &io_manager
                                                    );
        if(metadata_bytes_sent == RET_VAL_ERROR){
            fprintf(stderr, "Failed to send file metadata frame. Cancelling transfer\n");
            goto clean;
            continue;
        }

        WaitForSingleObject(hevent_file_metadata[index], INFINITE);
        // DWORD result = WaitForSingleObject(hevent_file_metadata[index], TIMEOUT_METADATA_RESPONSE_MS);
        // if (result == WAIT_OBJECT_0) {
        // // The event was signaled within the timeout —> proceed sending the rest of the file fragments
        // } else if (result == WAIT_TIMEOUT) {
        //     fprintf(stderr, "Timeout error waiting for metadata response\n");
        //     goto clean;
        // } else {
        //     // Unexpected error — maybe invalid handle
        //     fprintf(stderr, "Unexpected error when waiting for metadata response\n");
        //     goto clean;
        // }


        frame_fragment_offset = 0;
        remaining_bytes_to_send = file_size;

        while(remaining_bytes_to_send > 0){
            // handle stop signal received during file transfer
            DWORD result = WaitForSingleObject(hevent_stop_file_transfer[index], 0);
            if (result == WAIT_OBJECT_0) {
                fprintf(stderr, "File transfer force stopped\n");
                goto clean;
            }

            if(io_manager.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                throttle = TRUE;
            }
            if(io_manager.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                throttle = FALSE;
            }
            if(throttle){
                Sleep(10);
                continue;
            }

            chunk_bytes_to_send = fread(chunk_buffer, 1, FILE_CHUNK_SIZE, file);
            if (chunk_bytes_to_send == 0 && ferror(file)) {
                fprintf(stderr, "Error reading file\n");
                remaining_bytes_to_send = 0;
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
                                                                client.session_id, 
                                                                file_id, 
                                                                frame_fragment_offset, 
                                                                buffer, 
                                                                frame_fragment_size, 
                                                                client.socket, &client.server_addr,
                                                                &io_manager
                                                            );
                if(fragment_bytes_sent == RET_VAL_ERROR){
                    Sleep(100);
                    continue;
                }

                chunk_fragment_offset += frame_fragment_size;
                frame_fragment_offset += frame_fragment_size;                       
                chunk_bytes_to_send -= frame_fragment_size;
                remaining_bytes_to_send -= frame_fragment_size;
                client.file_bytes_to_send[index] = remaining_bytes_to_send;

            }     
        }                  

    clean:  //clean thread data
        sha256_final(&sha256_ctx, (uint8_t *)&client.file_hash[index].sha256);
        send_file_end(get_new_seq_num(), 
                        client.session_id, 
                        file_id, 
                        file_size, 
                        (uint8_t *)&client.file_hash[index].sha256,
                        client.socket, 
                        &client.server_addr,
                        &io_manager
                    );

        // fprintf(stdout, "File hash: ");
        // for(int i = 0; i < 32; i++){
        //     fprintf(stdout, "%02x", (unsigned char)client.file_hash[index].sha256[i]);
        // }
        // fprintf(stdout, "\n");

        memset((uint8_t *)&client.file_hash[index].sha256, 0, 32);
        if(file != NULL){
            fclose(file);
            file = NULL;
        }
        file_size = 0;
        client.file_size[index] = 0;
        client.file_bytes_to_send[index] = 0;       
        
        for(int i = 0; i < FILE_CHUNK_SIZE; i++){
            chunk_buffer[i] = 0;
        }
        remaining_bytes_to_send = 0;
        chunk_bytes_to_send = 0;
        chunk_fragment_offset = 0;
        frame_fragment_size = 0;
        frame_fragment_offset = 0;
        file_id = 0;
        throttle = FALSE;
        ResetEvent(hevent_file_metadata[index]);
        ResetEvent(hevent_stop_file_transfer[index]);
        ResetEvent(hevent_start_file_transfer[index]);       
    }
    _endthreadex(0);
    return 0;               
}
// --- Send message thread function ---
DWORD WINAPI thread_proc_message_send(LPVOID lpParam){

    int index = (int)(intptr_t)lpParam;

    char *file_path = "D:\\E\\test_file.txt";
    FILE *file = NULL;
    char *message_buffer = NULL;
    uint32_t message_id = 0;
    uint32_t message_len = 0;
    uint32_t remaining_bytes_to_send = 0;
    uint32_t frame_fragment_offset = 0;
    uint32_t frame_fragment_len = 0;
    BOOL throttle = FALSE;

    long long text_file_size;

    while(client.client_status == CLIENT_STATUS_READY){
        
        WaitForSingleObject(hevent_start_message_send[index], INFINITE);
        
        file = NULL;
        message_buffer = NULL;
        
        text_file_size = get_file_size(file_path);  
      
        if(text_file_size == RET_VAL_ERROR){
            goto clean;
        }
        if(text_file_size > MAX_MESSAGE_SIZE){
            fprintf(stdout, "Message file is too large! Message Size: %llu > Max size: %u\n", text_file_size, MAX_MESSAGE_SIZE);
            goto clean;
        }
        message_len = (uint32_t)text_file_size;
        client.message_len[index] = message_len;

        file = fopen(file_path, "rb");
        if(file == NULL){
            fprintf(stdout, "Error opening file!\n");
            goto clean;
        }

        message_id = InterlockedIncrement(&client.uid_count);
        frame_fragment_offset = 0;

        message_buffer = malloc(message_len + 1);
        if(message_buffer == NULL){
            fprintf(stdout, "Error allocating memeory buffer for message!\n");
            goto clean;
        }

        remaining_bytes_to_send = fread(message_buffer, 1, message_len, file);
        message_buffer[message_len] = '\0';
        if (remaining_bytes_to_send == 0 && ferror(file)) {
            fprintf(stdout, "Error reading message file!\n");
            goto clean;
        }

        while(remaining_bytes_to_send > 0){

            DWORD result = WaitForSingleObject(hevent_stop_message_send[index], 0);
            if (result == WAIT_OBJECT_0) {
                fprintf(stderr, "Message send force stopped\n");
                goto clean;
            }

            if(remaining_bytes_to_send > TEXT_FRAGMENT_SIZE){
                frame_fragment_len = TEXT_FRAGMENT_SIZE;
            } else {
                frame_fragment_len = remaining_bytes_to_send;
            }

            if(io_manager.ht_frame.count > HASH_FRAME_HIGH_WATERMARK){
                throttle = TRUE;
            }
            if(io_manager.ht_frame.count < HASH_FRAME_LOW_WATERMARK){
                throttle = FALSE;
            }
            if(throttle){
                Sleep(10);
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
                                                                client.socket, &client.server_addr,
                                                                &io_manager
                                                            );
            if(fragment_bytes_sent == RET_VAL_ERROR){
                Sleep(100);
                continue;
            }
            frame_fragment_offset += frame_fragment_len;                       
            remaining_bytes_to_send -= frame_fragment_len;
            client.message_bytes_to_send[index] = remaining_bytes_to_send;
        }

    clean:

        if(message_buffer != NULL){
            free(message_buffer);
            message_buffer = NULL;
        }
        if(file != NULL){
            fclose(file);
            file = NULL;
        }
        text_file_size = 0;
        client.message_len[index] = 0;
        client.message_bytes_to_send[index] = 0;
        message_id = 0;
        message_len = 0;
        remaining_bytes_to_send = 0;
        frame_fragment_offset = 0;
        frame_fragment_len = 0;
        throttle = FALSE;
        ResetEvent(hevent_stop_message_send[index]);
        ResetEvent(hevent_start_message_send[index]);
    }
    _endthreadex(0); // Properly exit the thread created by _beginthreadex
    return 0; 
}
// --- Process command ---
DWORD WINAPI thread_proc_client_command(LPVOID lpParam) {

    char cmd;
     
    while(client.client_status == CLIENT_STATUS_READY){

            fprintf(stdout,"Waiting for command...\n");

            cmd = getchar();
            switch(cmd) {
                //--------------------------------------------------------------------------------------------------------------------------
                case 'c':
                case 'C':
                    send_connect_request(get_new_seq_num(), 
                                            client.session_id, 
                                            client.client_id, 
                                            client.flags, 
                                            client.client_name, 
                                            client.socket, 
                                            &client.server_addr
                                        );
                    client.session_status = SESSION_DISCONNECTED;
                    if (hthread_recieve_frame) {
                        WaitForSingleObject(hthread_recieve_frame, INFINITE);
                        CloseHandle(hthread_recieve_frame);
                    }
                    client.session_status = SESSION_CONNECTING;
                    printf("Attempting to connect to server...\n");
                    Sleep(100);
                    hthread_recieve_frame = (HANDLE)_beginthreadex(NULL, 0, thread_proc_receive_frame, NULL, 0, NULL);
                    if (hthread_recieve_frame == NULL) {
                        fprintf(stderr, "Failed to create receive frame thread. Error: %d\n", GetLastError());
                        client.session_status = SESSION_DISCONNECTED;
                        client.client_status = CLIENT_STATUS_NONE; // Signal immediate shutdown
                    }
                    WaitForSingleObject(hevent_connection_successfull, CONNECTION_SUCCESFULL_TIMEOUT_MS);
                    ResetEvent(hevent_connection_successfull);
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Connection to server failed...\n");
                        client.session_status = SESSION_DISCONNECTED;
                    } else {
                        fprintf(stdout, "Connection to server success...\n");
                    }                    
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'd':
                case 'D': //disconnect
                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Not connected to server\n");
                        break;
                    }
                    int index = 0;
                    for(index = 0; index < MAX_CLIENT_FILE_STREAMS; index++){
                        DWORD result = WaitForSingleObject(hevent_start_file_transfer[index], 0);
                        //WAIT_OBJECT_0 - event is signaled
                        //WAIT_TIMEOUT - event is not signaled
                        if(result == WAIT_OBJECT_0){
                            SetEvent(hevent_stop_file_transfer[index]);
                        }
                    }
                    for(index = 0; index < MAX_CLIENT_MESSAGE_STREAMS; index++){
                        DWORD result = WaitForSingleObject(hevent_start_message_send[index], 0);
                        if(result == WAIT_OBJECT_0){
                            SetEvent(hevent_stop_message_send[index]);
                        }
                    }
                    index = 0;
                    int retry_count = 0;
                    while (index < MAX_CLIENT_FILE_STREAMS && retry_count < MAX_RETRIES_STOP_TRANSFER) {
                        DWORD result = WaitForSingleObject(hevent_stop_file_transfer[index], 0);
                        if (result == WAIT_TIMEOUT) {
                            index++;
                            retry_count = 0; // reset for next index
                        } else {
                            Sleep(10);
                            retry_count++;
                        }
                    }
                    if(retry_count >= MAX_RETRIES_STOP_TRANSFER){
                        fprintf(stderr, "ERROR: Retry stop transfer, disconnect anyway! This should not happen!\n");
                    }
                    index = 0;
                    retry_count = 0;
                    while (index < MAX_CLIENT_MESSAGE_STREAMS && retry_count < MAX_RETRIES_STOP_TRANSFER) {
                        DWORD result = WaitForSingleObject(hevent_stop_message_send[index], 0);
                        if (result == WAIT_TIMEOUT) {
                            index++;
                            retry_count = 0; // reset for next index
                        } else {
                            Sleep(10);
                            retry_count++;
                        }
                    }
                    if(retry_count >= MAX_RETRIES_STOP_TRANSFER){
                        fprintf(stderr, "ERROR: Retry stop message send, disconnect anyway! This should not happen!\n");
                    }

                    if(client.session_status != SESSION_CONNECTED){
                        fprintf(stdout, "Not connected to server\n");
                        break;
                    }

                    //clean_frame_hash_table(io_manager.frame_ht, &io_manager.frame_ht_mutex, &io_manager.frame_ht_count, &io_manager.frame_mem_pool);
                    ht_clean(&io_manager.ht_frame);
                    send_disconnect(client.session_id, client.socket, &client.server_addr);
                    client.session_status = SESSION_DISCONNECTED;
                    printf("Disconnecting from server...\n");
                    break;
                //--------------------------------------------------------------------------------------------------------------------------
                case 'q':
                case 'Q':
                    client.client_status = CLIENT_STATUS_NONE;
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
                    int index_file_stream;
                    for(index_file_stream = 0; index_file_stream < MAX_CLIENT_FILE_STREAMS; index_file_stream++){
                        DWORD result = WaitForSingleObject(hevent_start_file_transfer[index_file_stream], 0);
                        if (result == WAIT_OBJECT_0) {
                            // Event is signaled
                            continue;
                        } else if (result == WAIT_TIMEOUT) {
                            // Event is not signaled
                            SetEvent(hevent_start_file_transfer[index_file_stream]);
                            break;
                        }
                    }
                    if(index_file_stream == MAX_CLIENT_FILE_STREAMS){
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
                        DWORD result = WaitForSingleObject(hevent_start_message_send[msg_stream_index], 0);
                        if (result == WAIT_OBJECT_0) {
                            // Event is signaled
                            continue;
                        } else if (result == WAIT_TIMEOUT) {
                            // Event is not signaled
                            SetEvent(hevent_start_message_send[msg_stream_index]);
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

// --- Get new sequence number ---
uint64_t get_new_seq_num(){
    return InterlockedIncrement64(&client.frame_count);
}
// --- Main function ---
int main() {

    init_client_session();
    init_client_config();
    init_client_buffers();
    create_client_events();
    start_threads();
    while(client.client_status == CLIENT_STATUS_READY){
        fprintf(stdout, "\r\033[2K-- File: %.2f perc, Text: %.2f perc, Hash Frames: %u, Free Pool Blocks: %llu, DIFF: %llu", 
                            (float)(client.file_size[0] - client.file_bytes_to_send[0]) / (float)client.file_size[0] * 100.0, 
                            (float)(client.message_len[0] - client.message_bytes_to_send[0]) / (float)client.message_len[0] * 100.0,
                            io_manager.ht_frame.count,
                            io_manager.ht_frame.pool.free_blocks,
                            io_manager.ht_frame.pool.free_blocks - (io_manager.ht_frame.pool.block_count - io_manager.ht_frame.count)
                            );
        fflush(stdout);

        if(client.session_status == SESSION_DISCONNECTED){
            reset_client_session();
        }     
        Sleep(100); // Simulate some delay between messages        
    }
    fprintf(stdout, "Client shutting down!!!\n");
    shutdown_client(&client);
    
    return 0;
}







int send_file_metadata(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *file_name,
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager
                        ){

    UdpFrame frame;
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));

    if(file_name == NULL){
        fprintf(stderr, "ERROR: Invalid file name pointer (NULL).\n");
        return RET_VAL_ERROR;
    }
    uint32_t file_name_len = (uint32_t)strnlen(file_name, NAME_SIZE - 1);
 
    if(file_name_len == 0){
        fprintf(stderr, "ERROR: File name is 0 length.\n");
        return RET_VAL_ERROR;
    }
    if(file_name_len >= NAME_SIZE - 1){
        fprintf(stderr, "ERROR: File name is too long. Max length is %d characters.\n", NAME_SIZE - 1);
        return RET_VAL_ERROR;
    }
    file_name_len += 1; // Add 1 for null terminator

    // FILE *file = fopen(file_name, "rb");
    // if(file == NULL){
    //     fprintf(stderr, "ERROR: Failed to open file %s for reading.\n", file_name);
    //     return RET_VAL_ERROR;
    // }
    // SHA256_CTX sha256_ctx;
    // sha256_init(&sha256_ctx);
    // char buffer[4096]; // Buffer for reading file chunks
    // size_t bytes_read;
    // while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    //     sha256_update(&sha256_ctx, (const uint8_t *)buffer, bytes_read);
    // }
    // fclose(file);

    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_METADATA;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);
    snprintf(frame.payload.file_metadata.filename, file_name_len, "%s", file_name);
    // sha256_final(&sha256_ctx, frame.payload.file_metadata.file_hash);
           
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileMetadataPayload)));
    
    if(ht_insert_frame(&io_manager->ht_frame, &frame) == RET_VAL_ERROR){
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


int send_file_end(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const char *file_hash,
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager
                        ){

    UdpFrame frame;
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));

    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_END;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_metadata.file_id = _htonl(file_id);
    frame.payload.file_metadata.file_size = _htonll(file_size);

    memcpy(frame.payload.file_end.file_hash, file_hash, 32);
           
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileEndPayload)));
    
    if(ht_insert_frame(&io_manager->ht_frame, &frame) == RET_VAL_ERROR){
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




int send_file_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientIOManager* io_manager
                        ){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "\nInvalid text!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_FILE_FRAGMENT;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.file_fragment.file_id = _htonl(file_id);
    frame.payload.file_fragment.size = _htonl(fragment_size);
    frame.payload.file_fragment.offset = _htonll(fragment_offset);
    memcpy(frame.payload.file_fragment.bytes, fragment_buffer, fragment_size);
    
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(FileFragmentPayload)));  

    if(ht_insert_frame(&io_manager->ht_frame, &frame) == RET_VAL_ERROR){
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

int send_long_text_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t message_id, 
                            const uint32_t message_len, 
                            const uint32_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_len, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr, 
                            ClientIOManager* io_manager
                        ){

    UdpFrame frame;
    if(fragment_buffer == NULL){
        fprintf(stderr, "Invalid text pointer parsed!.\n");
        return SOCKET_ERROR;
    }
    // Initialize the text message frame
    memset(&frame, 0, sizeof(UdpFrame));
    // Set the header fields
    frame.header.start_delimiter = _htons(FRAME_DELIMITER);
    frame.header.frame_type = FRAME_TYPE_LONG_TEXT_MESSAGE;
    frame.header.seq_num = _htonll(seq_num);
    frame.header.session_id = _htonl(session_id);
    // Set the payload fields
    frame.payload.long_text_msg.message_id = _htonl(message_id);
    frame.payload.long_text_msg.message_len = _htonl(message_len);
    frame.payload.long_text_msg.fragment_len = _htonl(fragment_len);
    frame.payload.long_text_msg.fragment_offset = _htonl(fragment_offset);
    
    memcpy(frame.payload.long_text_msg.fragment_text, fragment_buffer, fragment_len);
    
    // Calculate the checksum for the frame
    frame.header.checksum = _htonl(calculate_crc32(&frame, sizeof(FrameHeader) + sizeof(LongTextPayload)));  

    if(ht_insert_frame(&io_manager->ht_frame, &frame) == RET_VAL_ERROR){
        fprintf(stderr, "Mem Pool is full, failed to allocate!\n");
        return RET_VAL_ERROR;
    }

    int bytes_sent = send_frame(&frame, src_socket, dest_addr);
    if(bytes_sent == SOCKET_ERROR){
        fprintf(stderr, "send_text_message() failed\n");
        return SOCKET_ERROR;
    }
    return bytes_sent;
}
