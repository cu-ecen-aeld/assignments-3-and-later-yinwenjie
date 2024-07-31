#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 9000
#define BACKLOG 10
#define BUFFERSIZE 1024
#define FILE_PATH "/var/tmp/aesdsocketdata"

int server_socket = -1;
int client_socket = -1;
int file_fd = -1;

void handle_signal(int signal) {
    syslog(LOG_INFO, "Caught signal %d, exiting", signal);
    if (client_socket != -1) {
        close(client_socket);
    }
    if (server_socket != -1) {
        close(server_socket);
    }
    if (file_fd != -1) {
        close(file_fd);
        remove(FILE_PATH);
    }
    closelog();
    exit(0);
}

void setup_signal_handler() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
}

int main(int argc, char** argv) {
    int daemon_mode = 0;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFERSIZE] = {0};
    ssize_t num_bytes;

    /* get option */
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

    // Create socket 
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        return -1;
    } else {
        printf("Create socket succeeded.\n");
    }
    
    // Bind socket to port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
        close(server_socket);
        return -1;
    } else {
        printf("Bind socket succeeded.\n");
    }
    
    // Listen for connections
    if (listen(server_socket, BACKLOG) == -1) {
        syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
        close(server_socket);
        return -1;
    } else {
        printf("Listen on socket succeeded.\n");
    }
    
    // Main loop to accept connections
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }
        
        // Log accepted connection
        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));
        printf("Accepted connection from %s\n", inet_ntoa(client_addr.sin_addr));

        // Open file for writing
        file_fd = open(FILE_PATH, O_RDWR | O_CREAT | O_APPEND, 0666);
        if (file_fd == -1) {
            syslog(LOG_ERR, "Failed to open file: %s", strerror(errno));
            close(client_socket);
            continue;
        } else {
            printf("Open file %s succeeded\n", FILE_PATH);
        }

        // Receive data
        while ((num_bytes = recv(client_socket, buffer, BUFFERSIZE, 0)) > 0) {
            if (write(file_fd, buffer, num_bytes) != num_bytes) {
                syslog(LOG_ERR, "Failed to write to file: %s", strerror(errno));
                break;
            } else {
                printf("Buffer content received: %s, bytes: %ld\n", buffer, num_bytes);
            }
        }
        
        // Send file contents to client
        lseek(file_fd, 0, SEEK_SET);
        while ((num_bytes = read(file_fd, buffer, BUFFERSIZE)) > 0) {
            if (send(client_socket, buffer, num_bytes, 0) != num_bytes) {
                syslog(LOG_ERR, "Failed to send data to client: %s", strerror(errno));
                break;
            } else {
                printf("Buffer content sent: %s, len:%ld\n", buffer, strlen(buffer));
            }
        }

        // Close file and client socket
        close(file_fd);
        file_fd = -1;
        close(client_socket);
        client_socket = -1;

        // Log closed connection
        syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_addr.sin_addr));
        printf("Closed connection from %s\n", inet_ntoa(client_addr.sin_addr));
    }    

    // Close server socket and syslog
    closelog();
    return 0;
}