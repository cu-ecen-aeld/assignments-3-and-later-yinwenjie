#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);
    if (argc < 3) {
	syslog(LOG_ERR, "Insufficient arguments\n");
	return 1;
    }
    const char* file_path = argv[1];
    const char* file_cont = argv[2];
    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (-1 == fd) {
	syslog(LOG_ERR, "create file failed.\n");
	return 1;
    } else {
	syslog(LOG_DEBUG, "file created.\n");
    }

    ssize_t bytes = write(fd, file_cont, strlen(file_cont));
    if (-1 == bytes) {
	syslog(LOG_ERR, "Failed to write content :%s: %s", file_cont, strerror(errno));
	return 1;
    } else {
	syslog(LOG_DEBUG, "Writing %s to %s", file_cont, file_path);
    }

    if(close(fd) == -1) {
	syslog(LOG_ERR, "close file failed.\n");
	return 1;
    } else {
	syslog(LOG_DEBUG, "file closed.\n");
    }
    closelog();
}
