#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/queue.h"
#include "include/mem_pool.h"
#include "include/hash.h"
#include "include/sha256.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS                         (0)
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR                           (-1)
#endif
#ifndef MAX_CLIENT_MESSAGE_STREAMS
#define MAX_CLIENT_MESSAGE_STREAMS              (10)
#endif


#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC             (120)
#endif

#define MAX_SERVER_ACTIVE_FSTREAMS              (10)


// --- Constants 
#define SERVER_NAME                             "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                             (10)

#define FRAGMENTS_PER_CHUNK                     (64ULL)
#define CHUNK_TRAILING                          (1u << 7) // 0b10000000
#define CHUNK_BODY                              (1u << 6) // 0b01000000
#define CHUNK_HASHED                            (1u << 5) // 0b00100000
#define CHUNK_WRITTEN                           (1u << 0) // 0b00000001
#define CHUNK_NONE                              (0)       // 0b00000000

#define BLOCK_SIZE_CHUNK                        ((uint64_t)(FILE_FRAGMENT_SIZE * 64))
#define BLOCK_COUNT_CHUNK                       ((uint64_t)(8192))

#define SERVER_SIZE_QUEUE_FRAME                 (8192 * MAX_CLIENTS)
#define SERVER_SIZE_QUEUE_PRIORITY_FRAME        (1024)

#define SERVER_SIZE_FQUEUE_ACK                  (65536)
#define SERVER_SIZE_MQUEUE_ACK                  (65536)
#define SERVER_SIZE_QUEUE_PRIORITY_ACK          (1024)

enum Status{
    STATUS_NONE = 0,
    STATUS_BUSY = 1,
    STATUS_READY = 2,
    STATUS_ERROR = 3
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
    STREAM_ERR_FREAD = 4,

    STREAM_ERR_SHA256_MISMATCH = 7,

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
    struct sockaddr_in server_addr;            // Server address structure
    uint8_t server_status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    volatile uint32_t session_id_counter;   // Global counter for unique session IDs
    char name[MAX_NAME_SIZE];               // Human-readable server name
    
    IOCP_CONTEXT iocp_context;
    HANDLE iocp_handle;
   
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

typedef struct {
    BOOL busy;                          // Indicates if this message stream is currently in use.
    uint8_t stream_err;                 // Stores an error code if something goes wrong with the stream.
    char *buffer;                       // Pointer to the buffer holding the message data.
    uint64_t *bitmap;                   // Pointer to a bitmap used for tracking received fragments. Each bit represents a fragment.
    uint32_t sid;                       // Session ID: Unique identifier for the sender of the message.
    uint32_t mid;                       // Message ID: Unique identifier for the message itself.
    uint32_t mlen;                      // Message Length: Total expected length of the complete message in bytes.
    uint32_t chars_received;            // Characters Received: Current number of characters (bytes) received so far for this message.
    uint64_t fragment_count;            // Fragment Count: Total number of expected fragments for the complete message.
    uint64_t bitmap_entries_count;      // Bitmap Entries Count: The number of uint64_t entries in the bitmap array.

    char fnm[MAX_NAME_SIZE];      // File Name: Buffer to store the name of the file associated with this message stream.

    CRITICAL_SECTION lock;              // Lock: Spinlock/Mutex to protect access to this MsgStream structure in multithreaded environments.
} MessageStream;

typedef struct{

    // uint32_t client_index;              // client_list[index]
    // uint32_t fstream_index;             // client.fstream[index]

    // SOCKET srv_socket;
    struct sockaddr_in client_addr;

    uint32_t sid;                       // Session ID associated with this file stream.
    uint32_t fid;                       // File ID, unique identifier for the file associated with this file stream.
    uint64_t fsize;                     // Total size of the file being transferred.

    uint64_t *bitmap;                   // Pointer to an array of uint64_t, where each bit represents a file fragment.
                                        // A bit set to 1 means the fragment has been received.
    uint8_t* flag;                      // Pointer to an array of uint8_t, used for custom flags per chunk/bitmap entry.
    char** pool_block_file_chunk;       // Pointer to an array of char pointers, where each char* points to a
                                        // memory block holding a complete chunk of data (64 fragments).
    BOOL trailing_chunk;                // True if the last bitmap entry represents a partial chunk (less than 64 fragments).
    BOOL trailing_chunk_complete;       // True if all bytes for the last, potentially partial, chunk have been received.
    uint64_t trailing_chunk_size;       // The actual size of the last chunk (if partial).

    uint64_t file_end_frame_seq_num;
    uint64_t fragment_count;            // Total number of fragments in the entire file.
    uint64_t recv_bytes_count;          // Total bytes received for this file so far.
    uint64_t written_bytes_count;       // Total bytes written to disk for this file so far.
    uint64_t bitmap_entries_count;      // Number of uint64_t entries in the bitmap array.
    uint64_t hashed_chunks_count;
 
    BOOL fstream_busy;                  // Indicates if this stream channel is currently in use for a transfer.
    uint8_t fstream_err;                // Stores an error code if something goes wrong with the stream.
    BOOL file_complete;                 // True if the entire file has been received, written and sha256 validated.
    BOOL file_bytes_received;           // True if all file bytes have been received.
    BOOL file_bytes_written;            // True if all file bytes were written to disk
    BOOL file_hash_received;            // True if end frame with sha256 received from the client
    BOOL file_hash_calculated;          // True if sha256 was calculated by the server
    BOOL file_hash_validated;           // True if received sha256 is equal to calculated sha256

    uint8_t received_sha256[32];        // Buffer for sha256 received from the client
    uint8_t calculated_sha256[32];      // Buffer for sha256 calculated by the server

    char fname[MAX_PATH];          // Array to store the file name+path.
    char fpath[MAX_PATH];
    FILE *fp;                           // File pointer for the file being written to disk.

    CRITICAL_SECTION lock;              // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.

    // HANDLE hevent_recv_file;
    // HANDLE htread_recv_file;
    // HANDLE hevent_close_stream;

}ServerFileStream;

typedef struct {
    SOCKET srv_socket;
    struct sockaddr_in client_addr;            // Client's address
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    
    uint32_t cid;                 // Unique ID received from the client
    char name[MAX_NAME_SIZE];               // Optional: human-readable identifier received from the client
    uint8_t flags;                       // Flags received from the client (e.g., protocol version, capabilities)
    uint8_t connection_status;
 
    uint32_t sid;                // Unique ID assigned by the server for this clients's session
    volatile time_t last_activity_time; // Last time the client sent a frame (for timeout checks)             

    uint32_t slot;                  // Index of the slot the client is connected to [0..MAX_CLIENTS-1]
    uint8_t slot_status;                // 0->FREE; 1->BUSY

    MessageStream mstream[MAX_CLIENT_MESSAGE_STREAMS];
//    ServerFileStream fstream[MAX_CLIENT_FILE_STREAMS];
     
    Statistics statistics;

    CRITICAL_SECTION lock;

} Client;
 
typedef struct{
    Client client[MAX_CLIENTS];     // Array of connected clients
    CRITICAL_SECTION lock;          // For thread-safe access to connected_clients
}ClientList;

typedef struct{
    uint32_t map[MAX_CLIENTS];
    CRITICAL_SECTION lock;
}ClientMap;

typedef struct {
    MemPool pool_file_chunk;

    QueueFrame queue_frame;
    QueueFrame queue_priority_frame;

    QueueAck mqueue_ack;
    QueueAck fqueue_ack;
    QueueAck queue_priority_ack;

    QueueFstream queue_fstream;
    ServerFileStream fstream[MAX_SERVER_ACTIVE_FSTREAMS];
    // CRITICAL_SECTION fstream_list_lock;
    // uint32_t available_fstreams;

    HANDLE test_semaphore;

    HashTableFramePendingAck ht_frame;
    HashTableIdentifierNode ht_fid;
    HashTableIdentifierNode ht_mid;

}ServerBuffers;

#endif