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

static int msg_match_fragment(Client *client, UdpFrame *frame);
static int msg_validate_fragment(Client *client, const int index, UdpFrame *frame);
static int msg_get_available_stream_channel(Client *client);
static int msg_init_stream(MessageStream *mstream, const uint32_t session_id, const uint32_t message_id, const uint32_t message_len);
static void msg_attach_fragment(MessageStream *mstream, char *fragment_buffer, const uint32_t fragment_offset, const uint32_t fragment_len);
static int msg_check_completion_and_record(MessageStream *mstream);

void close_message_stream(MessageStream *mstream);

// HANDLE received message fragment frame
int handle_message_fragment(Client *client, UdpFrame *frame);

#endif // MESSAGE_HANDLER_H