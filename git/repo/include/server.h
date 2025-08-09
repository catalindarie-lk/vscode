#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server_frames.h"
#include "include/resources.h"
#include "include/queue.h"
#include "include/mem_pool.h"
#include "include/hash.h"
#include "include/sha256.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS                             0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR                               -1
#endif
////////////////////////////////////////////////////

#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC                 60
#endif

// --- Constants 
#define SERVER_NAME                                 "lkdc UDP Text/File Transfer Server"
#define MAX_CLIENTS                                 10
#define SACK_READY_FRAME_TIMEOUT_MS                 100

#define SERVER_PARTITION_DRIVE                      "H:\\"
#define SERVER_ROOT_FOLDER                          "H:\\_test\\server_root\\"
#define SERVER_MESSAGE_TEXT_FILES_FOLDER            "H:\\_test\\messages_root\\"
#define SERVER_SID_FOLDER_NAME_FOR_CLIENT           "SID_"

#define FRAGMENTS_PER_CHUNK                         (64ULL)
#define CHUNK_TRAILING                              (1u << 7) // 0b10000000
#define CHUNK_BODY                                  (1u << 6) // 0b01000000
#define CHUNK_HASHED                                (1u << 5) // 0b00100000
#define CHUNK_WRITTEN                               (1u << 0) // 0b00000001
#define CHUNK_NONE                                  (0)       // 0b00000000

#define BLOCK_SIZE_CHUNK                            (FILE_FRAGMENT_SIZE * 64)
#define BLOCK_COUNT_CHUNK                           (2048 + 64 * MAX_SERVER_ACTIVE_FSTREAMS)

//---------------------------------------------------------------------------------------------------------
// --- Server Stream Configuration ---
#define MAX_SERVER_ACTIVE_FSTREAMS                  10
#define MAX_SERVER_ACTIVE_MSTREAMS                  10

// --- Server Worker Thread Configuration ---
#define SERVER_MAX_THREADS_RECV_SEND_FRAME          1
#define SERVER_MAX_THREADS_PROCESS_FRAME            8

#define SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES    1
#define SERVER_MAX_THREADS_SEND_MESSAGE_ACK_FRAMES  1



//---------------------------------------------------------------------------------------------------------
// --- Server SEND Buffer Sizes ---
#define SERVER_QUEUE_SIZE_SEND_FRAME                (4096 + 256 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_SEND_PRIO_FRAME           128
#define SERVER_QUEUE_SIZE_SEND_CTRL_FRAME           16
#define SERVER_QUEUE_SIZE_SEND_MESSAGE_ACK_FRAME    (1024 + 512 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_SEND_PRIO_ACK_FRAME       128
#define SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ            (SERVER_QUEUE_SIZE_SEND_FRAME)
#define SERVER_QUEUE_SIZE_CLIENT_SLOT               (SERVER_QUEUE_SIZE_CLIENT_ACK_SEQ * MAX_SERVER_ACTIVE_FSTREAMS)
// --- Server SEND Memory Pool Sizes ---
#define SERVER_POOL_SIZE_SEND                       (SERVER_QUEUE_SIZE_SEND_FRAME + \
                                                    SERVER_QUEUE_SIZE_SEND_PRIO_FRAME + \
                                                    SERVER_QUEUE_SIZE_SEND_CTRL_FRAME)
#define SERVER_POOL_SIZE_ACK_SEND                   (SERVER_QUEUE_SIZE_SEND_MESSAGE_ACK_FRAME + \
                                                    SERVER_QUEUE_SIZE_SEND_PRIO_ACK_FRAME)
#define SERVER_POOL_SIZE_IOCP_SEND                  (SERVER_POOL_SIZE_SEND + \
                                                    SERVER_POOL_SIZE_ACK_SEND + 128) // Total size for IOCP send contexts
// --- SERVER RECV Buffer Sizes ---
#define SERVER_QUEUE_SIZE_RECV_FRAME                (8192 + 1024 * MAX_SERVER_ACTIVE_FSTREAMS)
#define SERVER_QUEUE_SIZE_RECV_PRIO_FRAME           128
// --- Server RECV Memory Pool Sizes ---
#define SERVER_POOL_SIZE_RECV                       (SERVER_QUEUE_SIZE_RECV_FRAME + \
                                                    SERVER_QUEUE_SIZE_RECV_PRIO_FRAME)
#define SERVER_POOL_SIZE_IOCP_RECV                  SERVER_POOL_SIZE_RECV + 128



// --- Macro to Parse Global Data to Threads ---
// This macro simplifies passing pointers to global client data structures into thread functions.
// It creates local pointers within the thread function's scope, pointing to the global instances.
#define PARSE_SERVER_GLOBAL_DATA(server_obj, client_list_obj, buffers_obj, threads_obj) \
    ServerData *server = &(server_obj); \
    ClientListData *client_list = &(client_list_obj); \
    ServerBuffers *buffers = &(buffers_obj); \
    ServerThreads *threads = &(threads_obj); \
    MemPool *pool_file_chunk = &((buffers_obj).pool_file_chunk); \
    MemPool *pool_iocp_send_context = &((buffers_obj).pool_iocp_send_context); \
    MemPool *pool_iocp_recv_context = &((buffers_obj).pool_iocp_recv_context); \
    MemPool *pool_recv_udp_frame = &((buffers_obj).pool_recv_udp_frame); \
    QueuePtr *queue_recv_udp_frame = &((buffers_obj).queue_recv_udp_frame); \
    QueuePtr *queue_recv_prio_udp_frame = &((buffers_obj).queue_recv_prio_udp_frame); \
    QueuePtr *queue_process_fstream = &((buffers_obj).queue_process_fstream); \
    TableIDs *table_file_id = &((buffers_obj).table_file_id); \
    TableIDs *table_message_id = &((buffers_obj).table_message_id); \
    MemPool *pool_queue_ack_frame = &((buffers_obj).pool_queue_ack_frame); \
    QueuePtr *queue_message_ack_frame = &((buffers_obj).queue_message_ack_frame); \
    QueuePtr *queue_prio_ack_frame = &((buffers_obj).queue_prio_ack_frame); \
    MemPool *pool_send_udp_frame = &((buffers_obj).pool_send_udp_frame); \
    QueuePtr *queue_send_udp_frame = &((buffers_obj).queue_send_udp_frame); \
    QueuePtr *queue_send_prio_udp_frame = &((buffers_obj).queue_send_prio_udp_frame); \
    QueuePtr *queue_send_ctrl_udp_frame = &((buffers_obj).queue_send_ctrl_udp_frame); \
    QueueClientSlot *queue_client_slot = &((buffers_obj).queue_client_slot); \
    ServerFstreamPool *pool_fstreams = &((server_obj).pool_fstreams); \
    ServerClientPool *pool_clients = &((server_obj).pool_clients); \
// end of #define PARSE_GLOBAL_DATA // End marker for the macro definition



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

    STREAM_ERR_MALFORMED_DATA = 13,
    STREAM_ERR_INTERNAL_COPY  = 14,

    // STREAM_ERR_FILENAME = 20,
    STREAM_ERR_DRIVE_PARTITION = 20,
    STREAM_ERR_ROOT_FOLDER_CREATE = 22,
    STREAM_ERR_FILENAME_EXIST = 21
    
};

typedef uint8_t ClientSlotStatus;
enum ClientSlotStatus {
    SLOT_FREE = 0,
    SLOT_BUSY = 1
};

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

    char rpath[MAX_PATH];
    uint32_t rpath_len;
    char fname[MAX_PATH];               // Array to store the file name+path.
    uint32_t fname_len;

    char fpath[MAX_PATH];
    FILE *fp;                           // File pointer for the file being written to disk.

    SRWLOCK lock;              // Spinlock/Mutex to protect access to this FileStream structure in multithreaded environments.

}ServerFileStream;

typedef struct {
    SAckPayload payload; // SACK payload for this client
    uint32_t ack_pending;
    uint64_t start_timestamp;
    BOOL start_recorded;
    SRWLOCK lock;
}ClientSAckContext;

typedef struct {

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

    MessageStream mstream[MAX_SERVER_ACTIVE_MSTREAMS];
    CRITICAL_SECTION mstreams_lock;
    
    QueueSeq queue_file_ack_seq;

    ClientSAckContext sack_ctx; // SACK context for this client
 
    SRWLOCK lock;
} Client;

typedef struct {
    Client client[MAX_CLIENTS];     // Array of connected clients
    CRITICAL_SECTION lock;          // For thread-safe access to connected_clients
}ClientListData;

__declspec(align(64)) typedef struct {
    ServerFileStream *fstream;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
} ServerFstreamPool;

__declspec(align(64)) typedef struct {
    Client *client;               // Raw memory buffer
    uint64_t free_head;         // Index of the first free block
    uint64_t *next;             // Next free block indices
    uint8_t *used;              // Usage flags (optional, for safety/debugging)
    uint64_t block_count;       // Total number of blocks in the pool
    uint64_t free_blocks;
    SRWLOCK lock;               // Mutex for thread safety
} ServerClientPool;

typedef struct {
    SOCKET socket;
    struct sockaddr_in server_addr;            // Server address structure
    uint8_t server_status;                // Status of the server (e.g., busy, ready, error)
    uint32_t session_timeout;           // Timeout period for client inactivity
    volatile uint32_t session_id_counter;   // Global counter for unique session IDs
    char name[MAX_NAME_SIZE];               // Human-readable server name
    
    IOCP_CONTEXT iocp_context;
    HANDLE iocp_handle;

    ServerFstreamPool pool_fstreams;
    ServerClientPool pool_clients;
}ServerData;

typedef struct {
    MemPool pool_file_chunk;
    MemPool pool_iocp_send_context;
    MemPool pool_iocp_recv_context;

    MemPool pool_recv_udp_frame;
    QueuePtr queue_recv_udp_frame;
    QueuePtr queue_recv_prio_udp_frame;

    MemPool pool_queue_ack_frame;
    QueuePtr queue_message_ack_frame;
    QueuePtr queue_prio_ack_frame;

    MemPool pool_send_udp_frame;
    QueuePtr queue_send_udp_frame;
    QueuePtr queue_send_prio_udp_frame;
    QueuePtr queue_send_ctrl_udp_frame;

    QueuePtr queue_process_fstream;

    QueueClientSlot queue_client_slot;

    TableIDs table_file_id;
    TableIDs table_message_id;
}ServerBuffers;

typedef struct {

    HANDLE recv_send_frame[SERVER_MAX_THREADS_RECV_SEND_FRAME];
    HANDLE process_frame[SERVER_MAX_THREADS_PROCESS_FRAME];

    HANDLE send_file_sack_frame[SERVER_MAX_THREADS_SEND_FILE_SACK_FRAMES];
    HANDLE send_message_ack_frame[SERVER_MAX_THREADS_SEND_MESSAGE_ACK_FRAMES];
    HANDLE scan_for_trailing_sack;
    HANDLE send_prio_ack_frame;

    HANDLE send_frame;
    HANDLE send_prio_frame;
    HANDLE send_ctrl_frame;

    HANDLE client_timeout;
    HANDLE file_stream[MAX_SERVER_ACTIVE_FSTREAMS];
    HANDLE server_command;
} ServerThreads;

//--------------------------------------------------------------------------------------------------------------------------

extern ServerData Server;
extern ServerBuffers Buffers;
extern ClientListData ClientList;
extern ServerThreads Threads;

// Thread functions
static DWORD WINAPI func_thread_recv_send_frame(LPVOID lpParam);
static DWORD WINAPI func_thread_process_frame(LPVOID lpParam);

static DWORD WINAPI fthread_send_file_sack_frame(LPVOID lpParam);
static DWORD WINAPI fthread_scan_for_trailing_sack(LPVOID lpParam);

static DWORD WINAPI fthread_send_message_ack_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_prio_ack_frame(LPVOID lpParam);

static DWORD WINAPI fthread_send_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_prio_frame(LPVOID lpParam);
static DWORD WINAPI fthread_send_ctrl_frame(LPVOID lpParam);

static DWORD WINAPI fthread_client_timeout(LPVOID lpParam);
static DWORD WINAPI fthread_process_file_stream(LPVOID lpParam);
static DWORD WINAPI fthread_server_command(LPVOID lpParam);

// Client management functions
static Client* find_client(const uint32_t session_id);
static Client* add_client(const UdpFrame *recv_frame, const struct sockaddr_in *client_addr);
static int remove_client(const uint32_t slot);

static void cleanup_client(Client *client);
static BOOL validate_file_hash(ServerFileStream *fstream);
static void check_open_file_stream();





#endif