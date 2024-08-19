#ifndef _AESD_THREAD_H
#define _AESD_THREAD_H
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <syslog.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "queue.h"

#define FILE_PATH "/var/tmp/aesdsocketdata"

typedef struct thread_data_s thread_data_t;
struct thread_data_s {
    pthread_t thread;
    pthread_mutex_t* pMutex;
    FILE* pFile;
    int clientFd;
    int isCompleted;
};

typedef struct slist_thread_s slist_thread_t;
struct slist_thread_s {
    thread_data_t* pThreadData;
    SLIST_ENTRY(slist_thread_s) entries;
};

void *threadfunc(void *thread_param);

#endif