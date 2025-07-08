#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "include/fileio.h"

long long get_file_size(const char *filepath){

    FILE *fp = fopen(filepath, "rb"); // Open in binary mode
    if (fp == NULL) {
        fprintf(stderr, "Error: Could not open file!\n");
        return RET_VAL_ERROR;
    }
    // Seek to end to determine file size
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    long long size = _ftelli64(fp);
    if(size == RET_VAL_ERROR){
        fprintf(stderr, "Error reading file size! _ftelli64()\n");
        fclose(fp);
        return RET_VAL_ERROR;
    }
    fclose(fp);
    return(size);
}

size_t safe_fwrite(FILE *fp, const void *buffer, size_t total_size) {
    const size_t max_chunk = 1UL << 30; // 1 GB
    const uint8_t *ptr = (const uint8_t *)buffer;
    size_t total_written = 0;

    while (total_written < total_size) {
        size_t chunk = (total_size - total_written > max_chunk)
                       ? max_chunk
                       : total_size - total_written;

        size_t written = fwrite(ptr + total_written, 1, chunk, fp);
        if (written != chunk) {
            perror("fwrite failed");
            break;
        }
        total_written += written;
    }

    return total_written;
}

size_t safe_fread(FILE *fp, void *buffer, size_t total_size) {
    const size_t max_chunk = 1UL << 30; // 1 GB
    uint8_t *ptr = (uint8_t *)buffer;
    size_t total_read = 0;

    while (total_read < total_size) {
        size_t chunk = (total_size - total_read > max_chunk)
                       ? max_chunk
                       : total_size - total_read;

        size_t read = fread(ptr + total_read, 1, chunk, fp);
        if (read == 0) {
            if (feof(fp)) break;
            perror("fread failed");
            break;
        }
        total_read += read;
    }

    return total_read;
}
