
#include <stdio.h>                      // For printf, fprintf
#include <string.h>                     // For memset, memcpy
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>                   // For modern IP address functions (inet_pton, inet_ntop)
#include <windows.h>                    // For Windows-specific functions like CreateThread, Sleep       

#include "include/queue.h"
#include "include/client.h"
#include "include/client_api.h"


// helper function to send a file on the queue
// fpath_len: The actual content length of fpath, EXCLUDING the null terminator.
// rpath_len: The actual content length of rpath, EXCLUDING the null terminator.
// fname_len: The actual content length of fname, EXCLUDING the null terminator.
static void _StreamFile(const char *fpath, const size_t fpath_len,
                        const char *rpath, const size_t rpath_len,
                        const char *fname, const size_t fname_len) {

    QueueCommandEntry entry; // Represents the command to be enqueued
    int result; // For checking return values of secure functions
    memset(&entry, 0, sizeof(QueueCommandEntry));

    // --- Input Validation (now based on content lengths) ---

    // Validate fpath (full actual disk directory path)
    if (!fpath) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string invalid pointer.\n"); return; }
    // fpath_len 0 is invalid for a path
    if (fpath_len == 0) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string zero content length.\n"); return; }
    // A string of 'fpath_len' characters needs 'fpath_len + 1' bytes for the null terminator.
    // So, it must be strictly less than MAX_PATH to fit within a MAX_PATH buffer.
    if (fpath_len >= MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, fpath_len); return; }
    // (Optional but good check: Verify actual null termination if function might pass to strlen-reliant APIs)
    // if (strnlen_s(fpath, fpath_len + 1) > fpath_len) { /* Good, null terminated within expected length */ }
    // else { fprintf(stderr, "WARNING: _StreamFile - Absolute directory path may not be null-terminated within its claimed length + 1.\n"); }


    // Validate rpath (relative directory path)
    if (!rpath) { fprintf(stderr, "ERROR: _StreamFile - Relative directory path string invalid pointer.\n"); return; }
    // rpath_len MUST be at least 1 (for '\'), so 0 content length is an error
    if (rpath_len == 0) { // Should not happen with correct logic from recursive function (should always be at least "\")
        fprintf(stderr, "ERROR: _StreamFile - Relative directory path string zero content length.\n");
        return;
    }
    if (rpath_len >= MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Relative directory path string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, rpath_len); return; }
    // (Optional null termination check)


    // Validate fname (just the filename)
    if (!fname) { fprintf(stderr, "ERROR: _StreamFile - Filename string invalid pointer.\n"); return; }
    if (fname_len == 0) { fprintf(stderr, "ERROR: _StreamFile - Filename string zero content length (filename cannot be empty).\n"); return; }
    if (fname_len >= MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Filename string content too long (max %d chars excluding null). Length: %zu\n", MAX_PATH - 1, fname_len); return; }
    // (Optional null termination check)


    // --- Populate QueueCommandEntry using secure functions ---

    // Populate 'text' field for _StreamFile command type
    // Note: "sendfile" has 8 chars, fits easily in 32 bytes with null.
    result = snprintf(entry.command.send_file.text,
                      sizeof(entry.command.send_file.text),
                      "%s", "sendfile");
    // Check for errors (negative return) or truncation (result >= buffer size)
    if (result < 0 || (size_t)result >= sizeof(entry.command.send_file.text)) { // Cast result to size_t for comparison
        fprintf(stderr, "ERROR: _StreamFile - Failed to set 'text' field for send_file command (truncation or error). Result: %d, Buffer Size: %zu\n",
                result, sizeof(entry.command.send_file.text));
        return;
    }

    // Populate 'fpath' field with the FULL ACTUAL DISK DIRECTORY PATH
    // Use '%.*s' to copy exactly 'fpath_len' characters
    result = snprintf(entry.command.send_file.fpath,
                      sizeof(entry.command.send_file.fpath),
                      "%.*s", (int)fpath_len, fpath); // Cast fpath_len to int for printf specifier
    // Check if the exact number of characters was copied
    if (result < 0 || (size_t)result != fpath_len) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy absolute directory path to command entry (truncation or error). Path len: %zu, Result: %d, Buffer Size: %zu\n",
                fpath_len, result, sizeof(entry.command.send_file.fpath));
        return;
    }
    entry.command.send_file.fpath_len = (uint32_t)fpath_len;

    // Populate 'rpath' field with the RELATIVE DIRECTORY PATH
    // Use '%.*s' to copy exactly 'rpath_len' characters
    result = snprintf(entry.command.send_file.rpath,
                      sizeof(entry.command.send_file.rpath),
                      "%.*s", (int)rpath_len, rpath);
    if (result < 0 || (size_t)result != rpath_len) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy relative directory path to command entry (truncation or error). Path len: %zu, Result: %d, Buffer Size: %zu\n",
                rpath_len, result, sizeof(entry.command.send_file.rpath));
        return;
    }
    entry.command.send_file.rpath_len = (uint32_t)rpath_len;

    // Populate 'fname' field with JUST THE FILENAME
    // Use '%.*s' to copy exactly 'fname_len' characters
    result = snprintf(entry.command.send_file.fname,
                      sizeof(entry.command.send_file.fname),
                      "%.*s", (int)fname_len, fname);
    if (result < 0 || (size_t)result != fname_len) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy filename to command entry (truncation or error). Name len: %zu, Result: %d, Buffer Size: %zu\n",
                fname_len, result, sizeof(entry.command.send_file.fname));
        return;
    }
    entry.command.send_file.fname_len = (uint32_t)fname_len;

    // Push the command to the queue
    push_command(&buffers.queue_fstream, &entry); // Use the mock queue
    return;
}

//============================================================================================================================================================

// Send single file
// len: The actual length of the string fpath, NOT including the null terminator.
void SendSingleFile(const char *fpath, size_t len) {
    if (!fpath) {
        fprintf(stderr, "ERROR: SendSingleFile - Input file path is NULL.\n");
        return;
    }

    if (len == 0) {
        // An empty string (len=0) is a valid input if it means current directory or empty filename.
        // However, given the context of sending a single file, an empty path might be an error.
        // The original code treated strlen(fpath) == 0 as an error. We'll keep this logic.
        fprintf(stderr, "ERROR: SendSingleFile - Input file path length is zero (empty path).\n");
        return;
    }

    // MAX_PATH typically includes space for the null terminator.
    // So, if len is MAX_PATH, it means the string *exactly* fills the buffer,
    // leaving no space for the null terminator if the buffer itself is MAX_PATH bytes.
    // Therefore, for a string of 'len' characters to fit AND be null-terminated
    // in a MAX_PATH buffer, 'len' must be strictly less than MAX_PATH.
    if (len >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendSingleFile - Input file path length (%zu) is too long for MAX_PATH (%d) buffer (requires space for null terminator).\n", len, MAX_PATH);
        return;
    }

    if (len != strlen(fpath)) {
        fprintf(stderr, "WARNING: SendSingleFile - Provided length (%zu) does not match actual string length (%zu).\n", len, strlen(fpath));
        return;
    }

    char absoluteDirectoryPath[MAX_PATH]; // This will be the fpath for _StreamFile
    char fileNameOnly[MAX_PATH];          // This will be the fname for _StreamFile
    const char *relativeDirectoryPath = "\\"; // Fixed rpath for SendSingleFile

    // --- Extract Filename and Absolute Directory Path ---
    const char *last_slash_win = NULL;
    const char *last_slash_unix = NULL;
    const char *last_slash = NULL;

    // Search within the provided 'len' boundary
    // Iterate up to 'len' characters to find the last slash.
    for (size_t i = 0; i < len; ++i) {
        if (fpath[i] == '\\') {
            last_slash_win = fpath + i;
        } else if (fpath[i] == '/') {
            last_slash_unix = fpath + i;
        }
    }

    // Determine the last actual path separator (Windows-style preference first)
    if (last_slash_win && (!last_slash_unix || last_slash_win > last_slash_unix)) {
        last_slash = last_slash_win;
    } else if (last_slash_unix) {
        last_slash = last_slash_unix;
    }

    if (last_slash) {
        // Filename is after the last slash
        size_t filename_offset = (size_t)(last_slash - fpath) + 1;
        // The length of the filename string itself
        size_t filename_content_len = len - filename_offset;

        if (filename_offset > len || filename_content_len >= MAX_PATH) { // Check if there's actual filename content and if it fits
            fprintf(stderr, "ERROR: SendSingleFile - Path ends with a separator or filename content is too long for buffer: '%s'.\n", fpath);
            return;
        }

        // Use 'filename_content_len' with '%.*s' to copy exactly that many characters
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%.*s", (int)filename_content_len, fpath + filename_offset);
        if (snprintf_res < 0 || (size_t)snprintf_res != filename_content_len) { // Check if all characters were copied
            fprintf(stderr, "ERROR: SendSingleFile - Failed to extract filename '%s' from path (truncation or error). Result: %d, Expected: %zu\n", fpath + filename_offset, snprintf_res, filename_content_len);
            return;
        }

        // Absolute directory path is up to and including the last slash
        size_t dir_content_len = (size_t)(last_slash - fpath) + 1; // +1 to include the slash itself
        if (dir_content_len >= MAX_PATH) { // Needs space for null terminator
            fprintf(stderr, "ERROR: SendSingleFile - Directory path derived from '%.*s' is too long.\n", (int)len, fpath);
            return;
        }
        memcpy(absoluteDirectoryPath, fpath, dir_content_len);
        absoluteDirectoryPath[dir_content_len] = '\0';

    } else {
        // No slash found, assume the entire path is the filename and the directory is current ".\"
        // The entire input 'fpath' (of 'len' characters) is the filename.
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%.*s", (int)len, fpath);
        if (snprintf_res < 0 || (size_t)snprintf_res != len) { // Check if all characters were copied
            fprintf(stderr, "ERROR: SendSingleFile - Failed to copy full path as filename (truncation or error). Result: %d, Expected: %zu\n", snprintf_res, len);
            return;
        }

        // Set absoluteDirectoryPath to current directory indicator
        snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, ".\\");
        if (snprintf_res < 0 || snprintf_res >= MAX_PATH) { // Check if it fits
            fprintf(stderr, "ERROR: SendSingleFile - Failed to set default absolute directory path (truncation or error). Result: %d\n", snprintf_res);
            return;
        }
    }

    // Call the internal _StreamFile helper
    // fpath_len, rpath_len, fname_len should include the null terminator (+1) as per original calls
    _StreamFile(absoluteDirectoryPath, strlen(absoluteDirectoryPath),
                relativeDirectoryPath, strlen(relativeDirectoryPath), // rpath is fixed to "\"
                fileNameOnly, strlen(fileNameOnly));

    // printf("SendSingleFile: Processed file: %s\n", full_file_path);
}

//============================================================================================================================================================

// Send all files in a folder
// fd_path: The path to the folder.
// len: The actual length of the fd_path string, NOT including the null terminator.
void SendAllFilesInFolder(const char *fd_path, size_t len) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // For checking return values of snprintf

    if (!fd_path) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path is NULL.\n");
        return;
    }

    if (len == 0) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path length is zero (empty path).\n");
        return;
    }

    // len must be less than MAX_PATH to allow space for the null terminator.
    // Also, we need space for "\\*" which adds 2 characters + null terminator
    // So, fd_path_len + 2 + 1 <= MAX_PATH  =>  fd_path_len <= MAX_PATH - 3
    if (len >= MAX_PATH - 2) { // Need at least space for "\*" and null terminator
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input folder path length (%zu) is too long for MAX_PATH (%d) buffer when adding search wildcard.\n", len, MAX_PATH);
        return;
    }

    // Prepare search path: "C:\Folder\*" or "C:\Folder*"
    // Use '%.*s' to copy exactly 'len' characters from fd_path
    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%.*s\\*", (int)len, fd_path);
    // The expected result length is 'len' (from fd_path) + 2 (for "\*").
    // The snprintf_res is the number of characters *written*, excluding the null terminator.
    if (snprintf_res < 0 || (size_t)snprintf_res != len + 2) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to format search path for '%.*s' (truncation or error). Result: %d, Expected: %zu\n", (int)len, fd_path, snprintf_res, len + 2);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            printf("Folder '%.*s' not found.\n", (int)len, fd_path);
        } else {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Could not open folder '%.*s'. System Error Code: %lu\n", (int)len, fd_path, lastError);
        }
        return;
    }

    // --- Prepare 'fpath' for _StreamFile ---
    // This will be the absolute directory path, ensuring a trailing backslash.
    char absoluteDirectoryPathForSend[MAX_PATH];
    size_t current_abs_dir_len = len; // Start with the provided length of fd_path

    // Copy the original fd_path using the provided length
    snprintf_res = snprintf(absoluteDirectoryPathForSend, MAX_PATH, "%.*s", (int)len, fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != len) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy base directory path '%.*s' for _StreamFile (truncation or error). Result: %d, Expected: %zu\n", (int)len, fd_path, snprintf_res, len);
        FindClose(hFind);
        return;
    }

    // Append a trailing backslash if not already present, ensuring space
    // Check the last character of the copied string (which is at index len - 1)
    if (current_abs_dir_len > 0 &&
        absoluteDirectoryPathForSend[current_abs_dir_len - 1] != '\\' &&
        absoluteDirectoryPathForSend[current_abs_dir_len - 1] != '/') {

        if (current_abs_dir_len < MAX_PATH - 1) { // -1 for backslash, -1 for null terminator
            absoluteDirectoryPathForSend[current_abs_dir_len] = '\\';
            absoluteDirectoryPathForSend[current_abs_dir_len + 1] = '\0';
            current_abs_dir_len++; // Update the length to include the new backslash
        } else {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Absolute directory path '%.*s' is too long to add trailing backslash. Path may be incomplete.\n", (int)len, fd_path);
            return;
        }
    } else {
        // If it already has a slash or is empty, ensure null termination at 'len'
        absoluteDirectoryPathForSend[current_abs_dir_len] = '\0';
    }


    // --- Prepare 'rpath' for _StreamFile ---
    // Since this function only sends files from the *top-level* of fd_path
    // (no recursion), the relative path is simply the root relative path.
    // For consistency with your earlier examples, we'll make it "\".
    char relativeDirectoryPathForSend[MAX_PATH];
    snprintf_res = snprintf(relativeDirectoryPathForSend, MAX_PATH, "\\"); // Representing the root relative path
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to set relative directory path to '\\' (truncation or error). Result: %d\n", snprintf_res);
        FindClose(hFind);
        return;
    }


    do {
        // Skip '.' and '..'
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }

        // Only process files, not directories (as this is non-recursive)
        if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // --- Prepare 'fname' for _StreamFile ---
            // Just the filename from findFileData
            char fileNameOnly[MAX_PATH];
            size_t filename_content_len = strlen(findFileData.cFileName);

            // Check if filename fits
            if (filename_content_len >= MAX_PATH) {
                fprintf(stderr, "ERROR: SendAllFilesInFolder - Filename '%s' is too long (%zu) for buffer. Skipping this file.\n", findFileData.cFileName, filename_content_len);
                continue; // Skip this file and try the next
            }

            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != filename_content_len) {
                fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy filename '%s' (truncation or error). Result: %d, Expected: %zu. Skipping this file.\n", findFileData.cFileName, snprintf_res, filename_content_len);
                continue; // Skip this file and try the next
            }

            // --- Call _StreamFile with the three path components ---
            // fpath argument: absoluteDirectoryPathForSend (e.g., "C:\Folder\")
            // rpath argument: relativeDirectoryPathForSend (e.g., "\")
            // fname argument: fileNameOnly (e.g., "MyFile.txt")
            _StreamFile(absoluteDirectoryPathForSend, current_abs_dir_len, // current_abs_dir_len updated above
                        relativeDirectoryPathForSend, strlen(relativeDirectoryPathForSend),
                        fileNameOnly, filename_content_len); // filename_content_len already calculated

            // Print confirmation
            printf("Send File: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
                   absoluteDirectoryPathForSend, relativeDirectoryPathForSend, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}

//============================================================================================================================================================

// --- Recursive Helper Function ---
// root_path_to_replace: The initial absolute path provided by the user. Used to calculate relative paths.
// root_len: The length of root_path_to_replace (excluding null terminator).
// current_fd_path: The absolute path of the folder currently being processed.
// current_len: The length of current_fd_path (excluding null terminator).
static void _SendAllFilesInFolderRecursive(const char *root_path_to_replace, size_t root_len,
                                          const char *current_fd_path, size_t current_len) {

    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // Result for snprintf calls

    // Validation for incoming lengths (can be removed if confident about caller's validation)
    if (!current_fd_path || current_len == 0 || current_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Invalid current folder path or length (path: %p, len: %zu).\n", current_fd_path, current_len);
        return;
    }
    if (!root_path_to_replace || root_len == 0 || root_len >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Invalid root path or length (path: %p, len: %zu).\n", root_path_to_replace, root_len);
        return;
    }


    // Construct the search path using the actual current_fd_path for OS calls
    // current_len (for current_fd_path) + '\' + '*' + '\0'  =>  current_len + 3 bytes needed
    if (current_len >= MAX_PATH - 2) { // Need space for "\*" and null terminator
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Current folder path '%.*s' is too long for search wildcard (len: %zu).\n",
                (int)current_len, current_fd_path, current_len);
        return;
    }

    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%.*s\\*", (int)current_len, current_fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != current_len + 2) { // Expected written chars: current_len + '\' + '*'
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to format search path for '%.*s' (truncation or error). Result: %d, Expected: %zu\n",
                (int)current_len, current_fd_path, snprintf_res, current_len + 2);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Could not open folder '%.*s'. System Error Code: %lu\n", (int)current_len, current_fd_path, lastError);
        }
        return;
    }

    // Prepare the current ACTUAL disk directory path with a trailing backslash.
    // This will be passed as the 'fpath' argument to _StreamFile.
    char absoluteDirectoryPath[MAX_PATH]; // Full absolute path to current directory
    size_t abs_dir_path_len = current_len; // Start with the provided length

    // Copy the current_fd_path using its length
    snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, "%.*s", (int)current_len, current_fd_path);
    if (snprintf_res < 0 || (size_t)snprintf_res != current_len) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to copy current actual directory path '%.*s' (truncation or error). Result: %d, Expected: %zu\n",
                (int)current_len, current_fd_path, snprintf_res, current_len);
        FindClose(hFind);
        return;
    }

    // Append a trailing backslash if it's not already present.
    if (abs_dir_path_len > 0 &&
        absoluteDirectoryPath[abs_dir_path_len - 1] != '\\' &&
        absoluteDirectoryPath[abs_dir_path_len - 1] != '/')
    {
        if (abs_dir_path_len < MAX_PATH - 1) { // Need space for '\' and '\0'
            absoluteDirectoryPath[abs_dir_path_len] = '\\';
            absoluteDirectoryPath[abs_dir_path_len + 1] = '\0';
            abs_dir_path_len++; // Update length to include the new backslash
        } else {
            fprintf(stderr, "WARNING: _SendAllFilesInFolderRecursive - Current actual directory path '%.*s' is too long to add trailing backslash. Path may be incomplete.\n", (int)abs_dir_path_len, absoluteDirectoryPath);
            // Decide if this is a fatal error or just a warning for your application
        }
    } else {
        absoluteDirectoryPath[abs_dir_path_len] = '\0'; // Ensure null termination
    }


    // Prepare the RELATIVE DIRECTORY PATH (for rpath argument of _StreamFile)
    char relativeDirectoryPath[MAX_PATH]; // Will hold "relative\path\" or "\"
    size_t relative_path_content_start_idx; // Index in current_fd_path where relative path starts

    // Determine the part of current_fd_path that comes after root_path_to_replace
    if (current_len >= root_len && _strnicmp(current_fd_path, root_path_to_replace, root_len) == 0)
    {
        relative_path_content_start_idx = root_len;
        // Skip any leading path separators immediately after the root path, unless the root itself is just a drive letter "C:\"
        // This handles cases like root="C:\Folder", current="C:\Folder\Sub" or root="C:", current="C:\Folder"
        if (relative_path_content_start_idx < current_len &&
            (current_fd_path[relative_path_content_start_idx] == '\\' || current_fd_path[relative_path_content_start_idx] == '/')) {
            relative_path_content_start_idx++;
        }

        size_t relative_sub_path_len = current_len - relative_path_content_start_idx;

        if (relative_sub_path_len == 0) { // Current directory IS the root itself (or its path with just a trailing slash)
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
        } else { // Current directory is a subfolder
            // Add leading backslash and trailing backslash
            // "\\", content, "\\" + NULL => relative_sub_path_len + 3 bytes
            if (relative_sub_path_len + 2 >= MAX_PATH) { // +2 for leading/trailing slashes, +1 for null
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Calculated relative path '%.*s' is too long. Skipping this folder.\n",
                        (int)relative_sub_path_len, current_fd_path + relative_path_content_start_idx);
                FindClose(hFind);
                return;
            }
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\%.*s\\", (int)relative_sub_path_len, current_fd_path + relative_path_content_start_idx);
        }
    } else {
        // Fallback: This case should ideally not be hit with correct recursion logic.
        // If for some reason current_fd_path isn't under the root,
        // use a default of "\" for consistency.
        fprintf(stderr, "WARNING: _SendAllFilesInFolderRecursive - Current path '%.*s' is not a subpath of root '%.*s'. Using default relative path.\n",
                (int)current_len, current_fd_path, (int)root_len, root_path_to_replace);
        snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
    }

    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to prepare relative directory path for '%.*s' (truncation or error). Result: %d. Skipping this folder.\n",
                (int)current_len, current_fd_path, snprintf_res);
        FindClose(hFind);
        return;
    }


    // --- Loop through all files and directories found ---
    do {
        // Skip special directory entries: "." and ".."
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }

        size_t found_filename_len = strlen(findFileData.cFileName);
        // Sanity check on found filename length
        if (found_filename_len == 0 || found_filename_len >= MAX_PATH) {
            fprintf(stderr, "WARNING: _SendAllFilesInFolderRecursive - Invalid filename length for '%s' (len: %zu). Skipping.\n", findFileData.cFileName, found_filename_len);
            continue;
        }


        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // It's a subdirectory, recurse into it.
            // Construct the actual full path to the subfolder for the recursive call.
            char subFolderPathActual[MAX_PATH];
            // current_len + '\\' + found_filename_len + '\0' => current_len + 1 + found_filename_len + 1 bytes needed
            size_t sub_path_len = current_len + 1 + found_filename_len;

            if (sub_path_len >= MAX_PATH) { // Check if the new path will exceed MAX_PATH
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Constructed subfolder path '%.*s\\%s' is too long (len: %zu). Skipping recursion.\n",
                        (int)current_len, current_fd_path, findFileData.cFileName, sub_path_len);
                continue;
            }

            snprintf_res = snprintf(subFolderPathActual, MAX_PATH, "%.*s\\%s", (int)current_len, current_fd_path, findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != sub_path_len) {
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to construct actual subfolder path for '%.*s\\%s' (truncation or error). Result: %d. Skipping recursion.\n",
                        (int)current_len, current_fd_path, findFileData.cFileName, snprintf_res);
                continue;
            }

            // Recursive call with the original root and its length, and the new actual path and its length.
            _SendAllFilesInFolderRecursive(root_path_to_replace, root_len,
                                          subFolderPathActual, sub_path_len);

        } else {
            // It's a file, prepare its paths for _StreamFile.

            // 1. Prepare JUST THE FILENAME for `fname` argument of _StreamFile
            char fileNameOnly[MAX_PATH]; // Will hold "MyFile.txt"
            // We already have found_filename_len from above
            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || (size_t)snprintf_res != found_filename_len) {
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to copy raw filename '%s' (truncation or error). Result: %d, Expected: %zu. Skipping this file.\n", findFileData.cFileName, snprintf_res, found_filename_len);
                continue;
            }

            // 2. Call _StreamFile with the three path components
            //    fpath argument: absoluteDirectoryPath (e.g., "C:\Folder\Subfolder\")
            //    rpath argument: relativeDirectoryPath (e.g., "Subfolder\" or "\")
            //    fname argument: fileNameOnly (e.g., "MyFile.txt")
            _StreamFile(absoluteDirectoryPath, abs_dir_path_len,
                        relativeDirectoryPath, strlen(relativeDirectoryPath), // strlen here is fine as relativeDirectoryPath is null-terminated
                        fileNameOnly, found_filename_len);

            // Print confirmation with all paths for clarity
            printf("Sent: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
                   absoluteDirectoryPath, relativeDirectoryPath, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}

// --- Public Entry Function for Directory Traversal ---
// fd_path: The path to the folder.
// len: The actual length of the fd_path string, NOT including the null terminator.
void SendAllFilesInFolderAndSubfolders(const char *fd_path, size_t len) {
    if (!fd_path) {
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path is NULL.\n");
        return;
    }

    if (len == 0) {
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path length is zero (empty path).\n");
        return;
    }

    // `MAX_PATH` usually implies space for null terminator. So `len` must be strictly less than `MAX_PATH`.
    // Also consider the `\*\0` for the search path in recursive call. If len = MAX_PATH - 3, it still fits.
    // If len = MAX_PATH - 2, then MAX_PATH - 2 + 2 = MAX_PATH. This will exactly fill `folderPathSearch` and leave no space for null.
    // So `len` must be <= MAX_PATH - 3 for `folderPathSearch` to guarantee null termination.
    if (len >= MAX_PATH - 2) { // Need at least space for "\*" and null terminator
        fprintf(stderr, "ERROR: SendAllFilesInFolderAndSubfolders - Input path length (%zu) exceeds MAX_PATH (%d) for initial root path (requires space for search wildcard and null terminator).\n", len, MAX_PATH);
        return;
    }

    // Start the recursive traversal, passing the initial fd_path as the root to replace
    _SendAllFilesInFolderRecursive(fd_path, len, fd_path, len);
}