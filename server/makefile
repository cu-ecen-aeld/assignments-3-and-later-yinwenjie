CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -Wextra -O2

TARGET = aesdsocket

SRCS = aesdsocket.c aesd_thread.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
	
