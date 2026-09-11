#include <zephyr/net/socket.h>
#define F_GETFL ZVFS_F_GETFL
#define F_SETFL ZVFS_F_SETFL
#define O_NONBLOCK ZVFS_O_NONBLOCK
#define POLLOUT ZSOCK_POLLOUT
/* Zephyr sockets do not raise POSIX SIGPIPE. */
#define MSG_NOSIGNAL 0
#define socket zsock_socket
#define connect zsock_connect
#define fcntl zsock_fcntl
#define poll zsock_poll
#define pollfd zsock_pollfd
#define getsockopt zsock_getsockopt
#define send zsock_send
#define recv zsock_recv
#define close zsock_close
#include "../net/tcp_impl.h"
