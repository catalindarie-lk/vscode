#ifndef SERVER_FRAMES_H
#define SERVER_FRAMES_H

#include <stdint.h>
#include <ws2tcpip.h>
#include "include/server.h"

int send_connect_response(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr,
                    MemPool *mem_pool
                );

// int send_file_metadata_response(const uint64_t seq_num, 
//                     const uint32_t session_id, 
//                     const uint32_t file_id, 
//                     const uint8_t op_code,
//                     SOCKET src_socket, 
//                     const struct sockaddr_in *dest_addr,
//                     MemPool *mem_pool
//                 );

int send_ack(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr,
                    MemPool *mem_pool
                );

int construct_ack_frame(PoolEntryAckFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, const struct sockaddr_in *dest_addr);

// int construct_file_metadata_response(PoolEntrySendFrame *entry,
//                     const uint64_t seq_num, 
//                     const uint32_t session_id, 
//                     const uint32_t file_id, 
//                     const uint8_t op_code,
//                     SOCKET src_socket, const struct sockaddr_in *dest_addr);

int construct_connect_response_frame(PoolEntrySendFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, const struct sockaddr_in *dest_addr);

#endif