#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <fcntl.h>
#include <linux/fs.h>


#define PORT "9000"
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024
#define BACKLOG 10

FILE *file = NULL;
int server_socket = -1;
int client_socket = -1;

void handle_signal(int signal) {
    syslog(LOG_INFO, "Caught signal %d, exiting", signal);
    if (client_socket != -1) {
        close(client_socket);
    }
    if (server_socket != -1) {
        close(server_socket);
    }
    if (file != NULL) {
        fclose(file);
    }
    remove(FILE_PATH);
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

int main(int argc, char *argv[]) {
    int daemon_mode = 0;

    // get options to check daemon mode
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
        printf("daemon mode.\n");
    } else {
        printf("normal mode.\n");
    }

    // Open syslog
    openlog("aesdsocket", LOG_PID, LOG_USER);
    
    // Setup signal handler
    setup_signal_handler();

    // set up addrinfo hints and node
    struct addrinfo hints, *servinfo = NULL;
    int opt = 1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // get server socket info
    if (getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) {
        syslog(LOG_ERR, "Getaddrinfo failed");
        return -1;
    }

    // Create server socker & set option
    if((server_socket = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol)) == -1) {
        syslog(LOG_ERR, "failed to create socket");
        return -1;
    }
    if(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "Setsockopt failed");
        close(server_socket);
        freeaddrinfo(servinfo);
        return -1;
    }

    // Bind socket to port
    if(bind(server_socket,servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(server_socket);
        syslog(LOG_ERR, "bind socket failed");
        return -1;
    }

    // start as daemon
    if(daemon_mode) {
        printf("Run in daemon mode:\n");
        if(execute_in_daemon()) {
            syslog(LOG_ERR, "execute in daemon mode failed");
            return -1;
        }
    }
     
    freeaddrinfo(servinfo);

    // Listen for connections
    if(listen(server_socket, BACKLOG) == -1) {
        syslog(LOG_ERR, "Listen failed");
        close(server_socket);
        return -1;
    }

    // Main loop to accept connections
    while (1) {
        struct sockaddr_storage cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&cli_addr, &clilen);
        if(client_socket == -1) {
            syslog(LOG_ERR,"Accept failed");
            continue;
        }

        char client_ip[INET6_ADDRSTRLEN];
        void *addr;
        if (cli_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&cli_addr;
            addr = &(s->sin_addr);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&cli_addr;
            addr = &(s->sin6_addr);
        }

        // Log accepted connection
        inet_ntop(cli_addr.ss_family, addr, client_ip, sizeof(client_ip));
        printf("Accepted connection from %s\n", client_ip);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Open log file
        if((file = fopen(FILE_PATH, "a+")) == NULL) {
            syslog(LOG_ERR,"File open failed");
            close(client_socket);
            continue;
        }

        char buffer[BUFFER_SIZE];
        ssize_t bytes_received = BUFFER_SIZE;
        char *data = NULL;
        size_t data_len = 0;

        while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE, 0)) > 0) {
            data = realloc(data, data_len + bytes_received);
            if(data == NULL) {
                syslog(LOG_ERR, "Memory allocation failed");
                break;
            }

            memcpy(data+data_len, buffer, bytes_received);
            data_len += bytes_received;

            if(memchr(buffer,'\n', bytes_received) != NULL) {
                fwrite(data, 1, data_len, file);
                fflush(file);
                fseek(file,0,SEEK_SET);

                while((bytes_received = fread(buffer,1,BUFFER_SIZE,file))>0) {
                    send(client_socket,buffer,bytes_received,0);
                }

                free(data);
                data = NULL;
                data_len = 0;
            }
        }

        // free resources
        if (data != NULL) {
            free(data);
            data = NULL;
        }        
        if (file != NULL) {
            fclose(file);
            file = NULL;
        }
        if (client_socket != -1) {
            close(client_socket);
            client_socket = -1;
        }
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    // Close server socket and syslog
    closelog();
    return 0;
}