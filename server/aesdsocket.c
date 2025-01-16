#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <linux/fs.h>
#include <errno.h>

#include "aesd_thread.h"
#include "queue.h"

#define PORT "9000"
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define BACKLOG 10

#define USE_AESD_CHAR_DEVICE 1

#ifndef USE_AESD_CHAR_DEVICE
FILE *file = NULL;
#else
int file;
#endif

int server_socket = -1;
int client_socket = -1;

static void timer_thread(union sigval sigval) {
    char* time_fmt = "%a, %d %b %Y %T %z";
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    if( rc != 0 ) {
        fprintf(stderr, "Error %d (%s) getting clock %d (%s) time",
                errno,strerror(errno),CLOCK_REALTIME,"CLOCK_REALTIME");
        return;
    } else {
        syslog(LOG_DEBUG, "Clock realtime %ld.%09ld",ts.tv_sec,ts.tv_nsec);
    }
    char timeStr[32];
    char buffer[128];
    memset(timeStr, 0, sizeof(timeStr));
    struct tm tm;
    if ( gmtime_r(&ts.tv_sec,&tm) == NULL ) {
        fprintf(stderr, "Error calling gmtime_r with time %ld",ts.tv_sec);
    } else  {
        if (strftime(timeStr, sizeof(timeStr), time_fmt, &tm) == 0) {
            fprintf(stderr, "strftime returned 0");
            exit(EXIT_FAILURE);
        }
    }
    thread_data_t* td = (thread_data_t*) sigval.sival_ptr;
    if ( pthread_mutex_lock(td->pMutex) != 0 ) {
        fprintf(stderr, "Error %d (%s) locking thread data!\n",errno,strerror(errno));
    } else {        
        sprintf(buffer, "timestamp:%s\n", timeStr);
        syslog(LOG_DEBUG, "Write to file: %s", buffer);
#ifndef USE_AESD_CHAR_DEVICE
        fseek(file, 0, SEEK_END); // move pointer to the end of the file
        fwrite(buffer, 1, strlen(buffer), file);
#else
        lseek(file, 0, SEEK_END); // move pointer to the end of the device
        if(write(file, buffer, strlen(buffer)) < 0) {
            perror("Error writing to character device");
        }
#endif
        if ( pthread_mutex_unlock(td->pMutex) != 0 ) {
            fprintf(stderr, "Error %d (%s) unlocking thread data!\n",errno,strerror(errno));
        }
    }
}

void handle_signal(int signal) {
    syslog(LOG_INFO, "Caught signal %d, exiting", signal);
    if (client_socket != -1) {
        close(client_socket);
    }
    if (server_socket != -1) {
        close(server_socket);
    }
#ifndef USE_AESD_CHAR_DEVICE
    if(file != NULL) {
        fclose(file);
    }
    remove(FILE_PATH);
#else
    if(file >= 0) {
        close(file);
    }
#endif
    closelog();
    exit(0);
}

void setup_signal_handler() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
}

int execute_in_daemon() {
    pid_t pid = fork();
    if(pid < 0) {
        syslog(LOG_ERR, "fork process failed");
        exit(1);
    } else if(pid > 0) {
        exit(EXIT_SUCCESS);
    } else if (pid == 0) {
        syslog(LOG_INFO, "Child process.");
        if(setsid() == -1) {
            perror("setsid");
            return 1;
        }
    }
    return 0;    
}

int main(int argc, char* argv[]) {
    int daemon_mode = 0;

    // 1.Get options to check daemon mode
    int c;
    while((c = getopt(argc, argv, "d")) != -1) {
        switch(c){
               case 'd':
                   daemon_mode = 1; 
                   break;
                default:
                   printf("Please check option\n");
                   exit(1);
        }
    }
    if (daemon_mode) {
        // start as daemon
        printf("Running in daemon mode:\n");
        if(execute_in_daemon()) {
            syslog(LOG_ERR, "execute in daemon mode failed");
            return -1;
        }
    } else {
        printf("Running in normal mode.\n");
    }

    openlog(NULL, 0, LOG_USER);
    syslog(LOG_DEBUG, "Starting aesdsocket daemon mode: %d", daemon_mode);

    // 2.Setup signal handler
    setup_signal_handler();

    int status;
    int opt = 1;
    int server_socket = -1;
    int client_socket = -1;

    // 3.Set up addrinfo hints and node
    struct addrinfo hints, *servinfo = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status)); // gai_strerror only for error code of getaddrinfo function
        exit(-1);
    }
    
    // 4.Create server socker & set option
    server_socket = socket(servinfo->ai_family, servinfo->ai_socktype|SOCK_NONBLOCK|SOCK_CLOEXEC, servinfo->ai_protocol);
    if (server_socket == 0) {
        fprintf(stderr, "could not create socket\n");
        exit (-1);
    } 
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    // 5. Bind the socket 
    if ((status = bind(server_socket, servinfo->ai_addr, servinfo->ai_addrlen)) != 0) {
        perror("bind");
        fprintf(stderr, "bind socketfd %d error %d\n", server_socket, status);
        close(server_socket);
        exit(-1);
    }
    freeaddrinfo(servinfo);

    // 6. Listen for new connection
    if ((status = listen(server_socket, BACKLOG)) == -1) {
        perror("listen");
        fprintf(stderr, "listen socketfd %d error %d\n", server_socket, status);
        close(server_socket);
        exit(-1);
    } 
    syslog(LOG_DEBUG, "server is now listening to port:%s\n", PORT);

    // 7.Open local data file
#ifndef USE_AESD_CHAR_DEVICE
    file = fopen(FILE_PATH, "w+");
    if(!file) {
        fprintf(stderr, "Cannot open file %s\n", FILE_PATH);
        close(server_socket);
        closelog();
        return 1;
    }
#else
    file = open("/dev/aesdchar", O_RDWR);
    if(file < 0) {
        perror("Cannot open character device /dev/aesdchar");
        close(server_socket);
        closelog();
        return 1;
    }
#endif
    // 8.Mutex for the file accessing
    pthread_mutex_t mutex;
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Cannot Create Mutex");
        close(server_socket);
        closelog(); // close syslog 
        return 1;
    } else {
        syslog(LOG_DEBUG, "Mutex is inited successfully");
    }

    // 9.Create timer and thread for writing timestamp
    thread_data_t td;
    memset(&td, 0, sizeof(thread_data_t));
    td.pMutex = &mutex;
    td.pFile = file;

    struct sigevent sev;
    struct itimerspec its;
    timer_t timer_id;
    int clock_id = CLOCK_MONOTONIC;
    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = &td;
    sev.sigev_notify_function = timer_thread;
    if (timer_create(clock_id, &sev, &timer_id) == -1) {
        fprintf(stderr, "Error %d (%s) creating timer!\n",errno,strerror(errno));
    } else {
        syslog(LOG_DEBUG, "Successfully setup timer, wait for timer thread to run");
    }

    // Start the timer to expire after 1 second and then every 1 second
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;
    if (timer_settime(timer_id, 0, &its, NULL) == -1) {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_storage clientAddr;
    socklen_t clientAddrLen;
    clientAddrLen = sizeof clientAddr;
    char ipstr[INET6_ADDRSTRLEN];
    SLIST_HEAD(slisthead, slist_thread_s) head;
    SLIST_INIT(&head);
    while (1) {
        // 10. Accepting the connection 
        client_socket = accept(server_socket, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (client_socket != -1) {
            getpeername(client_socket, (struct sockaddr*)&clientAddr, &clientAddrLen);
            // deal with both IPv4 and IPv6:
            if (clientAddr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&clientAddr;
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
            } else { // AF_INET6
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&clientAddr;
                inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
            };
            printf("Accepted connection from %d ipaddr: %s\n", client_socket, ipstr);
            syslog(LOG_DEBUG, "Accepted connection from %s", ipstr);
            // start the thread for the new connection 
            pthread_t thread;
            thread_data_t* data = malloc(sizeof(thread_data_t));
            if (!data) {
                fprintf(stderr, "Failed to malloc thread_data \n");
                return false;
            }
            data->pMutex = &mutex;
            data->isCompleted = false;
            data->pFile = file;
            data->clientFd = client_socket;
            int rc = pthread_create(&thread, NULL, threadfunc, data);
            data->thread = thread;
            if (rc != 0) {
                fprintf(stderr, "Failed to create thread for connection from %d ipaddr: %s\n", client_socket, ipstr);
            } else {
                // Add the data to thread list
                slist_thread_t *threaListData = malloc(sizeof(slist_thread_t));
                threaListData->pThreadData = data;
                syslog(LOG_DEBUG, "Insert new thread %lu data to list", data->thread);
                SLIST_INSERT_HEAD(&head, threaListData, entries);
            }
        } else {
            usleep(100000);
        }

        // 11. Start thread to receieve and send data
        slist_thread_t *threadListData;
        slist_thread_t *tempThreadListData;
        SLIST_FOREACH_SAFE(threadListData, &head, entries, tempThreadListData) {
            if (threadListData->pThreadData->isCompleted) {
                void * thread_rtn = NULL;
                syslog(LOG_DEBUG, "Thread %lu of client %d is completed", threadListData->pThreadData->thread, threadListData->pThreadData->clientFd);
                int tryjoin_rtn =  pthread_join(threadListData->pThreadData->thread, &thread_rtn);
                if (tryjoin_rtn != 0) {
                    fprintf(stderr, "Cannot join the thread %lu of client %d", threadListData->pThreadData->thread, threadListData->pThreadData->clientFd);
                } else {
                    syslog(LOG_DEBUG, "Joined thread %lu of client %d successfully", threadListData->pThreadData->thread, threadListData->pThreadData->clientFd);
                    close(threadListData->pThreadData->clientFd);
                    SLIST_REMOVE(&head,threadListData, slist_thread_s, entries);
                    free(threadListData->pThreadData);
                    free(threadListData);
                }
            }
        }
    }
    
    syslog(LOG_DEBUG, "close server socket");
    close(server_socket);
    if (file) {
        fclose(file);
        file = NULL;
    }
    closelog();
    return 0;
}