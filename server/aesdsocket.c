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
#include <syslog.h>
#include <pthread.h>

#include "aesd_thread.h"   // Contains threadfunc, thread_data_t definitions
#include "queue.h"

#define PORT "9000"
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define BACKLOG 10

// Comment out the next line to revert to using the file instead of /dev/aesdchar
#define USE_AESD_CHAR_DEVICE 1

#ifndef USE_AESD_CHAR_DEVICE
// Use a global FILE pointer only for the non-device version
FILE *file = NULL;
#else
// For the device version, we do NOT hold a global descriptor open.
// Instead, we open/close inside each function that needs it.
#endif

int server_socket = -1;
int client_socket = -1;

// ------------------------------------------------------
// Signal Handling
// ------------------------------------------------------
void handle_signal(int signal)
{
    syslog(LOG_INFO, "Caught signal %d, exiting", signal);

    if (client_socket != -1) {
        close(client_socket);
    }
    if (server_socket != -1) {
        close(server_socket);
    }

#ifndef USE_AESD_CHAR_DEVICE
    // Only close/remove the regular file if we are using it
    if (file) {
        fclose(file);
        file = NULL;
    }
    remove(FILE_PATH);
#else
    // In device mode, nothing special to close here 
    // because we only open/close the device locally.
#endif

    closelog();
    exit(0);
}

void setup_signal_handler()
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
}

// ------------------------------------------------------
// Daemon Setup
// ------------------------------------------------------
int execute_in_daemon()
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork process failed");
        exit(1);
    } else if (pid > 0) {
        exit(EXIT_SUCCESS);
    } else {
        // Child process
        if (setsid() == -1) {
            perror("setsid");
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------
// Timer Thread Callback
// ------------------------------------------------------
static void timer_thread(union sigval sigval)
{
    char* time_fmt = "%a, %d %b %Y %T %z";
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    if(rc != 0) {
        fprintf(stderr, "Error %d (%s) getting clock time\n", errno, strerror(errno));
        return;
    }

    char timeStr[32];
    memset(timeStr, 0, sizeof(timeStr));
    struct tm tm;

    if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
        fprintf(stderr, "Error calling gmtime_r with time %ld\n", ts.tv_sec);
        return;
    }
    if (strftime(timeStr, sizeof(timeStr), time_fmt, &tm) == 0) {
        fprintf(stderr, "strftime returned 0\n");
        exit(EXIT_FAILURE);
    }

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "timestamp:%s\n", timeStr);

    thread_data_t* td = (thread_data_t*) sigval.sival_ptr;
    if (pthread_mutex_lock(td->pMutex) != 0) {
        fprintf(stderr, "Error %d (%s) locking thread data!\n", errno, strerror(errno));
        return;
    }

    syslog(LOG_DEBUG, "Write to file: %s", buffer);

#ifndef USE_AESD_CHAR_DEVICE
    // Seek end, write to the local file
    fseek(file, 0, SEEK_END);
    fwrite(buffer, 1, strlen(buffer), file);
#else
    // Open device, seek end, write, then close
    int dev_fd = open("/dev/aesdchar", O_RDWR);
    if (dev_fd < 0) {
        perror("Error opening /dev/aesdchar");
    } else {
        // Seek to end
        if (lseek(dev_fd, 0, SEEK_END) == (off_t)-1) {
            perror("Error seeking device file");
        }
        ssize_t wr = write(dev_fd, buffer, strlen(buffer));
        if (wr < 0) {
            perror("Error writing to character device");
        }
        close(dev_fd);
    }
#endif

    if (pthread_mutex_unlock(td->pMutex) != 0) {
        fprintf(stderr, "Error %d (%s) unlocking thread data!\n", errno, strerror(errno));
    }
}

// ------------------------------------------------------
// Main
// ------------------------------------------------------
int main(int argc, char* argv[])
{
    // Parse args for -d (daemon) flag
    int daemon_mode = 0;
    int c;
    while ((c = getopt(argc, argv, "d")) != -1) {
        switch (c) {
            case 'd':
                daemon_mode = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                exit(1);
        }
    }

    if (daemon_mode) {
        // Become a daemon
        if (execute_in_daemon()) {
            syslog(LOG_ERR, "execute in daemon mode failed");
            return -1;
        }
    }

    openlog(NULL, 0, LOG_USER);
    syslog(LOG_DEBUG, "Starting aesdsocket, daemon mode: %d", daemon_mode);

    // Setup signals
    setup_signal_handler();

    // --------------------------------------------------
    // Set up socket
    // --------------------------------------------------
    struct addrinfo hints, *servinfo = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) {
        fprintf(stderr, "getaddrinfo error\n");
        exit(-1);
    }

    server_socket = socket(servinfo->ai_family, servinfo->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           servinfo->ai_protocol);
    if (server_socket < 0) {
        fprintf(stderr, "could not create socket\n");
        exit(-1);
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    if (bind(server_socket, servinfo->ai_addr, servinfo->ai_addrlen) != 0) {
        perror("bind");
        close(server_socket);
        exit(-1);
    }
    freeaddrinfo(servinfo);

    if (listen(server_socket, BACKLOG) == -1) {
        perror("listen");
        close(server_socket);
        exit(-1);
    }
    syslog(LOG_DEBUG, "Server listening on port: %s", PORT);

#ifndef USE_AESD_CHAR_DEVICE
    // For file-based mode, open the local data file once
    file = fopen(FILE_PATH, "w+");
    if (!file) {
        fprintf(stderr, "Cannot open file %s\n", FILE_PATH);
        close(server_socket);
        closelog();
        return 1;
    }
#endif

    // Create a mutex
    pthread_mutex_t mutex;
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Cannot create mutex\n");
        close(server_socket);
        closelog();
        return 1;
    }
    syslog(LOG_DEBUG, "Mutex is inited successfully");

    // --------------------------------------------------
    // Create timer for writing timestamps
    // --------------------------------------------------
    thread_data_t td;
    memset(&td, 0, sizeof(thread_data_t));
    td.pMutex = &mutex;
#ifndef USE_AESD_CHAR_DEVICE
    td.pFile = file;  // For local file usage
#else
    // For device usage, no persistent file descriptor
    td.pFile = -1;
#endif

    struct sigevent sev;
    struct itimerspec its;
    timer_t timer_id;
    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = &td;
    sev.sigev_notify_function = timer_thread;

    int clock_id = CLOCK_MONOTONIC;
    if (timer_create(clock_id, &sev, &timer_id) == -1) {
        fprintf(stderr, "Error %d (%s) creating timer!\n", errno, strerror(errno));
    } else {
        syslog(LOG_DEBUG, "Successfully set up timer, wait for timer thread to run");
    }

    // Start the timer: first expiration in 10s, then repeat every 10s
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;
    if (timer_settime(timer_id, 0, &its, NULL) == -1) {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }

    // Accept loop...
    struct sockaddr_storage clientAddr;
    socklen_t clientAddrLen = sizeof clientAddr;
    char ipstr[INET6_ADDRSTRLEN];

    // A singly-linked list for tracking threads
    SLIST_HEAD(slisthead, slist_thread_s) head;
    SLIST_INIT(&head);

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&clientAddr, &clientAddrLen);
        if (client_socket >= 0) {
            // IP info
            if (clientAddr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&clientAddr;
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
            } else {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&clientAddr;
                inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
            }
            printf("Accepted connection from %s\n", ipstr);
            syslog(LOG_DEBUG, "Accepted connection from %s", ipstr);

            // Spawn thread to handle the connection
            pthread_t thread;
            thread_data_t *data = malloc(sizeof(thread_data_t));
            if (!data) {
                fprintf(stderr, "Failed to malloc thread_data\n");
                close(client_socket);
                continue;
            }
            data->pMutex = &mutex;
            data->isCompleted = false;
#ifndef USE_AESD_CHAR_DEVICE
            data->pFile = file;
#else
            data->pFile = -1; // We'll open/close /dev/aesdchar within the thread
#endif
            data->clientFd = client_socket;

            int rc = pthread_create(&thread, NULL, threadfunc, data);
            data->thread = thread;
            if (rc != 0) {
                fprintf(stderr, "Failed to create thread for client %s\n", ipstr);
                free(data);
                close(client_socket);
            } else {
                // Insert into singly-linked list
                slist_thread_t *threadListData = malloc(sizeof(slist_thread_t));
                threadListData->pThreadData = data;
                SLIST_INSERT_HEAD(&head, threadListData, entries);
            }
        } else {
            // No new connections, wait briefly
            usleep(100000);
        }

        // Check for completed threads
        slist_thread_t *threadListData = NULL, *tempThreadListData = NULL;
        SLIST_FOREACH_SAFE(threadListData, &head, entries, tempThreadListData)
        {
            if (threadListData->pThreadData->isCompleted) {
                void *thread_rtn = NULL;
                int tryjoin_rtn = pthread_join(threadListData->pThreadData->thread, &thread_rtn);
                if (tryjoin_rtn != 0) {
                    fprintf(stderr, "Cannot join thread %lu\n", threadListData->pThreadData->thread);
                } else {
                    syslog(LOG_DEBUG, "Joined thread %lu successfully", threadListData->pThreadData->thread);
                    close(threadListData->pThreadData->clientFd);
                    SLIST_REMOVE(&head, threadListData, slist_thread_s, entries);
                    free(threadListData->pThreadData);
                    free(threadListData);
                }
            }
        }
    }

    // Cleanup
    syslog(LOG_DEBUG, "Shutting down server");
    close(server_socket);

#ifndef USE_AESD_CHAR_DEVICE
    if (file) {
        fclose(file);
        file = NULL;
        remove(FILE_PATH);
    }
#endif

    closelog();
    return 0;
}
