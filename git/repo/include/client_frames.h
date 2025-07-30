#ifndef CLIENT_FRAMES_H
#define CLIENT_FRAMES_H

#include <stdint.h>
#include <ws2tcpip.h>
#include "include/client.h"

size_t s_strnlen(const char *s, size_t maxlen);

int construct_connect_request(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t client_id, 
                            const uint32_t flags, 
                            const char *client_name);

int construct_disconnect(UdpFrame *frame, 
                            const uint32_t session_id);

int construct_keep_alive(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id);

int construct_file_fragment(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_size);

int construct_file_metadata(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size,
                            const char *rpath,
                            const uint32_t rpath_len,
                            const char *fname,
                            const uint32_t fname_len,
                            const uint32_t file_fragment_size);
                        
int construct_file_end(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t file_id, 
                            const uint64_t file_size, 
                            const char *file_hash);

int construct_text_fragment(UdpFrame *frame,
                            const uint64_t seq_num, 
                            const uint32_t session_id, 
                            const uint32_t message_id, 
                            const uint32_t message_len, 
                            const uint32_t fragment_offset, 
                            const char* fragment_buffer, 
                            const uint32_t fragment_len);

#endif