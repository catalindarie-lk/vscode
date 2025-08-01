#ifndef CLIENT_H
#define CLIENT_H

#include <stdint.h>                                             // For fixed-width integer types like uint8_t, uint32_t, uint64_t
#include <windows.h>                                            // For Windows-specific types like BOOL, HANDLE, CRITICAL_SECTION, SOCKET, MAX_PATH, etc.
#include "include/protocol_frames.h"                            // Includes definitions related to network frame structures and types
#include "include/queue.h"                                      // Includes definitions for queue data structures
#include "include/mem_pool.h"                                   // Includes definitions for memory pool management
#include "include/hash.h"                                       // Includes definitions for hash table structures

// --- Standard Return Value Definitions ---
#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif
#ifndef DEFAULT_SESSION_TIMEOUT_SEC
#define DEFAULT_SESSION_TIMEOUT_SEC 120
#endif

// --- Client-Specific Constants ---

#define CLIENT_ID                               (0xFF)                // Example client ID, can be set dynamically by the application
#define CLIENT_NAME                             "lkdc UDP Text/File Transfer Client"
#define MIN_CONNECTION_TIMEOUT_SEC              15                     // Minimum timeout for a connection in seconds

#define CLIENT_ROOT_FOLDER                      "H:\\_test\\client_root\\"

#define CONNECT_REQUEST_TIMEOUT_MS              2500                  // Timeout for a connection request in milliseconds
#define DISCONNECT_REQUEST_TIMEOUT_MS           2500                  // Timeout for a disconnect request in milliseconds

#define TEXT_CHUNK_SIZE                         (TEXT_FRAGMENT_SIZE * 128) // Size of a text data chunk (derived from protocol_frames.h)
#define FILE_CHUNK_SIZE                         (FILE_FRAGMENT_SIZE * 128) // Size of a file data chunk (derived from protocol_frames.h)

#define RESEND_TIMEOUT_SEC                      3                     // Seconds before a pending frame is considered for retransmission
#define MAX_MESSAGE_SIZE_BYTES                  (4 * 1024 * 1024)     // Maximum size for a single large text message (4 MB)

// --- Client Stream Configuration ---
#define CLIENT_MAX_ACTIVE_FSTREAMS              5                     // Maximum number of concurrent file streams (transfers)
#define CLIENT_MAX_ACTIVE_MSTREAMS              1                     // Maximum number of concurrent message streams (e.g., long text messages)

// --- Client Worker Thread Configuration ---
#define CLIENT_MAX_THREADS_RECV_SEND_FRAME      2                     // Number of threads dedicated to receiving and sending frames
#define CLIENT_MAX_TREADS_PROCESS_FRAME         2                     // Number of threads dedicated to processing received frames
#define CLIENT_MAX_THREADS_POP_SEND_FRAME       1                     // Number of threads for popping normal send frames from queue
#define CLIENT_MAX_THREADS_POP_SEND_PRIO_FRAME  1                    // Number of threads for popping priority send frames from queue
#define CLIENT_MAX_THREADS_POP_SEND_CTRL_FRAME  1                    // Number of threads for popping control frames from queue

// --- Client Memory Pool Sizes ---
#define CLIENT_POOL_SIZE_IOCP_SEND              1024 // Total size for IOCP send contexts
#define CLIENT_POOL_SIZE_IOCP_RECV              1024                  // Size for IOCP receive contexts

#define CLIENT_POOL_SIZE_SEND_FRAME             (CLIENT_QUEUE_SIZE_SEND_FRAME + \
                                                CLIENT_QUEUE_SIZE_SEND_PRIO_FRAME + \
                                                CLIENT_QUEUE_SIZE_SEND_CTRL_FRAME) \

// --- Client Queue Buffer Sizes ---
#define CLIENT_QUEUE_SIZE_SEND_FRAME            256
#define CLIENT_QUEUE_SIZE_SEND_PRIO_FRAME       32
#define CLIENT_QUEUE_SIZE_SEND_CTRL_FRAME       8


#define CLIENT_QUEUE_SIZE_RECV_FRAME            (4096 + (512 * CLIENT_MAX_ACTIVE_FSTREAMS)) // Size of the queue for received frames
#define CLIENT_QUEUE_SIZE_RECV_PRIO_FRAME       1024                  // Size of the queue for received priority frames

#define CLIENT_QUEUE_SIZE_FSTREAM_COMMANDS      1024                  // Size of the queue for file stream commands
#define CLIENT_QUEUE_SIZE_MSTREAM_COMMANDS      1024                  // Size of the queue for message stream commands

// --- Macro to Parse Global Data to Threads ---
// This macro simplifies passing pointers to global client data structures into thread functions.
// It creates local pointers within the thread function's scope, pointing to the global instances.
#define PARSE_GLOBAL_DATA(client_obj, buffers_obj, threads_obj) \
    ClientData *client = &(client_obj); /* Pointer to the global ClientData structure */ \
    ClientBuffers *buffers = &(buffers_obj); /* Pointer to the global ClientBuffers structure */ \
    ClientThreads *threads = &(threads_obj); /* Pointer to threads struct queue */ \
    MemPool *pool_send_iocp_context = &((buffers_obj).pool_send_iocp_context); /* Pointer to IOCP send context memory pool */ \
    MemPool *pool_recv_iocp_context = &((buffers_obj).pool_recv_iocp_context); /* Pointer to IOCP recv context memory pool */ \
    QueueFrame *queue_recv_frame = &((buffers_obj).queue_recv_frame); /* Pointer to received frame queue */ \
    QueueFrame *queue_recv_prio_frame = &((buffers_obj).queue_recv_prio_frame);/* Pointer to received priority frame queue */\
    s_MemPool *pool_send_frame = &((buffers_obj).pool_send_frame); /* Pointer to send frame memory pool */ \
    QueueSendFrame *queue_send_frame = &((buffers_obj).queue_send_frame); /* Pointer to send queue for normal frames */ \
    QueueSendFrame *queue_send_prio_frame = &((buffers_obj).queue_send_prio_frame); /* Pointer to send queue for priority frames */ \
    QueueSendFrame *queue_send_ctrl_frame = &((buffers_obj).queue_send_ctrl_frame); /* Pointer to send queue for control frames */ \
    TableSendFrame *table_send_frame = &((buffers_obj).table_send_frame); /* Pointer to hash table for sent frames awaiting ACK */ \
    QueueCommand *queue_fstream = &((buffers_obj).queue_process_fstream); /* Pointer to file stream command queue */ \
    QueueCommand *queue_mstream = &((buffers_obj).queue_process_mstream); /* Pointer to message stream command queue */ \
    // end of #define PARSE_GLOBAL_DATA // End marker for the macro definition


// --- Enumerations ---

// General status enumeration for a client entity
enum Status{
    STATUS_CLOSED = 0,                          // Client is closed or inactive
    STATUS_BUSY = 1,                            // Client is currently performing an operation
    STATUS_READY = 2,                           // Client is ready for operations
    STATUS_ERROR = 3                            // Client is in an error state
};

// Session status enumeration for a client's connection
typedef uint8_t SessionStatus;                  // Define SessionStatus as an 8-bit unsigned integer
enum SessionStatus{
    CONNECTION_CLOSED = 0,                      // Connection is not established
    CONNECTION_PENDING = 1,                     // Connection request sent, awaiting response
    CONNECTION_ESTABLISHED = 2,                 // Connection is active and established
};

// --- Structure Definitions ---

// Structure to hold a file's SHA256 hash
typedef struct{
    uint8_t sha256[32];                         // 32 bytes for SHA256 hash
}FileHash;

// Structure representing a single client message stream
typedef struct{
    BOOL mstream_busy;                          // Flag indicating if the message stream is active/busy
    
    char *message_buffer;                       // Pointer to the buffer holding the message content
    uint32_t message_len;                       // Length of the message in bytes
    
    uint32_t message_id;                        // Unique identifier for this message
    uint32_t remaining_bytes_to_send;           // Bytes yet to be sent for this message
    BOOL throttle;                              // Flag to indicate if sending should be throttled

    CRITICAL_SECTION lock;                      // Critical section for protecting access to this stream's data
}ClientMessageStream;

// Structure representing a single client file stream
typedef struct{
    BOOL fstream_busy;                          // Flag indicating if the file stream is active/busy
    FILE *fp;                                   // File pointer for the open file
    
    uint32_t fid;                               // File ID unique to this transfer
    long long fsize;                            // Total size of the file in bytes
    char fpath[MAX_PATH];                       // Full path to the file on the client
    uint32_t fpath_len;                         // Length of the file path string
    char rpath[MAX_PATH];                       // Relative path for the file (for server-side storage)
    uint32_t rpath_len;                         // Length of the relative path string
    char fname[MAX_PATH];                       // File name (without path)
    uint32_t fname_len;                         // Length of the file name string
    FileHash fhash;                             // SHA256 hash of the file

    uint64_t pending_bytes;                     // Remaining bytes of the file data to be sent/received
    uint64_t pending_metadata_seq_num;          // Sequence number for pending metadata frames
    uint8_t *chunk_buffer;                      // Buffer to hold chunks of file data for reading/writing
    BOOL throttle;                              // Flag to indicate if transfer should be throttled    

    HANDLE hevent_metadata_response_ok;         // Event handle for successful metadata response
    HANDLE hevent_metadata_response_nok;        // Event handle for unsuccessful metadata response
    CRITICAL_SECTION lock;                      // Critical section for protecting access to this stream's data
}ClientFileStream;

// Main structure holding global client data and state
typedef struct{
    SOCKET socket;                              // The UDP socket for communication
    struct sockaddr_in client_addr;             // Client's local address information
    struct sockaddr_in server_addr;             // Server's address information

    uint8_t client_status;                      // Current status of the client (from Status enum)
    uint8_t session_status;                     // Current status of the session (from SessionStatus enum)
    uint32_t cid;                               // Client ID
    uint8_t flags;                              // Various flags for client state
    char client_name[MAX_NAME_SIZE];            // Human readable client name
    time_t last_active_time;                    // Timestamp of last activity, used for session timeout
   
    volatile uint64_t frame_count;              // Monotonically increasing counter for frame sequence numbers
    volatile uint32_t fid_count;                // Counter for generating new unique file IDs
    volatile uint32_t mid_count;                // Counter for generating new unique message IDs

    uint32_t sid;                               // Session ID assigned by the server upon successful connection
    uint8_t server_status;                      // Server's reported status (e.g., 0-NOK; 1-OK)
    uint32_t session_timeout;                   // Session timeout value received from the server (for keep-alive)
    char server_name[MAX_NAME_SIZE];            // Human readable server name
    
    HANDLE hevent_connection_pending;           // Event handle for connection pending state
    HANDLE hevent_connection_established;       // Event handle for connection established state
    HANDLE hevent_connection_closed;            // Event handle for connection closed state

    IOCP_CONTEXT iocp_context;                  // IOCP context structure
    HANDLE iocp_handle;                         // Handle to the IOCP port

    HANDLE fstreams_semaphore;                  // Semaphore to control access to file stream slots
    HANDLE mstreams_semaphore;                  // Semaphore to control access to message stream slots

    CRITICAL_SECTION fstreams_lock;             // Critical section for protecting access to the fstream array

    ClientFileStream fstream[CLIENT_MAX_ACTIVE_FSTREAMS]; // Array of active file streams
    ClientMessageStream mstream[CLIENT_MAX_ACTIVE_MSTREAMS]; // Array of active message streams

} ClientData;

// Structure holding handles to all client-related threads
typedef struct {
    HANDLE recv_send_frame[CLIENT_MAX_THREADS_RECV_SEND_FRAME]; // Handles for threads managing frame receive/send
    HANDLE process_frame[CLIENT_MAX_TREADS_PROCESS_FRAME];     // Handles for threads processing frames
    HANDLE resend_frame;                                       // Handle for the resend management thread
    HANDLE keep_alive;                                         // Handle for the keep-alive thread
    HANDLE pop_send_frame[CLIENT_MAX_THREADS_POP_SEND_FRAME];  // Handles for threads popping normal send frames
    HANDLE pop_send_prio_frame;                                // Handle for thread popping priority send frames
    HANDLE pop_send_ctrl_frame;                                // Handle for thread popping control send frames

    HANDLE process_fstream[CLIENT_MAX_ACTIVE_FSTREAMS]; // Handles for individual file stream threads (if any)
    HANDLE process_mstream[CLIENT_MAX_ACTIVE_MSTREAMS]; // Handles for individual message stream threads (if any)   

    HANDLE client_command;                      // Handle for the client command processing thread
}ClientThreads;

// Structure holding all client-related buffer and memory pool structures
typedef struct {
    MemPool pool_send_iocp_context;             // Memory pool for IOCP send contexts
    MemPool pool_recv_iocp_context;             // Memory pool for IOCP receive contexts

    QueueFrame queue_recv_frame;                // Queue for incoming received frames
    QueueFrame queue_recv_prio_frame;           // Queue for incoming priority received frames

    s_MemPool pool_send_frame;                  // Memory pool for normal frames to be sent
    // s_MemPool pool_send_ctrl_frame;             // Memory pool for control frames to be sent
    
    QueueSendFrame queue_send_frame;            // Queue for normal frames awaiting transmission
    QueueSendFrame queue_send_prio_frame;       // Queue for priority frames awaiting transmission
    QueueSendFrame queue_send_ctrl_frame;       // Queue for control frames awaiting transmission
    TableSendFrame table_send_frame;              // Hash table to track sent frames awaiting acknowledgment (ACK)

    QueueCommand queue_process_fstream;         // Queue for file stream commands
    QueueCommand queue_process_mstream;         // Queue for message stream commands
}ClientBuffers;

// --- Global Data Declarations (defined in a corresponding .c file) ---
extern ClientData Client;                       // Global instance of client data
extern ClientBuffers Buffers;                   // Global instance of client buffers and pools
extern ClientThreads Threads;                   // Global instance of client thread handles

// --- Function Prototypes ---
// Functions related to sequence number generation and session management
uint64_t get_new_seq_num();                     // Generates and returns a new unique sequence number for frames
int init_client_session();                      // Initializes the client session, sockets, pools, queues, etc.
int reset_client_session();                     // Resets the client session to a clean state

// Functions for cleaning up stream-specific resources
void clean_file_stream(ClientFileStream *fstream);          // Cleans up resources associated with a file stream
void clean_message_stream(ClientMessageStream *mstream);    // Cleans up resources associated with a message stream
 
// Thread entry point functions
DWORD WINAPI fthread_recv_send_frame(LPVOID lpParam);       // Thread for receiving and dispatching frames
DWORD WINAPI fthread_process_frame(LPVOID lpParam);         // Thread for processing received frames
DWORD WINAPI fthread_resend_frame(LPVOID lpParam);          // Thread for managing retransmissions
DWORD WINAPI fthread_keep_alive(LPVOID lpParam);            // Thread for sending keep-alive messages to maintain session
DWORD WINAPI fthread_pop_send_frame(LPVOID lpParam);        // Thread for popping normal frames from send queue and initiating send
DWORD WINAPI fthread_pop_send_prio_frame(LPVOID lpParam);   // Thread for popping priority frames from send queue
DWORD WINAPI fthread_pop_send_ctrl_frame(LPVOID lpParam);   // Thread for popping control frames from send queue
DWORD WINAPI fthread_process_fstream(LPVOID lpParam);               // Thread for managing a specific file stream operation
DWORD WINAPI fthread_process_mstream(LPVOID lpParam);               // Thread for managing a specific message stream operation
DWORD WINAPI fthread_client_command(LPVOID lpParam);        // Thread for processing client commands (e.g., from UI or other modules)


#endif // CLIENT_H // End of header guard