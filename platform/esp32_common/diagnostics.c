#include "diagnostics.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
BUILD_ASSERT(IS_ENABLED(CONFIG_NET_CONTEXT_RCVTIMEO) && IS_ENABLED(CONFIG_NET_CONTEXT_SNDTIMEO),
             "Web diagnostics require bounded socket waits");
static const char page[] =
    "<!doctype html><meta charset=utf-8><title>EAF diagnostics</title>"
    "<style>body{background:#101820;color:#e3edf5;font:16px monospace;margin:2em}"
    "pre{white-space:pre-wrap}h1{font-size:24px}</style><h1>EAF application logs</h1>"
    "<p id=s>Connecting...</p><pre id=l></pre><script>"
    "async function poll(){const abort=new AbortController();const "
    "timer=setTimeout(()=>abort.abort(),4000);try{const r=await "
    "fetch('/logs',{cache:'no-store',signal:abort.signal});"
    "if(!r.ok)throw Error(r.status);document.getElementById('l').textContent=await r.text();"
    "document.getElementById('s').textContent='Connected · '+new Date().toLocaleTimeString();}"
    "catch(e){document.getElementById('s').textContent='Disconnected — retrying';}"
    "finally{clearTimeout(timer);setTimeout(poll,2000);}}poll();</script>";
static char response[4096];
static K_THREAD_STACK_DEFINE(web_stack, 3072);
static struct k_thread web_thread;
static bool send_all(int fd, const char *data, size_t length) {
    int64_t deadline = k_uptime_get() + 500;
    while (length) {
        if (k_uptime_get() >= deadline)
            return false;
        ssize_t n = zsock_send(fd, data, length, 0);
        if (n <= 0)
            return false;
        data += (size_t)n;
        length -= (size_t)n;
    }
    return true;
}
static void serve(int fd) {
    struct zsock_timeval timeout = {.tv_sec = 0, .tv_usec = 300000};
    if (zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        board_log("Web socket setup failed errno=%d", errno);
        return;
    }
    char request[1024] = {0};
    size_t used = 0;
    int64_t deadline = k_uptime_get() + 500;
    while (used < sizeof(request) - 1u && k_uptime_get() < deadline) {
        ssize_t n = zsock_recv(fd, request + used, sizeof(request) - 1u - used, 0);
        if (n <= 0)
            return;
        used += (size_t)n;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n"))
            break;
    }
    if (!strstr(request, "\r\n\r\n"))
        return;
    const char *body;
    size_t length;
    const char *header;
    if (!strncmp(request, "GET /logs HTTP/1.", sizeof("GET /logs HTTP/1.") - 1u)) {
        body = response;
        length = board_log_read(response, sizeof(response));
        header = "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n";
    } else if (!strncmp(request, "GET / HTTP/1.", sizeof("GET / HTTP/1.") - 1u)) {
        body = page;
        length = sizeof(page) - 1u;
        header = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n";
    } else {
        body = "Not found\n";
        length = strlen(body);
        header = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n";
    }
    if (send_all(fd, header, strlen(header)) &&
        send_all(fd, "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                 sizeof("Cache-Control: no-store\r\nConnection: close\r\n\r\n") - 1u))
        (void)send_all(fd, body, length);
}
static void run(void *a, void *b, void *c) {
    (void)a;
    (void)b;
    (void)c;
    for (;;) {
        int listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in address = {
            .sin_family = AF_INET, .sin_port = htons(80), .sin_addr = {.s_addr = INADDR_ANY}};
        if (listener < 0 || zsock_bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
            zsock_listen(listener, 1)) {
            if (listener >= 0)
                (void)zsock_close(listener);
            k_sleep(K_SECONDS(5));
            continue;
        }
        board_log("Web diagnostics listening on port 80");
        for (;;) {
            int fd = zsock_accept(listener, NULL, NULL);
            if (fd < 0) {
                board_log("Web accept failed errno=%d", errno);
                break;
            }
            serve(fd);
            (void)zsock_close(fd);
        }
        (void)zsock_close(listener);
        k_sleep(K_SECONDS(1));
    }
}
void board_diagnostics_start(void) {
    (void)k_thread_create(&web_thread, web_stack, K_THREAD_STACK_SIZEOF(web_stack), run, NULL, NULL,
                          NULL, 7, 0, K_NO_WAIT);
#if defined(CONFIG_SCHED_CPU_MASK) && CONFIG_EAF_BOARD_MAIN_CPU >= 0
    (void)k_thread_cpu_pin(&web_thread, CONFIG_EAF_BOARD_MAIN_CPU);
#endif
}
