/* Included by one OS translation unit after its socket API mappings. */
#include <eaf/eaf_net.h>
#include <errno.h>
#include <limits.h>
int hal_tcp_connect(eaf_tcp_t *tcp, uint32_t ipv4, uint16_t port, uint32_t timeout_ms) {
    if (!tcp || tcp->open || !port || timeout_ms > INT_MAX)
        return EAF_INVALID;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return EAF_IO;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return EAF_IO;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    addr.sin_addr.s_addr = htonl(ipv4);
    int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc && errno != EINPROGRESS) {
        close(fd);
        return EAF_IO;
    }
    if (rc) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        rc = poll(&pfd, 1, (int)timeout_ms);
        int error = 0;
        socklen_t size = sizeof(error);
        if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
            close(fd);
            return EAF_IO;
        }
    }
    *tcp = (eaf_tcp_t){fd, true};
    return EAF_OK;
}
int hal_tcp_send(eaf_tcp_t *tcp, const void *data, size_t bytes, size_t *sent) {
    if (!tcp || !tcp->open || !data || !sent)
        return EAF_INVALID;
    *sent = 0;
    ssize_t n = send(tcp->fd, data, bytes, MSG_NOSIGNAL);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? EAF_AGAIN : EAF_IO;
    if (!n && bytes)
        return EAF_IO;
    *sent = (size_t)n;
    return EAF_OK;
}
int hal_tcp_recv(eaf_tcp_t *tcp, void *data, size_t capacity, size_t *received) {
    if (!tcp || !tcp->open || !data || !capacity || !received)
        return EAF_INVALID;
    *received = 0;
    ssize_t n = recv(tcp->fd, data, capacity, 0);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? EAF_AGAIN : EAF_IO;
    if (!n)
        return EAF_EOF;
    *received = (size_t)n;
    return EAF_OK;
}
void hal_tcp_close(eaf_tcp_t *tcp) {
    if (tcp && tcp->open) {
        close(tcp->fd);
        tcp->open = false;
    }
}
