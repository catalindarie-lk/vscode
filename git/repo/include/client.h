#ifndef CLIENT_H
#define CLIENT_H


#include <stdint.h>
#include <windows.h>
#include "include/protocol_frames.h"
#include "include/queue.h"
#include "include/mem_pool.h"
#include "include/hash.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif
#ifndef MAX_CLIENT_MESSAGE_STREAMS
#define MAX_CLIENT_MESSAGE_STREAMS              (10)
#endif
#ifndef MAX_CLIENT_FILE_STREAMS
#define MAX_CLIENT_FILE_STREAMS                 10
#endif
#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC             (120)
#endif

#define MAX_CLIENT_ACTIVE_FSTREAMS              (3)
#define MAX_CLIENT_ACTIVE_MSTREAMS              (3)

// --- Constants ---

#define CLIENT_ID                               (0xFF)        // Example client ID, can be set dynamically
#define CLIENT_NAME                             "lkdc UDP Text/File Transfer Client"

#define CONNECT_REQUEST_TIMEOUT_MS              (2500)
#define DISCONNECT_REQUEST_TIMEOUT_MS           (2500)

#define TEXT_CHUNK_SIZE                         (TEXT_FRAGMENT_SIZE * 128)
#define FILE_CHUNK_SIZE                         (FILE_FRAGMENT_SIZE * 8192)

#define RESEND_TIMEOUT_SEC                      (10)           //seconds

#define MAX_MESSAGE_SIZE_BYTES                  (16 * 1024 * 1024)         // Max size of a long text message

#define TIMEOUT_METADATA_RESPONSE_MS            (5000)      //wait for 5 sec after sending a metadata fragment for response
#define MAX_RETRIES_STOP_TRANSFER               (5)         //max retries to stop file/message transfer

#define CLIENT_SIZE_QUEUE_PRIORITY_FRAME        (256)

#define CLIENT_SIZE_QUEUE_COMMAND_FSTREAM       (1024)
#define CLIENT_SIZE_QUEUE_COMMAND_MSTREAM       (1024)
//----------------------------------------------------------------------------------------------------------
#define CLIENT_SIZE_QUEUE_FRAME                 (8192 + (512 * MAX_CLIENT_ACTIVE_FSTREAMS))
#define HASH_SIZE_FRAME                         (CLIENT_SIZE_QUEUE_FRAME + CLIENT_SIZE_QUEUE_PRIORITY_FRAME)
#define HASH_FRAME_HIGH_WATERMARK               (HASH_SIZE_FRAME * 0.75)
#define HASH_FRAME_LOW_WATERMARK                (HASH_SIZE_FRAME * 0.25)
#define BLOCK_SIZE_FRAME                        ((uint64_t)(sizeof(FramePendingAck)))
#define BLOCK_COUNT_FRAME                       ((uint64_t)(HASH_SIZE_FRAME))

enum Status{
    STATUS_CLOSED = 0,
    STATUS_BUSY = 1,
    STATUS_READY = 2,
    STATUS_ERROR = 3
};

typedef uint8_t SessionStatus;
enum SessionStatus{
    CONNECTION_CLOSED = 0,
    CONNECTION_LISTENING = 1,
    CONNECTION_ESTABLISHED = 2,
};

typedef struct{
    uint8_t sha256[32];
}FileHash;

typedef struct{

    BOOL mstream_busy;
    
    char *message_buffer;
    uint32_t message_len;
    
    uint32_t message_id;
    uint32_t remaining_bytes_to_send;
    BOOL throttle;

    CRITICAL_SECTION lock;

    // HANDLE hevent_start_message_send;
    // HANDLE hevent_close_message_stream_thread;
    // HANDLE hthread_message_send;
}ClientMessageStream;

typedef struct{
    
    char fpath[MAX_PATH];
    char fname[MAX_PATH];

    long long fsize;
    FILE *fp;
    FileHash fhash;

    uint32_t fid;

    BOOL fstream_busy;
    BOOL throttle;
    uint64_t pending_bytes;
    uint64_t pending_metadata_seq_num;
    uint8_t *chunk_buffer;//[FILE_CHUNK_SIZE];
        
    HANDLE hevent_metadata_response;
    CRITICAL_SECTION lock;

}ClientFileStream;

typedef struct{
    SOCKET socket;
    struct sockaddr_in client_addr;
    struct sockaddr_in server_addr;

    uint8_t client_status;         
    uint8_t session_status;     // 0-DISCONNECTED; 1-CONNECTED
    uint32_t cid;
    uint8_t flags;
    char client_name[MAX_NAME_SIZE];
    time_t last_active_time;
   
    volatile uint64_t frame_count;       // this will be sent as seq_num
    volatile uint32_t fid_count;
    volatile uint32_t mid_count;

    uint32_t sid;        // session id received from the server after connection accepted
    uint8_t server_status;      // 0-NOK; 1-OK (connection confirmed by server)
    uint32_t session_timeout;   // timeout received from the server; to be used to send KEEP_ALIVE frames
    char server_name[MAX_NAME_SIZE];       // Human readable server name
    
    HANDLE hevent_connection_listening;
    HANDLE hevent_connection_established;
    HANDLE hevent_connection_closed;

    //ClientMessageStream mstream[MAX_CLIENT_MESSAGE_STREAMS];

    IOCP_CONTEXT iocp_context;
    HANDLE iocp_handle;
 
} ClientData;

typedef struct {
    HANDLE fstream[MAX_CLIENT_ACTIVE_FSTREAMS];
    HANDLE fstream_semaphore;

    HANDLE mstream[MAX_CLIENT_ACTIVE_MSTREAMS];
    HANDLE mstream_semaphore;

}ClientThreads;

typedef struct {
    QueueFrame queue_frame;
    QueueFrame queue_priority_frame;
    HashTableFramePendingAck ht_frame;

    ClientFileStream fstream[MAX_CLIENT_ACTIVE_FSTREAMS];
    QueueCommand queue_fstream;

    ClientMessageStream mstream[MAX_CLIENT_MESSAGE_STREAMS];
    QueueCommand queue_mstream;
}ClientBuffers;

static void clean_message_stream(ClientMessageStream *mstream);

uint64_t get_new_seq_num();
void request_disconnect();
void force_disconnect();
void timeout_disconnect();
void request_connect();
void transfer_file();
void send_text_message();

#endif 