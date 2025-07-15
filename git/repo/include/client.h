#ifndef CLIENT_H
#define CLIENT_H


#include <stdint.h>
#include <windows.h>
#include "include/protocol_frames.h"
#include "include/queue.h"
#include "include/mem_pool.h"
#include "include/hash.h"
#include "sha256.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif
#ifndef MAX_CLIENT_MESSAGE_STREAMS
#define MAX_CLIENT_MESSAGE_STREAMS      10
#endif
#ifndef MAX_CLIENT_FILE_STREAMS
#define MAX_CLIENT_FILE_STREAMS         10
#endif


#ifndef SERVER_STATUS_NONE
#define SERVER_STATUS_NONE              0
#endif
#ifndef SERVER_STATUS_BUSY
#define SERVER_STATUS_BUSY              1
#endif
#ifndef SERVER_STATUS_READY
#define SERVER_STATUS_READY             2
#endif
#ifndef SERVER_STATUS_ERROR
#define SERVER_STATUS_ERROR             3
#endif

#ifndef CLIENT_STATUS_NONE
#define CLIENT_STATUS_NONE              0
#endif
#ifndef CLIENT_STATUS_BUSY
#define CLIENT_STATUS_BUSY              1
#endif
#ifndef CLIENT_STATUS_READY
#define CLIENT_STATUS_READY             2
#endif
#ifndef CLIENT_STATUS_ERROR
#define CLIENT_STATUS_ERROR             3
#endif


#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC     120
#endif


// --- Constants ---
#define SERVER_PORT                     12345       // Port the server listens on
#define FRAME_DELIMITER                 0xAABB      // A magic number to identify valid frames
#define RECV_TIMEOUT_MS                 100         // Timeout for recvfrom in milliseconds in the receive thread
#define CLIENT_ID                       0xAA        // Example client ID, can be set dynamically
#define CLIENT_NAME                     "lkdc UDP Text/File Transfer Client"

#define CONNECTION_SUCCESFULL_TIMEOUT_MS    2500   

#define TEXT_CHUNK_SIZE                 (TEXT_FRAGMENT_SIZE * 128)
#define FILE_CHUNK_SIZE                 (FILE_FRAGMENT_SIZE * 128)

#define RESEND_TIMEOUT                  10           //seconds
#define RESEND_TIME_TRANSFER            1000        //miliseconds
#define RESEND_TIME_IDLE                10          //miliseconds

#define MAX_MESSAGE_SIZE                (INT32_MAX - 1)         // Max size of a long text message

#define HASH_FRAME_HIGH_WATERMARK       (HASH_SIZE_FRAME * 0.4)
#define HASH_FRAME_LOW_WATERMARK        (HASH_SIZE_FRAME * 0.2)

#define BLOCK_SIZE_FRAME                ((uint64_t)(sizeof(FramePendingAck)))
#define BLOCK_COUNT_FRAME               ((uint64_t)(HASH_FRAME_HIGH_WATERMARK * 2))

#define TIMEOUT_METADATA_RESPONSE_MS    (5000)      //wait for 5 sec after sending a metadata fragment for response
#define MAX_RETRIES_STOP_TRANSFER       (5)         //max retries to stop file/message transfer


typedef uint8_t SessionStatus;
enum SessionStatus{
    SESSION_DISCONNECTED = 0,
    SESSION_CONNECTING = 1,
    SESSION_CONNECTED = 2
};

typedef struct{
    uint8_t sha256[32];
}FileHash;

typedef struct{

    FILE *fp;
    char *fpath;
    char *fname;
    long long file_size;
    uint32_t file_id;
    uint64_t remaining_bytes_to_send;

//    uint64_t pending_metadata_seq_num;  
    uint8_t chunk_buffer[FILE_CHUNK_SIZE];
//    uint32_t chunk_bytes_to_send;
//    uint32_t chunk_fragment_offset;
//    uint32_t frame_fragment_size;
//    uint64_t frame_fragment_offset;
    BOOL throttle;

    SHA256_CTX sha256_ctx;
    FileHash file_hash;

    HANDLE hevent_start_file_transfer;
    HANDLE hevent_stop_file_transfer;
    HANDLE hevent_file_metadata;
    HANDLE hthread_file_transfer;

}FileStream;

typedef struct{

    FILE *fp;
    char *fpath;
    char *fname;
    long long text_file_size;

    char *message_buffer;
    uint32_t message_id;
    uint32_t message_len;
    uint32_t remaining_bytes_to_send;
    uint32_t frame_fragment_offset;
    uint32_t frame_fragment_len;
    BOOL throttle;  

    HANDLE hevent_start_message_send;
    HANDLE hevent_stop_message_send;
    HANDLE hthread_message_send;
}MessageStream;

typedef struct{
    SOCKET socket;
    struct sockaddr_in client_addr;
    struct sockaddr_in server_addr;

    uint8_t client_status;         
    uint8_t session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint32_t client_id;
    uint8_t flags;
    char client_name[NAME_SIZE];
    time_t last_active_time;
   
    volatile uint64_t frame_count;       // this will be sent as seq_num
    volatile uint32_t uid_count;      // file id counter for the current session

    uint32_t session_id;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[NAME_SIZE];       // Human readable server name
    
    FileStream fstream[MAX_CLIENT_FILE_STREAMS];
    MessageStream mstream[MAX_CLIENT_MESSAGE_STREAMS];

    char log_path[PATH_SIZE];
 
} ClientData;

typedef struct {
    QueueFrame queue_frame;
    QueueFrame queue_priority_frame;
    HashTableFramePendingAck ht_frame;
}ClientIOManager;



uint64_t get_new_seq_num(); 

#endif