#ifndef _AESD_THREAD_H_
#define _AESD_THREAD_H_

#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h> // get peer name 

#include "queue.h"

typedef struct thread_data_s thread_data_t;
struct thread_data_s{
    /* data */
    pthread_t thread; // pointer to thread itself
    pthread_mutex_t *pMutex;
    FILE* pFile;
    int clientFd;
    bool isCompleted;
};

typedef struct slist_thread_s slist_thread_t;
struct slist_thread_s {
    thread_data_t* pThreadData;
    SLIST_ENTRY(slist_thread_s) entries;
};

void *threadfunc(void *thread_param);

#endif