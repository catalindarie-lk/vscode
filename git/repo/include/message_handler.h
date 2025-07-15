#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <stdint.h>
#include "include/protocol_frames.h"
#include "include/server.h"

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame, ServerBuffers* buffers);

#endif // MESSAGE_HANDLER_H