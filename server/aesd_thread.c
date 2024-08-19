#include "aesd_thread.h"

#include <unistd.h>

#define BUFFER_SIZE 1024
#define FILE_PATH "/var/tmp/aesdsocketdata"


void *threadfunc(void *thread_param) {
    thread_data_t* pThreadData = (thread_data_t*)thread_param;
    int client_socket = pThreadData->clientFd;
    uint64_t thread_fd = pThreadData->thread;

    printf("Thread func from thread %lu, called by client socket: %d\n", thread_fd, client_socket);
    usleep(2000);
    
    pThreadData->isCompleted = 1;
    printf("Complete thread func from thread %lu, called by client socket: %d\n", thread_fd, client_socket);
    return NULL;
}