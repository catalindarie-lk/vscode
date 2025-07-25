
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
static void _StreamFile(const char *fpath, const size_t fpath_len, const char *rpath, const size_t rpath_len, const char *fname, const size_t fname_len) {

    QueueCommandEntry entry; // Represents the command to be enqueued
    int result; // For checking return values of secure functions
    memset(&entry, 0, sizeof(QueueCommandEntry));
    // --- Input Validation ---
    // Validate fpath (full actual disk directory path)
    if (!fpath) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string invalid pointer.\n"); return; }
    if (fpath_len <= 0) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string zero length.\n"); return; }
    if (fpath_len > MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path string too long (max %d chars including null). Length: %zu\n", MAX_PATH, fpath_len); return; }
    if (strnlen_s(fpath, fpath_len) == fpath_len) { fprintf(stderr, "ERROR: _StreamFile - Absolute directory path not null-terminated within %zu bytes.\n", fpath_len); return; }

    // Validate rpath (relative directory path)
    if (!rpath) { fprintf(stderr, "ERROR: _StreamFile - Relative directory path string invalid pointer.\n"); return; }
    // rpath_len MUST be at least 1 (for '\')
    if (rpath_len <= 0) { // Should not happen with correct logic from recursive function
        fprintf(stderr, "ERROR: _StreamFile - Relative directory path string zero length.\n");
        return;
    }
    // Check if the string itself (excluding null terminator) has zero length but should have at least 1.
    if (strnlen_s(rpath, rpath_len) == 0) {
        fprintf(stderr, "ERROR: _StreamFile - Relative directory path string is empty (missing backslash for root?).\n");
        return;
    }
    if (rpath_len > MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Relative directory path string too long (max %d chars including null). Length: %zu\n", MAX_PATH, rpath_len); return; }
    if (strnlen_s(rpath, rpath_len) == rpath_len) { fprintf(stderr, "ERROR: _StreamFile - Relative directory path not null-terminated within %zu bytes.\n", rpath_len); return; }

    // Validate fname (just the filename)
    if (!fname) { fprintf(stderr, "ERROR: _StreamFile - Filename string invalid pointer.\n"); return; }
    if (fname_len <= 0) { fprintf(stderr, "ERROR: _StreamFile - Filename string zero length.\n"); return; } // Filename cannot be empty
    if (fname_len > MAX_PATH) { fprintf(stderr, "ERROR: _StreamFile - Filename string too long (max %d chars including null). Length: %zu\n", MAX_PATH, fname_len); return; }
    if (strnlen_s(fname, fname_len) == fname_len) { fprintf(stderr, "ERROR: _StreamFile - Filename not null-terminated within %zu bytes.\n", fname_len); return; }


    // --- Populate QueueCommandEntry using secure functions ---

// Populate 'text' field for _StreamFile command type
    result = snprintf(entry.command.send_file.text,
                      sizeof(entry.command.send_file.text),
                      "%s", "sendfile");
    // Check for errors (negative return) or truncation (result >= buffer size)
    if (result < 0 || result >= sizeof(entry.command.send_file.text)) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to set 'text' field for send_file command (truncation or error). Result: %d, Buffer Size: %zu\n",
                result, sizeof(entry.command.send_file.text));
        return;
    }

    // Populate 'fpath' field with the FULL ACTUAL DISK DIRECTORY PATH
    result = snprintf(entry.command.send_file.fpath,
                      sizeof(entry.command.send_file.fpath),
                      "%s", fpath);
    if (result < 0 || result >= sizeof(entry.command.send_file.fpath)) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy absolute directory path to command entry (truncation or error). Path: %s, Result: %d, Buffer Size: %zu\n",
                fpath, result, sizeof(entry.command.send_file.fpath));
        return;
    }

    // Populate 'rpath' field with the RELATIVE DIRECTORY PATH
    result = snprintf(entry.command.send_file.rpath,
                      sizeof(entry.command.send_file.rpath),
                      "%s", rpath);
    if (result < 0 || result >= sizeof(entry.command.send_file.rpath)) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy relative directory path to command entry (truncation or error). Path: %s, Result: %d, Buffer Size: %zu\n",
                rpath, result, sizeof(entry.command.send_file.rpath));
        return;
    }

    // Populate 'fname' field with JUST THE FILENAME
    result = snprintf(entry.command.send_file.fname,
                      sizeof(entry.command.send_file.fname),
                      "%s", fname);
    if (result < 0 || result >= sizeof(entry.command.send_file.fname)) {
        fprintf(stderr, "ERROR: _StreamFile - Failed to copy filename to command entry (truncation or error). Name: %s, Result: %d, Buffer Size: %zu\n",
                fname, result, sizeof(entry.command.send_file.fname));
        return;
    }

    // Push the command to the queue
    push_command(&buffers.queue_fstream, &entry);
    return;
}
// --- Recursive Helper Function Definition ---
static void _SendAllFilesInFolderRecursive(const char *root_path_to_replace, const char *current_fd_path) {

    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // Result for snprintf calls

    // Construct the search path using the actual current_fd_path for OS calls
    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%s\\*", current_fd_path);
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to format search path for '%s' (truncation or error). Result: %d\n", current_fd_path, snprintf_res);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Could not open folder '%s'. System Error Code: %lu\n", current_fd_path, lastError);
        }
        return;
    }

    // Prepare the current ACTUAL disk directory path with a trailing backslash.
    // This will be passed as the 'fpath' argument to _StreamFile.
    char absoluteDirectoryPath[MAX_PATH]; // Full absolute path to current directory
    size_t current_fd_path_len_actual = strlen(current_fd_path); // Use strlen instead of strnlen_s

    snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, "%s", current_fd_path);
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to copy current actual directory path '%s' (truncation or error). Result: %d\n", current_fd_path, snprintf_res);
        FindClose(hFind); // Ensure the search handle is closed on error
        return;
    }

    // Append a trailing backslash if it's not already present.
    // Note: This logic can be simplified and made more robust using a dedicated path normalization function.
    // However, for direct conversion, keeping the original logic.
    if (current_fd_path_len_actual > 0 &&
        absoluteDirectoryPath[current_fd_path_len_actual - 1] != '\\' &&
        absoluteDirectoryPath[current_fd_path_len_actual - 1] != '/')
    {
        if (current_fd_path_len_actual < MAX_PATH - 1) {
            absoluteDirectoryPath[current_fd_path_len_actual] = '\\';
            absoluteDirectoryPath[current_fd_path_len_actual + 1] = '\0';
        } else {
            fprintf(stderr, "WARNING: _SendAllFilesInFolderRecursive - Current actual directory path '%s' is too long to add trailing backslash. Path may be incomplete.\n", absoluteDirectoryPath);
            // Decide if this is a fatal error or just a warning for your application
            // If it's fatal, you might 'FindClose(hFind); return;' here.
        }
    }


    // Prepare the RELATIVE DIRECTORY PATH (for rpath argument of _StreamFile)
    char relativeDirectoryPath[MAX_PATH]; // Will hold "relative\path\" or "\"
    const char *path_suffix_start;
    size_t root_len = strlen(root_path_to_replace); // Use strlen

    // Determine the part of current_fd_path that comes after root_path_to_replace
    // Using _strnicmp which is a Microsoft extension; for full portability use strncmp and handle case manually or use custom case-insensitive compare.
    if (strlen(current_fd_path) >= root_len &&
        _strnicmp(current_fd_path, root_path_to_replace, root_len) == 0)
    {
        path_suffix_start = current_fd_path + root_len;
        // Skip any leading path separators (e.g., if root_path was C:\ and current_fd_path is C:\Sub)
        while (*path_suffix_start == '\\' || *path_suffix_start == '/') {
            path_suffix_start++;
        }

        // If it's the root directory itself (path_suffix_start is empty), use "\"
        // Otherwise, concatenate the suffix and a trailing backslash.
        if (path_suffix_start[0] == '\0') { // Current directory IS the root
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
        } else { // Current directory is a subfolder
            snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\%s\\", path_suffix_start);
        }
    } else {
        // Fallback: This case should ideally not be hit with correct recursion logic.
        // If for some reason current_fd_path isn't under the root,
        // use a default of "\" for consistency.
        snprintf_res = snprintf(relativeDirectoryPath, MAX_PATH, "\\");
    }

    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to prepare relative directory path for '%s' (truncation or error). Result: %d. Skipping this folder.\n", current_fd_path, snprintf_res);
        FindClose(hFind);
        return;
    }


    // --- Loop through all files and directories found ---
    do {
        // Skip special directory entries: "." and ".."
        if (strcmp(findFileData.cFileName, ".") == 0 || strcmp(findFileData.cFileName, "..") == 0) {
            continue;
        }

        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // It's a subdirectory, recurse into it.
            // Construct the actual full path to the subfolder for the recursive call.
            char subFolderPathActual[MAX_PATH];

            snprintf_res = snprintf(subFolderPathActual, MAX_PATH, "%s%s", absoluteDirectoryPath, findFileData.cFileName);
            if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to construct actual subfolder path for '%s%s' (truncation or error). Result: %d. Skipping recursion.\n", absoluteDirectoryPath, findFileData.cFileName, snprintf_res);
                continue;
            }

            // Recursive call with the original root and the new actual path for traversal.
            _SendAllFilesInFolderRecursive(root_path_to_replace, subFolderPathActual);

        } else {
            // It's a file, prepare its paths for _StreamFile.

            // 1. Prepare JUST THE FILENAME for `fname` argument of _StreamFile
            char fileNameOnly[MAX_PATH]; // Will hold "MyFile.txt"
            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
                fprintf(stderr, "ERROR: _SendAllFilesInFolderRecursive - Failed to copy raw filename '%s' (truncation or error). Result: %d. Skipping this file.\n", findFileData.cFileName, snprintf_res);
                continue;
            }

            // 2. Call _StreamFile with the three path components
            //    fpath argument: absoluteDirectoryPath (e.g., "C:\Folder\Subfolder\")
            //    rpath argument: relativeDirectoryPath (e.g., "Subfolder\" or "\")
            //    fname argument: fileNameOnly (e.g., "MyFile.txt")
            _StreamFile(absoluteDirectoryPath, strlen(absoluteDirectoryPath) + 1, // Use strlen instead of strnlen_s
                     relativeDirectoryPath, strlen(relativeDirectoryPath) + 1, // Use strlen instead of strnlen_s
                     fileNameOnly, strlen(fileNameOnly) + 1); // Use strlen instead of strnlen_s

            // Print confirmation with all paths for clarity
            printf("Sent: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
                   absoluteDirectoryPath, relativeDirectoryPath, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}

// Send single file
void SendSingleFile(const char *fpath) {
    if (!fpath || strlen(fpath) == 0) {
        fprintf(stderr, "ERROR: SendSingleFile - Input file path is null or empty.\n");
        return;
    }

    char absoluteDirectoryPath[MAX_PATH]; // This will be the fpath for _StreamFile
    char fileNameOnly[MAX_PATH];          // This will be the fname for _StreamFile
    const char *relativeDirectoryPath = "\\"; // Fixed rpath for SendSingleFile

    // --- Extract Filename and Absolute Directory Path ---
    const char *last_slash_win = strrchr(fpath, '\\');
    const char *last_slash_unix = strrchr(fpath, '/');
    const char *last_slash = NULL;

    // Determine the last actual path separator (Windows-style preference first)
    if (last_slash_win && (!last_slash_unix || last_slash_win > last_slash_unix)) {
        last_slash = last_slash_win;
    } else if (last_slash_unix) {
        last_slash = last_slash_unix;
    }

    if (last_slash) {
        // Filename is after the last slash
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", last_slash + 1);
        if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
            fprintf(stderr, "ERROR: SendSingleFile - Failed to extract filename '%s' from path (truncation or error). Result: %d\n", last_slash + 1, snprintf_res);
            return;
        }

        // Absolute directory path is up to and including the last slash
        size_t dir_len = (size_t)(last_slash - fpath) + 1;
        if (dir_len >= MAX_PATH) {
            fprintf(stderr, "ERROR: SendSingleFile - Directory path derived from '%s' is too long.\n", fpath);
            return;
        }
        memcpy(absoluteDirectoryPath, fpath, dir_len);
        absoluteDirectoryPath[dir_len] = '\0';

    } else {
        // No slash found, assume the entire path is the filename and the directory is current ".\"
        int snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", fpath);
        if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
            fprintf(stderr, "ERROR: SendSingleFile - Failed to copy full path as filename (truncation or error). Result: %d\n", snprintf_res);
            return;
        }

        // Set fpath to current directory indicator
        snprintf_res = snprintf(absoluteDirectoryPath, MAX_PATH, ".\\");
        if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
            fprintf(stderr, "ERROR: SendSingleFile - Failed to set default absolute directory path (truncation or error). Result: %d\n", snprintf_res);
            return;
        }
    }

    // Call the internal _StreamFile helper
    // fpath_len, rpath_len, fname_len should include the null terminator (+1)
    _StreamFile(absoluteDirectoryPath, strlen(absoluteDirectoryPath) + 1,
             relativeDirectoryPath, strlen(relativeDirectoryPath) + 1, // rpath is fixed to "\"
             fileNameOnly, strlen(fileNameOnly) + 1);

    // printf("SendSingleFile: Processed file: %s\n", full_file_path);
}
// Send all files in a folder
void SendAllFilesInFolder(const char *fd_path) {

    WIN32_FIND_DATA findFileData;
    HANDLE hFind;

    char folderPathSearch[MAX_PATH]; // This is for FindFirstFile/FindNextFile
    int snprintf_res; // For checking return values of snprintf

    // Prepare search path: "C:\Folder\*" or "C:\Folder*"
    snprintf_res = snprintf(folderPathSearch, MAX_PATH, "%s\\*", fd_path);
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to format search path for '%s' (truncation or error). Result: %d\n", fd_path, snprintf_res);
        return;
    }

    hFind = FindFirstFile(folderPathSearch, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
            printf("Folder '%s' not found.\n", fd_path);
        } else {
            fprintf(stderr, "ERROR: SendAllFilesInFolder - Could not open folder '%s'. System Error Code: %lu\n", fd_path, lastError);
        }
        return;
    }

    // --- Prepare 'fpath' for _StreamFile ---
    // This will be the absolute directory path, ensuring a trailing backslash.
    char absoluteDirectoryPathForSend[MAX_PATH];
    size_t fd_path_len = strlen(fd_path);

    snprintf_res = snprintf(absoluteDirectoryPathForSend, MAX_PATH, "%s", fd_path);
    if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy base directory path '%s' for _StreamFile (truncation or error). Result: %d\n", fd_path, snprintf_res);
        FindClose(hFind);
        return;
    }

    // Append a trailing backslash if not already present, ensuring space
    if (fd_path_len > 0 &&
        absoluteDirectoryPathForSend[fd_path_len - 1] != '\\' &&
        absoluteDirectoryPathForSend[fd_path_len - 1] != '/')
    {
        if (fd_path_len < MAX_PATH - 1) { // -1 for backslash, another -1 for null terminator
            absoluteDirectoryPathForSend[fd_path_len] = '\\';
            absoluteDirectoryPathForSend[fd_path_len + 1] = '\0';
        } else {
            fprintf(stderr, "WARNING: SendAllFilesInFolder - Absolute directory path '%s' is too long to add trailing backslash. Path may be incomplete.\n", absoluteDirectoryPathForSend);
            // Decide how to handle this warning: fatal error or continue.
        }
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

            snprintf_res = snprintf(fileNameOnly, MAX_PATH, "%s", findFileData.cFileName);
            if (snprintf_res < 0 || snprintf_res >= MAX_PATH) {
                fprintf(stderr, "ERROR: SendAllFilesInFolder - Failed to copy filename '%s' (truncation or error). Result: %d. Skipping this file.\n", findFileData.cFileName, snprintf_res);
                continue; // Skip this file and try the next
            }

            // --- Call _StreamFile with the three path components ---
            // fpath argument: absoluteDirectoryPathForSend (e.g., "C:\Folder\")
            // rpath argument: relativeDirectoryPathForSend (e.g., "\")
            // fname argument: fileNameOnly (e.g., "MyFile.txt")
            _StreamFile(absoluteDirectoryPathForSend, strlen(absoluteDirectoryPathForSend) + 1,
                     relativeDirectoryPathForSend, strlen(relativeDirectoryPathForSend) + 1,
                     fileNameOnly, strlen(fileNameOnly) + 1);

            // Print confirmation
            printf("Send File: Abs Dir: %s; Rel Dir: %s; File Name: %s;\n",
                   absoluteDirectoryPathForSend, relativeDirectoryPathForSend, fileNameOnly);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
    return;
}
// --- Public Entry Function for Directory Traversal ---
void SendAllFilesInFolderAndSubfolders(const char *fd_path) {
    if (!fd_path || strnlen_s(fd_path, MAX_PATH) == 0) {
        fprintf(stderr, "ERROR: SendAllFilesInFolder - Input path is null or empty.\n");
        return;
    }
    // Start the recursive traversal, passing the initial fd_path as the root to replace
    _SendAllFilesInFolderRecursive(fd_path, fd_path);
}

