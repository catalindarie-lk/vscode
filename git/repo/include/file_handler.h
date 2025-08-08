#ifndef PROTOCOL_FRAME_HANDLERS_H
#define PROTOCOL_FRAME_HANDLERS_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif


void init_fstream_pool(ServerFileStreamPool* pool, /*const uint64_t block_size,*/ const uint64_t block_count);
ServerFileStream* alloc_fstream(ServerFileStreamPool* pool);
void free_fstream(ServerFileStreamPool* pool, ServerFileStream* fstream);
ServerFileStream* find_fstream(ServerFileStreamPool* pool, const uint32_t sid, const uint32_t fid);
void close_file_stream(ServerFileStream *fstream);

int handle_file_metadata(Client *client, UdpFrame *frame);
int handle_file_fragment(Client *client, UdpFrame *frame);
int handle_file_end(Client *client, UdpFrame *frame);

#endif // FRAME_HANDLERS_H