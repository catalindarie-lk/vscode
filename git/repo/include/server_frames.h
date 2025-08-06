#ifndef SERVER_FRAMES_H
#define SERVER_FRAMES_H

#include <stdint.h>
#include <ws2tcpip.h>
#include "include/server.h"

int construct_ack_frame(PoolEntryAckFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint8_t op_code, 
                    const SOCKET src_socket, const struct sockaddr_in *dest_addr);


int construct_connect_response_frame(PoolEntrySendFrame *entry,
                    const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t session_timeout, 
                    const uint8_t status, 
                    const char *server_name, 
                    SOCKET src_socket, const struct sockaddr_in *dest_addr);

int construct_sack_frame(PoolEntrySendFrame *entry,
                    const uint32_t session_id,
                    const SAckPayload *sack_payload, 
                    const SOCKET src_socket, const struct sockaddr_in *dest_addr);

#endif