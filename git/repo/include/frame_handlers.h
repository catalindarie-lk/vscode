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

// Process received file metadata frame
int handle_file_metadata(Client *client, UdpFrame *frame, ServerIOManager* io_manager);
// Process received file fragment frame
int handle_file_fragment(Client *client, UdpFrame *frame, ServerIOManager* io_manager);
// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame, ServerIOManager* io_manager);

#endif // FRAME_HANDLERS_H