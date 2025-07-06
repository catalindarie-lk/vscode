#ifndef FRAME_HANDLERS_H
#define FRAME_HANDLERS_H

#include <stdint.h>
#include "frames.h"
#include "server.h"


// Process received file metadata frame
int handle_file_metadata(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager);
// Process received file fragment frame
int handle_file_fragment(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager);
// HANDLE received message fragment frame
int handle_message_fragment(ClientData *client, UdpFrame *frame, ServerIOManager* io_manager);

#endif // FRAME_HANDLERS_H