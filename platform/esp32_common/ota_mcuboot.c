#include "diagnostics.h"
#include "ota.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>

#define OTA_CHUNK 1024u
#define OTA_RECV_TIMEOUT_MS 5000

static struct flash_img_context flash_ctx;
static atomic_t ota_reboot_flag;

bool ota_available(void) {
    return true;
}

bool ota_reboot_requested(void) {
    return atomic_get(&ota_reboot_flag) != 0;
}

static bool send_all(int fd, const char *data, size_t length) {
    int64_t deadline = k_uptime_get() + 1000;
    while (length) {
        if (k_uptime_get() >= deadline)
            return false;
        ssize_t sent = zsock_send(fd, data, length, 0);
        if (sent <= 0)
            return false;
        data += (size_t)sent;
        length -= (size_t)sent;
    }
    return true;
}

static void respond(int fd, const char *status, const char *body) {
    char header[128];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.0 %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     status, (unsigned)strlen(body));
    if (n > 0)
        (void)send_all(fd, header, (size_t)n);
    (void)send_all(fd, body, strlen(body));
}

static bool header_has_prefix(const char *line, const char *prefix) {
    while (*prefix) {
        char a = *line++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool find_content_length(const char *request, size_t *length) {
    const char *p = request;
    for (; *p; ++p) {
        if (header_has_prefix(p, "\nContent-Length:")) {
            const char *value = p + sizeof("\nContent-Length:") - 1u;
            while (*value == ' ')
                ++value;
            *length = (size_t)strtoul(value, NULL, 10);
            return *length > 0u;
        }
    }
    return false;
}

bool ota_handle_request(int fd, const char *request, size_t used) {
    size_t content_length = 0;
    if (!find_content_length(request, &content_length)) {
        respond(fd, "411 Length Required", "missing Content-Length\n");
        return true;
    }
    const char *header_end = strstr(request, "\r\n\r\n");
    if (!header_end) {
        respond(fd, "400 Bad Request", "bad request\n");
        return true;
    }
    size_t body_in_buffer = used - (size_t)(header_end + 4 - request);
    if (body_in_buffer > content_length)
        body_in_buffer = content_length;

    struct zsock_timeval timeout = {.tv_sec = OTA_RECV_TIMEOUT_MS / 1000u,
                                    .tv_usec = (OTA_RECV_TIMEOUT_MS % 1000u) * 1000};
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (flash_img_init(&flash_ctx)) {
        board_log("OTA: flash image init failed");
        respond(fd, "500 Internal Error", "flash init failed\n");
        return true;
    }
    board_log("OTA: receiving %u bytes", (unsigned)content_length);

    size_t received = 0;
    if (body_in_buffer) {
        if (flash_img_buffered_write(&flash_ctx, (const uint8_t *)(header_end + 4), body_in_buffer,
                                     false)) {
            respond(fd, "500 Internal Error", "flash write failed\n");
            return true;
        }
        received = body_in_buffer;
    }
    uint8_t buffer[OTA_CHUNK];
    while (received < content_length) {
        size_t want = content_length - received;
        if (want > sizeof(buffer))
            want = sizeof(buffer);
        ssize_t n = zsock_recv(fd, buffer, want, 0);
        if (n <= 0) {
            board_log("OTA: short read at %u/%u", (unsigned)received, (unsigned)content_length);
            respond(fd, "500 Internal Error", "short read\n");
            return true;
        }
        if (flash_img_buffered_write(&flash_ctx, buffer, (size_t)n, false)) {
            respond(fd, "500 Internal Error", "flash write failed\n");
            return true;
        }
        received += (size_t)n;
    }
    if (flash_img_buffered_write(&flash_ctx, buffer, 0u, true)) {
        respond(fd, "500 Internal Error", "flash flush failed\n");
        return true;
    }
    if (boot_request_upgrade(BOOT_UPGRADE_PERMANENT)) {
        respond(fd, "500 Internal Error", "upgrade request failed\n");
        return true;
    }
    board_log("OTA: image written, reboot pending");
    respond(fd, "200 OK", "ok, rebooting\n");
    atomic_set(&ota_reboot_flag, 1);
    return true;
}
