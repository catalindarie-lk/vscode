#ifndef FRAMES_H
#define FRAMES_H

#include <stdint.h>
//#include <winsock2.h>
#include <ws2tcpip.h>


#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

#define MAX_PAYLOAD_SIZE                1400                // Max size of data within a frame payload (adjust as needed)
#define FRAME_DELIMITER                 0xAABB              // A magic number to identify valid frames

#define TEXT_FRAGMENT_SIZE              ((uint32_t)(MAX_PAYLOAD_SIZE - sizeof(uint32_t) * 4))
#define FILE_FRAGMENT_SIZE              ((uint32_t)(MAX_PAYLOAD_SIZE - (sizeof(uint32_t) * 2) - sizeof(uint64_t)))

#define NAME_SIZE                       255                 // Maximum size for client/server names
#define PATH_SIZE                       255                 // Maximum size for file paths


// --- Frame Types ---
typedef uint8_t FrameType;
enum FrameType{

    FRAME_TYPE_CONNECT_REQUEST = 1,             // Client's initial contact to server
    FRAME_TYPE_CONNECT_RESPONSE = 2,            // Server's response to client connect
    FRAME_TYPE_DISCONNECT = 3,                 // Client requests to disconnect

    FRAME_TYPE_ACK = 4,                         // Acknowledgment for a received frame
    FRAME_TYPE_KEEP_ALIVE = 6,

    FRAME_TYPE_FILE_METADATA = 20,       // Client requests to send a file (includes filename, size, hash)
    FRAME_TYPE_FILE_FRAGMENT = 22,                   // File data fragment

    FRAME_TYPE_LONG_TEXT_MESSAGE = 30      // Fragment of a long text message
};

typedef uint8_t AckErrorCode;
enum AckErrorCode {
    STS_ACK = 0,
    STS_KEEP_ALIVE = 1,
    STS_TRANSFER_COMPLETE = 2,     // Transfer was complete, receive buffer de-allocated

    ERR_INVALID_FILE_ID = 100,     // Server has completed this transfer
    ERR_INVALID_SESSION = 101,     // Session ID not recognized
    ERR_DUPLICATE_FRAME = 102,     // Frame was already received
    ERR_TIMEOUT = 104,             // Session timed out due to inactivity
    ERR_UNSUPPORTED_FRAME = 105,   // Frame type not supported
    ERR_MALFORMED_FRAME = 106,     // Frame structure or size invalid
    ERR_RESOURCE_LIMIT = 107,      // Server ran out of memory or slots
    ERR_UNAUTHORIZED = 108,        // Authentication/authorization failed
    ERR_INTERNAL_ERROR = 109,      // Catch-all for unexpected server fault

};

#pragma pack(push, 1) 
// Common Header for all frames
typedef struct {
    uint16_t start_delimiter;                               // Magic number (e.g., 0xAABB)
    uint8_t  frame_type;                                    // Discriminator: what kind of payload is in the union
    uint64_t seq_num;                                       // Global sequence number for this frame from the sender
    uint32_t session_id;                                    // Unique identifier for the session (e.g., client ID or session ID)
    uint32_t checksum;                                      // Checksum for this frame's header + active union member (CRC32 recommended)
} FrameHeader;
// Payload Structures for different frame types
typedef struct {
    uint32_t client_id;                                     // Unique identifier of the sender
    uint8_t  flag;                                          // Protocol the client supports
    char     client_name[NAME_SIZE];                    // Optional: human-readable identifier
} ConnectRequestPayload;

typedef struct {
    uint32_t session_timeout;                               // Suggested timeout period for client inactivity
    uint8_t  server_status;                                 // BUSY (0) READY (1) or ERR (x), etc
    char     server_name[NAME_SIZE];                    // Optional: human-readable identifier
} ConnectResponsePayload;

typedef struct {
    uint8_t op_code;
} AckPayload;

typedef struct {
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint64_t file_size;                                     // Total size of the file being transferred
    uint8_t  file_hash[32];                                 // For MD5 hash (adjust size for SHA256 etc.)
    char     filename[NAME_SIZE];                       // Max filename length
} FileMetadataPayload;

typedef struct {
    uint32_t file_id;                                       // Unique identifier for the file transfer session
    uint64_t offset;                                        // Offset of this fragment within the file
    uint32_t size;                                          // Length of actual data in 'fragment_data'
    char  bytes[FILE_FRAGMENT_SIZE];                        // Adjusted size
} FileFragmentPayload;

typedef struct {
    uint32_t message_id;                                    // Unique ID for this specific long message
    uint32_t message_len;                                   // Total length of the original message
    uint32_t fragment_len;                                  // Length of actual text data in 'fragment_data'
    uint32_t fragment_offset;                               // Offset of this fragment within the long message
    char     fragment_text[TEXT_FRAGMENT_SIZE];             // Adjusted size
} LongTextPayload;

// Main UDP Frame Structure
typedef struct {
    FrameHeader header;
    union {
        ConnectRequestPayload request;                      // Client's connect request
        ConnectResponsePayload response;                    // Server's response to client connect
        AckPayload ack;
        FileMetadataPayload file_metadata;                  // File metadata request/response
        FileFragmentPayload file_fragment;                  // File data fragment
        LongTextPayload long_text_msg;                      // Fragment of a long text message
        uint8_t raw_payload[MAX_PAYLOAD_SIZE];              // For generic access or padding
    } payload;
} UdpFrame;
#pragma pack(pop)

int send_frame(const UdpFrame *frame, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );
int send_ack(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );
int send_disconnect(const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );

int send_connect_response(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );
int send_connect_request(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t client_id, 
                    const uint32_t flag, 
                    const char *client_name, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );
int send_keep_alive(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );




#endif // FRAMES_H