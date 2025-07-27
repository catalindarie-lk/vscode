#ifndef CLIENT_API_H
#define CLIENT_API_H


void SendSingleFile(const char *fpath, size_t len);
void SendAllFilesInFolder(const char *fd_path, size_t len);
void SendAllFilesInFolderAndSubfolders(const char *fd_path, size_t len);



#endif