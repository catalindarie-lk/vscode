#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdio.h>              // For size_t type
#include <stdint.h>             // For uint32_t type
#include <windows.h>
#include "include/protocol_frames.h"             // Frame definitions

#ifndef RET_VAL_SUCCESS
#define RET_VAL_SUCCESS 0
#endif
#ifndef RET_VAL_ERROR
#define RET_VAL_ERROR -1
#endif

// ----- Function implementations -----
// CRC32 calculation
int calculate_crc32(const void *data, size_t len);
// CRC32 calculation
uint32_t calculate_crc32_table(const void *data, size_t len);
// Checksum validation
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);


#endif // CHECKSUM_H