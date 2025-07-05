#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdio.h>              // For size_t type
#include <stdint.h>             // For uint32_t type
#include <windows.h>
#include "frames.h"             // Frame definitions


// ----- Function implementations -----
// CRC32 calculation
int calculate_crc32(const void *data, size_t len);
// CRC32 calculation
uint32_t calculate_crc32_table(const void *data, size_t len);
// Checksum validation
BOOL is_checksum_valid(const UdpFrame *frame, int bytes_received);


#endif // CHECKSUM_H