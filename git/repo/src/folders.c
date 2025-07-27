
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "include/folders.h"
#include "include/protocol_frames.h"


// This function takes a FULL, ABSOLUTE path and ensures all its parent directories exist.
bool CreateAbsoluteFolderRecursive(const char *absolutePathToCreate) {
    char tempPath[MAX_PATH];
    char *p;
    DWORD dwError;

    // Make a copy of the path as strtok_s modifies the string.
    // Ensure it's safe and null-terminated.
    if (strncpy_s(tempPath, sizeof(tempPath), absolutePathToCreate, sizeof(tempPath) - 1) != 0) {
        fprintf(stderr, "Error: Failed to copy path '%s' for recursive creation.\n", absolutePathToCreate);
        return FALSE;
    }
    tempPath[sizeof(tempPath) - 1] = '\0'; // Ensure null termination

    // Handle potential trailing backslash for consistent tokenizing, but only if not root (e.g., "C:\")
    size_t path_len = strlen(tempPath);
    if (path_len > 0 && (tempPath[path_len - 1] == '\\' || tempPath[path_len - 1] == '/')) {
        // If it's just "C:\" or "C:/", don't remove the backslash.
        // Otherwise, remove it.
        if (path_len > 3 || (path_len == 3 && tempPath[1] != ':')) { // Heuristic for non-root paths like "C:\"
             tempPath[path_len - 1] = '\0';
        }
    }

    // Iterate through the path components
    char currentPath[MAX_PATH] = {0};
    char *next_token;
    char *token = strtok_s(tempPath, "\\/", &next_token); // Use both \ and / as delimiters

    // Special handling for drive letters (e.g., "C:", "D:")
    if (token != NULL && strlen(token) == 2 && token[1] == ':') {
        // This is a drive letter, copy it as the first part of currentPath
        if (strcpy_s(currentPath, sizeof(currentPath), token) != 0) {
            fprintf(stderr, "Error: Failed to copy drive letter '%s'.\n", token);
            return FALSE;
        }
        token = strtok_s(NULL, "\\/", &next_token); // Move to the next token
    } else if (token != NULL && (token[0] == '\\' || token[0] == '/') && strlen(token) == 1) {
        // This handles UNC paths like "\\server\share" by ignoring initial empty token
        // if path started with \\ or //, but only if it's not a single backslash
        // for relative path from current directory.
        // For simplicity with drive letters, we assume absolute paths here for this logic.
    }

    while (token != NULL) {
        // Append current token to currentPath with a backslash
        if (strlen(currentPath) > 0) { // Add backslash if not the very first component
            if (strcat_s(currentPath, sizeof(currentPath), "\\") != 0) {
                fprintf(stderr, "Error: Failed to append backslash to '%s'.\n", currentPath);
                return FALSE;
            }
        }
        if (strcat_s(currentPath, sizeof(currentPath), token) != 0) {
            fprintf(stderr, "Error: Failed to append token '%s' to '%s'.\n", token, currentPath);
            return FALSE;
        }

        // Try to create the directory
        if (!CreateDirectoryA(currentPath, NULL)) {
            dwError = GetLastError();
            if (dwError != ERROR_ALREADY_EXISTS) {
                fprintf(stderr, "Failed to create directory '%s'. Error code: %lu\n", currentPath, dwError);
                return FALSE;
            }
        }
        token = strtok_s(NULL, "\\/", &next_token);
    }
    // printf("Folder structure '%s' created or already exists.\n", absolutePathToCreate);
    return TRUE;
}


// wrapper function to create a relative path within a given root
bool CreateRelativeFolderRecursive(const char *rootDirectory, const char *relativePath) {
    char fullPathToCreate[MAX_PATH];
    int result;

    if (rootDirectory == NULL || relativePath == NULL) {
        fprintf(stderr, "Error: rootDirectory or relativePath is NULL.\n");
        return FALSE;
    }

    // Determine if rootDirectory already has a trailing backslash
    size_t root_len = strlen(rootDirectory);
    if (root_len == 0) {
        fprintf(stderr, "Error: rootDirectory is empty.\n");
        return FALSE;
    }

    // Construct the full absolute path
    if (root_len + strlen(relativePath) + 2 > MAX_PATH) { // +1 for potential backslash, +1 for null terminator
        fprintf(stderr, "Error: Combined path '%s\\%s' exceeds MAX_PATH (%d).\n", rootDirectory, relativePath, MAX_PATH);
        return FALSE;
    }

    // Copy root directory
    result = strcpy_s(fullPathToCreate, sizeof(fullPathToCreate), rootDirectory);
    if (result != 0) {
        fprintf(stderr, "Error: Failed to copy rootDirectory '%s'. errno: %d\n", rootDirectory, errno);
        return FALSE;
    }

    // Append a backslash if rootDirectory doesn't already have one and it's not a bare drive letter (e.g., "C:")
    if (fullPathToCreate[root_len - 1] != '\\' && fullPathToCreate[root_len - 1] != '/') {
         // Special handling for drive roots like "C:" - ensure it becomes "C:\"
        if (!(root_len == 2 && fullPathToCreate[1] == ':')) {
            result = strcat_s(fullPathToCreate, sizeof(fullPathToCreate), "\\");
            if (result != 0) {
                fprintf(stderr, "Error: Failed to append backslash to rootDirectory. errno: %d\n", errno);
                return FALSE;
            }
        }
    }

    // Append relative path
    result = strcat_s(fullPathToCreate, sizeof(fullPathToCreate), relativePath);
    if (result != 0) {
        fprintf(stderr, "Error: Failed to append relativePath '%s'. errno: %d\n", relativePath, errno);
        return FALSE;
    }

    // Now call the core recursive creation function with the combined path
    //printf("Attempting to create full path: '%s'\n", fullPathToCreate);
    return CreateAbsoluteFolderRecursive(fullPathToCreate);
}


/**
 * @brief Normalizes a Windows-style file path by:
 * 1. Replacing consecutive backslashes with a single one.
 * 2. Handling trailing backslashes for consistency (adds one for drive roots,
 * removes for others unless specified).
 * 3. Handles UNC paths (\\server\share) correctly by preserving leading \\.
 *
 * @param path The input path string (will be modified in place).
 * @param add_trailing_backslash If true, ensures a single trailing backslash for directories
 * (unless it's a file path). If false, removes trailing backslash
 * unless it's a drive root (e.g., "C:\").
 * @return True on success, False if the path is too long after normalization or input is invalid.
 */
bool NormalizePaths(char *path, bool add_trailing_backslash) {
    if (path == NULL || strlen(path) == 0) {
        // Empty or NULL path is considered an invalid input for normalization
        fprintf(stderr, "Error: Input path is NULL or empty.\n");
        return false;
    }

    char normalized_path[MAX_PATH];
    int write_idx = 0;
    int read_idx = 0;
    bool is_unc_path = false;

    // Handle leading double backslashes for UNC paths (e.g., \\server\share)
    if (path[0] == '\\' && path[1] == '\\') {
        normalized_path[write_idx++] = '\\';
        normalized_path[write_idx++] = '\\';
        read_idx = 2;
        is_unc_path = true;
    } else if (path[0] == '/' && path[1] == '/') { // Also support forward slashes for UNC-like paths
        normalized_path[write_idx++] = '\\'; // Normalize to backslash for Windows
        normalized_path[write_idx++] = '\\';
        read_idx = 2;
        is_unc_path = true;
    }

    // Handle drive letter root (e.g., "C:")
    // This is distinct from UNC paths or relative paths
    if (!is_unc_path && strlen(path) >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
        normalized_path[write_idx++] = path[0];
        normalized_path[write_idx++] = ':';
        // If there's no backslash after the drive letter, add one to read_idx if needed
        if (strlen(path) > 2 && (path[2] == '\\' || path[2] == '/')) {
            read_idx = 3; // Skip C:\ or C:/
        } else {
            // Path is just "C:", we'll add the '\' later if needed
            read_idx = 2;
        }
    }

    // Process the rest of the path
    bool last_char_was_separator = false;
    for (; read_idx < strlen(path); ++read_idx) {
        if (path[read_idx] == '\\' || path[read_idx] == '/') {
            if (!last_char_was_separator) { // Only add one separator if multiple found
                if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow during normalization.\n"); return false; }
                normalized_path[write_idx++] = '\\'; // Normalize all to backslashes
            }
            last_char_was_separator = true;
        } else {
            if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow during normalization.\n"); return false; }
            normalized_path[write_idx++] = path[read_idx];
            last_char_was_separator = false;
        }
    }

    // Handle trailing backslashes
    if (write_idx > 0) {
        // If the path ends with a separator and it's not a bare drive root (e.g., "C:\")
        // and it's not a bare UNC root (e.g., "\\server\share")
        bool is_drive_root = (write_idx == 3 && normalized_path[1] == ':' && normalized_path[2] == '\\');
        bool is_unc_root = (is_unc_path && write_idx == 2); // after \\

        // Remove trailing backslash if not desired and not a drive root or UNC root
        if (!add_trailing_backslash && normalized_path[write_idx - 1] == '\\' && !is_drive_root && !is_unc_root) {
            write_idx--;
        }
        // Add trailing backslash if desired and not already present, and not ending with a drive letter (e.g., "C:")
        else if (add_trailing_backslash && normalized_path[write_idx - 1] != '\\') {
            // If the path ends with a drive letter (like "C:"), add a backslash to make it "C:\"
            if (!(write_idx == 2 && normalized_path[1] == ':')) {
                if (write_idx >= MAX_PATH - 1) { fprintf(stderr, "Error: Path buffer overflow when adding trailing backslash.\n"); return false; }
                normalized_path[write_idx++] = '\\';
            }
        }
    }

    // Null-terminate the normalized path
    if (write_idx >= MAX_PATH) { // Final check after possible trailing backslash addition
        fprintf(stderr, "Error: Path buffer overflow after normalization and termination.\n");
        return false;
    }
    normalized_path[write_idx] = '\0';

    // Copy the normalized path back to the original buffer
    if (strcpy_s(path, MAX_PATH, normalized_path) != 0) {
        fprintf(stderr, "Error: Failed to copy normalized path back to original buffer.\n");
        return false;
    }

    return true;
}




//======================================================================================================================================================================================================
// Function to check if a file exists and is not a directory.
// Uses Windows API for direct and robust checks without actually opening the file.
static bool file_exists(const char* filename) {
    DWORD attributes = GetFileAttributesA(filename);
    return (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
}

// Function to generate a unique filename by appending a timestamp (including milliseconds).
// It takes the original full path, a buffer to write the new path into, and the buffer's size.
// Returns true on success (new path written to buffer), false on failure (e.g., buffer overflow).
static bool generate_timestamp_filename_fixed_buffer(const char* original_full_path, char* buffer, size_t buffer_size) {
    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname_base[_MAX_FNAME]; // Base name without extension
    char ext[_MAX_EXT];          // Extension including the dot (e.g., ".txt")

    // Use _splitpath_s for robust parsing of the original path on Windows.
    _splitpath_s(original_full_path, drive, _MAX_DRIVE, dir, _MAX_DIR, fname_base, _MAX_FNAME, ext, _MAX_EXT);

    SYSTEMTIME st;
    GetSystemTime(&st); // Get current system time with millisecond precision

    // Format the timestamp as YYYYMMDD_HHMMSS_mmm.
    char timestamp_str[64]; // Buffer for the timestamp string itself.
    int len = snprintf(timestamp_str, sizeof(timestamp_str), "_%04d%02d%02d_%02d%02d%02d_%03d",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Check for snprintf errors or if the timestamp string itself overflowed its buffer.
    if (len < 0 || len >= sizeof(timestamp_str)) {
        fprintf(stderr, "Error in generate_timestamp_filename_fixed_buffer: Timestamp string buffer too small or formatting error.\n");
        return false;
    }

    // Combine fname_base and timestamp_str into a single filename for _makepath_s.
    char new_fname_with_timestamp[_MAX_FNAME + sizeof(timestamp_str)]; // Sufficiently large buffer.
    strcpy_s(new_fname_with_timestamp, sizeof(new_fname_with_timestamp), fname_base);
    strcat_s(new_fname_with_timestamp, sizeof(new_fname_with_timestamp), timestamp_str);

    // _makepath_s safely combines path components back into the destination buffer.
    // Use the combined filename (new_fname_with_timestamp) as the 'fname' argument.
    int result = _makepath_s(buffer, buffer_size, drive, dir, new_fname_with_timestamp, ext);

    // _makepath_s returns 0 on success. Non-zero indicates an error (e.g., buffer too small).
    if (result != 0) {
        fprintf(stderr, "Error in generate_timestamp_filename_fixed_buffer: Failed to create new path (destination buffer too small or invalid characters).\n");
        return false;
    }

    return true; // Successfully generated new path.
}


// Opens a file with the specified mode. If the file already exists,
// it generates a new timestamped name and attempts to open that.
//
// - in_fpath: The original, desired path for the file. This string will not be modified.
// - out_fpath: A buffer provided by the caller to store the
//              *actual* path that was opened (either the original or the new timestamped path).
// - fpath_max_size: The size of `out_fpath` in bytes.
// - mode: The file opening mode string (e.g., "wb+", "r", "a").
//
// Returns a FILE* pointer on success, or NULL on failure.
FILE* FopenRename(const char* in_fpath, char* out_fpath, size_t fpath_max_size, const char* mode) {
    // Basic validation of input paths and buffer.
    if (!in_fpath || !in_fpath[0] || !out_fpath || fpath_max_size == 0) {
        fprintf(stderr, "Error in _fopen_rename: Invalid input path, output buffer, or buffer size.\n");
        return NULL;
    }

    // Copy the original input path to the output buffer first.
    // This is the path we'll attempt to open initially.
    // Use strcpy_s for safety, ensuring it doesn't overrun the out_fpath.
    if (strcpy_s(out_fpath, fpath_max_size, in_fpath) != 0) {
        fprintf(stderr, "Error in _fopen_rename: Input path too long for output buffer.\n");
        return NULL;
    }

    FILE* fp = NULL;

    // Check if the file (at the path currently in out_fpath) already exists.
    if (file_exists(out_fpath)) {
        // fprintf(stdout, "File '%s' already exists. Generating new file name with timestamp...\n", out_fpath);

        // Generate the new timestamped filename directly into out_fpath.
        bool success = generate_timestamp_filename_fixed_buffer(in_fpath, out_fpath, fpath_max_size);

        if (!success) {
            // If generating the new name failed (e.g., buffer overflow during timestamp creation).
            fprintf(stderr, "Error in _fopen_rename: Failed to generate a unique filename for '%s'. Aborting open.\n", in_fpath);
            return NULL; // Return NULL indicating failure to open.
        }
        // fprintf(stdout, "New file path generated: '%s'\n", out_fpath);
    } else {
        // File does not exist, so we will attempt to open it directly with its original name
        // (which is already copied into out_fpath).
        // fprintf(stdout, "File '%s' does not exist. Attempting to open directly...\n", out_fpath);
    }

    // Attempt to open the file using the (potentially modified) path stored in out_fpath.
    fp = fopen(out_fpath, mode);

    // Check if fopen was successful.
    if (fp == NULL) {
        // fopen failed. Print an error message including the path and errno.
        fprintf(stderr, "Error in _fopen_rename: Failed to create/open file '%s' with mode '%s' (errno: %d - %s)\n",
                out_fpath, mode, errno, strerror(errno));
    } else {
        // File successfully opened.
        // The actual path used is now stored in out_fpath.
    }

    // Return the FILE* pointer (NULL on failure, valid pointer on success).
    return fp;
}
