#ifndef SAFEFILEIO_H
#define SAFEFILEIO_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


// Writes large buffer to file in chunks. Returns total bytes written.
size_t safe_fwrite(FILE *fp, const void *buffer, size_t total_size);

// Reads large data from file into buffer in chunks. Returns total bytes read.
size_t safe_fread(FILE *fp, void *buffer, size_t total_size);


#endif // SAFEFILEIO_H