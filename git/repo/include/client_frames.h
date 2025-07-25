#ifndef CLIENT_FRAMES_H
#define CLIENT_FRAMES_H

#include <stdint.h>
#include <ws2tcpip.h>
#include "include/client.h"

size_t s_strnlen(const char *s, size_t maxlen);

int send_connect_request(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const uint32_t client_id, 
                    const uint32_t flags, 
                    const char *client_name, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );

int send_keep_alive(const uint64_t seq_num, 
                    const uint32_t session_id, 
                    const SOCKET src_socket, 
                    const struct sockaddr_in *dest_addr
                );

int send_file_metadata(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *rpath,
                            const char *fname,
                            const uint32_t file_fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        );

int send_file_fragment(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size, 
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
                        );

int send_file_end(const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const char *file_hash,
                            const SOCKET src_socket, 
                            const struct sockaddr_in *dest_addr,
                            ClientBuffers* buffers
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
                            ClientBuffers* buffers
                        );



#endif