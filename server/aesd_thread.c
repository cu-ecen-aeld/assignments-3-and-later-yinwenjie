#include "aesd_thread.h"

#define INIT_BUFFER_SIZE 100

void *threadfunc(void *thread_param) {
    thread_data_t* data = (thread_data_t*)thread_param;
    int clientFd = data->clientFd;
    FILE* fd = data->pFile; // asumming this file is already open with read/write permission 
    pthread_mutex_t* pMutex = data->pMutex;
    syslog(LOG_DEBUG, "threadfunc pThread %lu for client %d started", data->thread, data->clientFd);

    char *buffer = malloc(INIT_BUFFER_SIZE);
    memset(buffer, 0, INIT_BUFFER_SIZE);
    int currentMaxSize = INIT_BUFFER_SIZE;
    int totalLen = 0;
    int readBytes = 0;

    // Recevie the newline from client
    while (!data->isCompleted) {
        readBytes = recv(clientFd, &buffer[totalLen], INIT_BUFFER_SIZE, 0);
        if (readBytes >= 0) {
            totalLen += readBytes;
            if (buffer[totalLen - 1] == '\n') {
                syslog(LOG_DEBUG, "Received full package:\n%s", buffer);
                pthread_mutex_lock(pMutex);
                fseek(fd, 0, SEEK_END);
                int ret = fwrite(buffer, 1, totalLen, fd);
                if (ret) {
                    syslog(LOG_DEBUG, "Succesfully write %d bytes to file", ret);
                }
                fseek(fd, 0, SEEK_SET);
                memset(buffer, 0, currentMaxSize);
                totalLen = 0;
                while (!feof(fd)) {
                    readBytes = fread(buffer, 1, currentMaxSize, fd);
                    syslog(LOG_DEBUG, "read %d bytes from file and sending to client", readBytes);
                    send(clientFd, buffer, readBytes, 0);
                }
                syslog(LOG_DEBUG, "unlock the pMutex");
                pthread_mutex_unlock(pMutex);
                data->isCompleted = true;
            }
            if (readBytes == 0) {
                syslog(LOG_DEBUG, "No more reading from client");
                data->isCompleted = true;
            }
            if ((totalLen + INIT_BUFFER_SIZE) > currentMaxSize) {
                currentMaxSize *= 2;
                buffer = realloc(buffer, currentMaxSize);
                if (!buffer) {
                    fprintf(stderr, "Cannot realloc memory");
                }
            }
        }
    }
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
    return NULL;
}