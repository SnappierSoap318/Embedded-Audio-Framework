#pragma once
#include <eaf/eaf_types.h>
#define EAF_AGAIN (-5)
typedef struct {
    int fd;
    bool open;
} eaf_tcp_t;
/* Numeric IPv4 in host byte order. All operations belong to a transport task,
   never the deadline-driven audio owner. Timeouts are milliseconds. */
int hal_tcp_connect(eaf_tcp_t *tcp, uint32_t ipv4, uint16_t port, uint32_t timeout_ms);
int hal_tcp_send(eaf_tcp_t *tcp, const void *data, size_t bytes, size_t *sent);
int hal_tcp_recv(eaf_tcp_t *tcp, void *data, size_t capacity, size_t *received);
void hal_tcp_close(eaf_tcp_t *tcp);
