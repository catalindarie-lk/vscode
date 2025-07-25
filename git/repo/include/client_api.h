#ifndef CLIENT_API_H
#define CLIENT_API_H


void SendSingleFile(const char *fpath);
void SendAllFilesInFolder(const char *fd_path);
void SendAllFilesInFolderAndSubfolders(const char *fd_path);



#endif