#include <errno.h>
#include <lwip/sockets.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* lwIP enables Nagle by default; the audio transport needs low latency, so the
   shared implementation's connect is routed through a wrapper that disables it
   before the connection completes. */
static int tcp_connect_nodelay(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    int on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return connect(fd, addr, addrlen);
}

#undef connect
#define connect tcp_connect_nodelay

/* Socket declarations must precede this shared implementation. */
#include "../net/tcp_impl.h"
