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

int handle_file_metadata(Client *client, UdpFrame *frame, ServerBuffers* buffers);
int handle_file_fragment(Client *client, UdpFrame *frame, ServerBuffers* buffers);
int handle_file_end(Client *client, UdpFrame *frame, ServerBuffers* buffers);

#endif // FRAME_HANDLERS_H