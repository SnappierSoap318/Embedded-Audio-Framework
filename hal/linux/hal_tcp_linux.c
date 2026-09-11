#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* Socket declarations must precede this shared implementation. */
#include "../net/tcp_impl.h"
