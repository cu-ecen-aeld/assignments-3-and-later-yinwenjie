CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -O2

TARGET = aesdsocket

SRCS = aesdsocket.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
	
