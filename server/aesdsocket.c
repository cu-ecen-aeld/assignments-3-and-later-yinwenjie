#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#define USE_AESD_CHAR_DEVICE 1

#ifdef USE_AESD_CHAR_DEVICE
// Character device path
const char *filepath = "/dev/aesdchar";
#else
// Regular file path
const char *filepath = "/var/tmp/aesdsocketdata";
#endif

int sockfd;

#ifndef USE_AESD_CHAR_DEVICE
FILE *file = NULL;
#else
int file = -1;
#endif

pthread_mutex_t file_mutex;

void *handle_client(void *ptr);
void signalInterruptHandler(int signo);
int createTCPServer(int deamonize);

// A simple linked list of threads
typedef struct node {
    pthread_t tid;
    struct node *next;
} Node;

typedef struct {
    int client_sockfd;
    struct sockaddr_in client_addr;
} client_info_t;

// ---------------------------------------------------------------------------
// Thread function to handle client connection
// ---------------------------------------------------------------------------
void *handle_client(void *ptr)
{
    client_info_t *client_info = (client_info_t *)ptr;
    int client_sockfd = client_info->client_sockfd;
    struct sockaddr_in client_addr = client_info->client_addr;
    free(client_info);

    socklen_t client_len = sizeof(client_addr);
    char client_ip[INET6_ADDRSTRLEN];

    // Get client IP for logging
    if(getpeername(client_sockfd, (struct sockaddr *)&client_addr, &client_len) == 0)
    {
        if(client_addr.sin_family == AF_INET)
        {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &(ipv4->sin_addr), client_ip, INET6_ADDRSTRLEN);
        }
        else if(client_addr.sin_family == AF_INET6)
        {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &(ipv6->sin6_addr), client_ip, INET6_ADDRSTRLEN);
        }
    }
    else
    {
        perror("Unable to get client IP address");
        close(client_sockfd);
        return NULL;
    }

    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    char *buffer = NULL;
    ssize_t num_bytes = 0;
    ssize_t recv_bytes = 0;

    while(1)
    {
        int connection_closed = 0;
        int received_error = 0;

        // Keep expanding `buffer` until we see a newline
        do {
            // Reallocate for the next chunk
            buffer = realloc(buffer, num_bytes + 1024);
            if (!buffer) {
                syslog(LOG_ERR, "Unable to allocate space on heap");
                close(client_sockfd);
                return NULL;
            }

            recv_bytes = recv(client_sockfd, buffer + num_bytes, 1024, 0);
            if(recv_bytes == 0)
            {
                // Client closed connection
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                connection_closed = 1;
                break;
            }
            else if(recv_bytes < 0)
            {
                // Error receiving
                if(errno == EAGAIN || errno == EINTR) {
                    // Non-fatal, retry
                    continue;
                }
                syslog(LOG_ERR, "Received error from %s", client_ip);
                perror("recv error");
                received_error = 1;
                break;
            }

            num_bytes += recv_bytes;
        } while (!memchr(buffer + num_bytes - recv_bytes, '\n', recv_bytes));
        // ^ Continue until we find a newline in the last received chunk

        if(received_error == 1 || connection_closed == 1)
        {
            free(buffer);
            buffer = NULL;
            num_bytes = 0;
            break;
        }
        else
        {
            // We have at least one newline in `buffer`
            buffer[num_bytes] = '\0'; // null-terminate for safety

            // Write incoming data, then read entire file/device back to client
            pthread_mutex_lock(&file_mutex);

#ifndef USE_AESD_CHAR_DEVICE
            // -------------------------------------------------
            // Regular file usage
            // -------------------------------------------------
            if(!file) {
                // Should not happen if opened in main, but just in case
                file = fopen(filepath, "a+");
                if(!file) {
                    perror("Unable to open file");
                    pthread_mutex_unlock(&file_mutex);
                    break;
                }
            }
            // Write the data
            fputs(buffer, file);
            fflush(file);                       // *** CHANGED ***
            fseek(file, 0, SEEK_SET);           // *** CHANGED ***

            // Now read entire file and send back
            size_t bufferSize = 1024;
            char *writeBuf = malloc(bufferSize);
            if(!writeBuf) {
                perror("Unable to allocate memory for writeBuf");
                pthread_mutex_unlock(&file_mutex);
                break;
            }

            while(fgets(writeBuf, bufferSize, file) != NULL)
            {
                send(client_sockfd, writeBuf, strlen(writeBuf), 0);
            }
            free(writeBuf);

            // Optionally keep file open or re-close each time
            // We'll keep it open for the life of the server

#else
            // -------------------------------------------------
            // Character device usage
            // -------------------------------------------------
            // 1) Open in append mode for writing
            file = open(filepath, O_RDWR | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if(file < 0) {
                perror("Unable to open /dev/aesdchar");
                pthread_mutex_unlock(&file_mutex);
                break;
            }

            // 2) Write the data
            ssize_t written = write(file, buffer, strlen(buffer));
            if(written < 0) {
                perror("Error writing to /dev/aesdchar");
                close(file);
                pthread_mutex_unlock(&file_mutex);
                break;
            }

            // 3) Rewind to start for reading
            lseek(file, 0, SEEK_SET);

            // 4) Read everything, send to client
            size_t bufferSize = 1024;
            char* writeBuf = malloc(bufferSize);
            if(!writeBuf) {
                perror("Unable to allocate memory for writeBuf");
                close(file);
                pthread_mutex_unlock(&file_mutex);
                break;
            }
            ssize_t bytesRead;
            while ((bytesRead = read(file, writeBuf, bufferSize)) > 0) {
                send(client_sockfd, writeBuf, bytesRead, 0);
            }
            free(writeBuf);

            // 5) Close device
            close(file);
            file = -1;
#endif

            pthread_mutex_unlock(&file_mutex);

            // Reset for next message
            free(buffer);
            buffer = NULL;
            num_bytes = 0;
        }
    }

    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    close(client_sockfd);
    return NULL;
}

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------
void signalInterruptHandler(int signo)
{
    if((signo == SIGTERM) || (signo == SIGINT))
    {
#ifndef USE_AESD_CHAR_DEVICE
        // Clean up file usage
        if(file) {
            fclose(file);
            file = NULL;
        }
        int Status = remove("/var/tmp/aesdsocketdata");
        if(Status == 0)
        {
            printf("Successfully deleted file /var/tmp/aesdsocketdata\n");
        }
        else
        {
            printf("Unable to delete file at path /var/tmp/aesdsocketdata\n");
        }
#endif
        printf("Gracefully handling SIGTERM/SIGINT\n");
        syslog(LOG_INFO,  "Caught signal, exiting");

        if(sockfd >= 0) {
            close(sockfd);
        }

#ifndef USE_AESD_CHAR_DEVICE
        // Already closed above
#else
        // For device: if it was open, it's closed in handle_client
        // so we don't hold it open globally.
#endif

        pthread_mutex_destroy(&file_mutex);
        closelog();
        exit(EXIT_SUCCESS);
    }

    if(signo == SIGALRM)
    {
#ifndef USE_AESD_CHAR_DEVICE
        // Example: write a timestamp to the file
        struct timeval tv;
        struct tm ptm;
        char time_string[40];
        long milliseconds;

        // Get the current time
        gettimeofday(&tv, NULL);

        // Format the date/time
        localtime_r(&tv.tv_sec, &ptm);
        strftime(time_string, sizeof(time_string), "%Y-%m-%d %H:%M:%S", &ptm);
        milliseconds = tv.tv_usec;

        alarm(10); // schedule next alarm

        pthread_mutex_lock(&file_mutex);
        if(file) {
            // Write a timestamp line
            fprintf(file, "timestamp:%s.%06ld\n", time_string, milliseconds);
            fflush(file);
        }
        pthread_mutex_unlock(&file_mutex);
#endif
    }
}

// ---------------------------------------------------------------------------
// Create TCP server
// ---------------------------------------------------------------------------
int createTCPServer(int deamonize)
{
    signal(SIGINT, signalInterruptHandler);
    signal(SIGTERM, signalInterruptHandler);
    signal(SIGALRM, signalInterruptHandler);

#ifndef USE_AESD_CHAR_DEVICE
    // Open or create the file once for append/read
    file = fopen(filepath, "a+");
    if(!file)
    {
        perror("Unable to open or create the file");
        return -1;
    }
#else
    // In device mode, we open/close around each read/write, so do nothing here
#endif

    openlog("aesdsocket.c", LOG_CONS | LOG_PID, LOG_USER);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd == -1)
    {
        syslog(LOG_ERR, "Unable to create TCP Socket");
#ifndef USE_AESD_CHAR_DEVICE
        fclose(file);
        file = NULL;
#endif
        closelog();
        return -1;
    }

    int enable = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        syslog(LOG_ERR, "setsockopt(SO_REUSEADDR) failed");
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    struct sockaddr_in addr;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);

    if(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        syslog(LOG_ERR, "TCP Socket bind failure");
        perror("TCP Socket bind failure");
        close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
        fclose(file);
        file = NULL;
#endif
        closelog();
        return -1;
    }

    if(listen(sockfd, 5) == -1)
    {
        syslog(LOG_ERR, "Unable to listen at created TCP socket");
        perror("Unable to listen at created TCP socket");
        close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
        fclose(file);
        file = NULL;
#endif
        closelog();
        return -1;
    }

    // Deamonize if specified
    if(deamonize == 1)
    {
        pid_t pid = fork();
        if(pid < 0)
        {
            printf("failed to fork\n");
            close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
            fclose(file);
            file = NULL;
#endif
            closelog();
            exit(EXIT_FAILURE);
        }

        if(pid > 0 )
        {
            // Parent
            printf("Running as a daemon\n");
            exit(EXIT_SUCCESS);
        }

        umask(0);

        if(setsid() < 0)
        {
            printf("Failed to create SID for child\n");
            close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
            fclose(file);
            file = NULL;
#endif
            closelog();
            exit(EXIT_FAILURE);
        }

        if(chdir("/") < 0)
        {
            printf("Unable to change directory to root\n");
            close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
            fclose(file);
            file = NULL;
#endif
            closelog();
            exit(EXIT_FAILURE);
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    alarm(10);
    syslog(LOG_INFO, "TCP server listening at port %d", ntohs(addr.sin_port));

    if(pthread_mutex_init(&file_mutex, NULL) != 0)
    {
        syslog(LOG_ERR, "Cannot Create Mutex");
        close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
        fclose(file);
        file = NULL;
#endif
        closelog();
        return -1;
    }

    int num_threads = 0;
    Node *head = NULL;

    while(1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
        if(client_sockfd == -1)
        {
            if(errno == EINTR) {
                // Possibly interrupted by signal, check again
                continue;
            }
            syslog(LOG_ERR, "Unable to accept the client's connection");
            perror("Unable to accept the client's connection");
#ifndef USE_AESD_CHAR_DEVICE
            fclose(file);
            file = NULL;
#endif
            closelog();
            return -1;
        }

        // Allocate and populate client_info for thread
        client_info_t *client_info = malloc(sizeof(client_info_t));
        if(!client_info)
        {
            syslog(LOG_ERR, "Unable to allocate memory for client_info");
            perror("Unable to allocate memory for client_info");
            close(client_sockfd);
            continue;
        }
        client_info->client_sockfd = client_sockfd;
        client_info->client_addr = client_addr;

        Node *n = malloc(sizeof(Node));
        if(!n)
        {
            syslog(LOG_ERR, "Failed to allocate memory for thread node");
            perror("Failed to allocate memory for thread node");
            close(client_sockfd);
            free(client_info);
            continue;
        }

        if(pthread_create(&(n->tid), NULL, handle_client, (void *)client_info) != 0)
        {
            syslog(LOG_ERR, "Unable to create thread");
            perror("Unable to create thread");
            close(client_sockfd);
            free(client_info);
            free(n);
            continue;
        }

        n->next = head;
        head = n;
        num_threads++;
    }

    // Cleanup threads
    Node *current = head;
    while(current != NULL)
    {
        if(pthread_join(current->tid, NULL) != 0) {
            fprintf(stderr, "Failed to join thread \n");
        }
        current = current->next;
    }

    // Free the linked list
    current = head;
    while(current != NULL)
    {
        Node *next = current->next;
        free(current);
        current = next;
    }

    close(sockfd);
#ifndef USE_AESD_CHAR_DEVICE
    if(file) {
        fclose(file);
        file = NULL;
        remove(filepath);
    }
#endif
    pthread_mutex_destroy(&file_mutex);
    closelog();
    return 0;
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int deamonize = 0;

    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-d") == 0)
        {
            deamonize = 1;
        }
    }

    if(createTCPServer(deamonize) == -1) {
        printf("Error in running application\n");
    }

    return 0;
}
