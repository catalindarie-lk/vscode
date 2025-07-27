#ifndef FOLDERS_H
#define FOLDERS_H

#include <stdbool.h>


bool CreateAbsoluteFolderRecursive(const char *absolutePathToCreate);

bool CreateRelativeFolderRecursive(const char *rootDirectory, const char *relativePath);

bool NormalizePaths(char *path, bool add_trailing_backslash);

FILE* _fopen_rename(const char* in_fpath, char* out_fpath, size_t fpath_max_size, const char* mode);

#endif