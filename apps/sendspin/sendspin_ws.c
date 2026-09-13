#include <eaf/eaf_sendspin.h>
#include <string.h>

void eaf_sendspin_ws_prng_init(eaf_sendspin_ws_prng_t *prng, uint32_t seed) {
    prng->state = seed ? seed : 0x9e3779b9u;
}

uint32_t eaf_sendspin_ws_prng_next(eaf_sendspin_ws_prng_t *prng) {
    uint32_t x = prng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng->state = x;
    return x;
}

void eaf_sendspin_ws_client_key(eaf_sendspin_ws_prng_t *prng, char out[25]) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t r = eaf_sendspin_ws_prng_next(prng);
        bytes[i] = (uint8_t)r;
        bytes[i + 1] = (uint8_t)(r >> 8);
        bytes[i + 2] = (uint8_t)(r >> 16);
        bytes[i + 3] = (uint8_t)(r >> 24);
    }
    size_t used = 0;
    for (size_t i = 0; i < 15; i += 3) {
        uint32_t value = (uint32_t)bytes[i] << 16 | (uint32_t)bytes[i + 1] << 8 | bytes[i + 2];
        out[used++] = alphabet[(value >> 18) & 63u];
        out[used++] = alphabet[(value >> 12) & 63u];
        out[used++] = alphabet[(value >> 6) & 63u];
        out[used++] = alphabet[value & 63u];
    }
    uint32_t tail = (uint32_t)bytes[15] << 16;
    out[used++] = alphabet[(tail >> 18) & 63u];
    out[used++] = alphabet[(tail >> 12) & 63u];
    out[used++] = '=';
    out[used++] = '=';
    out[used] = '\0';
}

static int append(char *dst, size_t capacity, size_t *used, const char *text) {
    size_t length = strlen(text);
    if (*used + length + 1u > capacity)
        return EAF_INVALID;
    memcpy(dst + *used, text, length);
    *used += length;
    dst[*used] = '\0';
    return EAF_OK;
}

int eaf_sendspin_ws_build_upgrade(char *dst, size_t capacity, const char *host, const char *path,
                                  const char *key, size_t *written) {
    if (!dst || !host || !path || !key || !written || !capacity)
        return EAF_INVALID;
    size_t used = 0;
    dst[0] = '\0';
    if (append(dst, capacity, &used, "GET ") || append(dst, capacity, &used, path) ||
        append(dst, capacity, &used, " HTTP/1.1\r\nHost: ") || append(dst, capacity, &used, host) ||
        append(dst, capacity, &used,
               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13"
               "\r\nSec-WebSocket-Key: ") ||
        append(dst, capacity, &used, key) || append(dst, capacity, &used, "\r\n\r\n"))
        return EAF_INVALID;
    *written = used;
    return EAF_OK;
}

int eaf_sendspin_ws_check_upgrade_response(const char *response, size_t length) {
    if (!response || length < 12u || memcmp(response, "HTTP/", 5) != 0)
        return EAF_INVALID;
    const char *space = memchr(response, ' ', length);
    if (!space)
        return EAF_INVALID;
    ++space;
    if ((size_t)(response + length - space) < 3u || memcmp(space, "101", 3) != 0)
        return EAF_INVALID;
    return EAF_OK;
}

int eaf_sendspin_ws_encode(uint8_t *dst, size_t capacity, uint8_t opcode, const uint8_t *payload,
                           size_t length, uint32_t mask_key, size_t *written) {
    if (!dst || (!payload && length) || !written)
        return EAF_INVALID;
    size_t position = 2;
    if (length > 125u)
        position += length <= 0xFFFFu ? 2u : 8u;
    size_t total = position + 4u + length;
    if (total > capacity)
        return EAF_INVALID;
    dst[0] = (uint8_t)(0x80u | (opcode & 0x0Fu));
    if (length <= 125u) {
        dst[1] = (uint8_t)(0x80u | (uint8_t)length);
    } else if (length <= 0xFFFFu) {
        dst[1] = (uint8_t)(0x80u | 126u);
        dst[2] = (uint8_t)(length >> 8);
        dst[3] = (uint8_t)length;
    } else {
        dst[1] = (uint8_t)(0x80u | 127u);
        for (size_t i = 0; i < 8; ++i)
            dst[2 + i] = (uint8_t)((uint64_t)length >> (56u - 8u * i));
    }
    dst[position] = (uint8_t)mask_key;
    dst[position + 1] = (uint8_t)(mask_key >> 8);
    dst[position + 2] = (uint8_t)(mask_key >> 16);
    dst[position + 3] = (uint8_t)(mask_key >> 24);
    for (size_t i = 0; i < length; ++i)
        dst[position + 4u + i] = payload[i] ^ dst[position + (i & 3u)];
    *written = total;
    return EAF_OK;
}

void eaf_sendspin_ws_rx_init(eaf_sendspin_ws_rx_t *rx, uint8_t *message, size_t capacity) {
    *rx = (eaf_sendspin_ws_rx_t){.message = message, .message_capacity = capacity};
}

static bool is_control(uint8_t opcode) {
    return opcode == EAF_SENDPIN_WS_CLOSE || opcode == EAF_SENDPIN_WS_PING ||
           opcode == EAF_SENDPIN_WS_PONG;
}

static int begin_frame(eaf_sendspin_ws_rx_t *rx) {
    uint8_t length_code = (uint8_t)(rx->header[1] & 0x7Fu);
    size_t position = 2u;
    uint64_t payload_length = length_code;
    if (length_code == 126u) {
        payload_length = (uint64_t)rx->header[2] << 8 | rx->header[3];
        position = 4u;
    } else if (length_code == 127u) {
        payload_length = 0;
        for (size_t i = 0; i < 8; ++i)
            payload_length = payload_length << 8 | rx->header[2 + i];
        position = 10u;
    }
    if (rx->masked)
        memcpy(rx->mask, rx->header + position, 4);

    uint8_t opcode = rx->opcode;
    if (opcode == 0x0u) {
        if (!rx->fragmenting)
            return EAF_INVALID;
    } else if (opcode == EAF_SENDPIN_WS_TEXT || opcode == EAF_SENDPIN_WS_BINARY) {
        if (rx->fragmenting)
            return EAF_INVALID;
        rx->message_length = 0;
        rx->message_opcode = opcode;
    } else if (is_control(opcode)) {
        rx->control_length = 0;
    } else {
        return EAF_UNSUPPORTED;
    }
    size_t target = is_control(opcode) ? rx->control_length : rx->message_length;
    size_t room = is_control(opcode) ? sizeof(rx->control) : rx->message_capacity;
    if (payload_length > room - target)
        return EAF_UNSUPPORTED;
    rx->payload_length = payload_length;
    rx->payload_used = 0;
    return EAF_OK;
}

int eaf_sendspin_ws_rx_feed(eaf_sendspin_ws_rx_t *rx, const uint8_t *data, size_t length,
                            eaf_sendspin_ws_message_fn callback, void *ctx) {
    if (!rx || (!data && length))
        return EAF_INVALID;
    if (!rx->message && rx->message_capacity)
        return EAF_INVALID;
    while (length) {
        if (rx->header_used < 2u) {
            rx->header[rx->header_used++] = *data++;
            --length;
            if (rx->header_used < 2u)
                continue;
            uint8_t b1 = rx->header[1];
            rx->opcode = (uint8_t)(rx->header[0] & 0x0Fu);
            rx->fin = (rx->header[0] & 0x80u) != 0;
            rx->masked = (b1 & 0x80u) != 0;
            uint8_t code = (uint8_t)(b1 & 0x7Fu);
            rx->header_needed = 2u;
            if (code == 126u)
                rx->header_needed = 4u;
            else if (code == 127u)
                rx->header_needed = 10u;
            if (rx->masked)
                rx->header_needed += 4u;
            continue;
        }
        if (rx->header_used < rx->header_needed) {
            size_t take = rx->header_needed - rx->header_used;
            if (take > length)
                take = length;
            memcpy(rx->header + rx->header_used, data, take);
            rx->header_used += take;
            data += take;
            length -= take;
            if (rx->header_used < rx->header_needed)
                continue;
        }
        if (!rx->frame_ready) {
            int rc = begin_frame(rx);
            if (rc)
                return rc;
            rx->frame_ready = true;
        }
        size_t take = length;
        if ((uint64_t)take > rx->payload_length - rx->payload_used)
            take = (size_t)(rx->payload_length - rx->payload_used);
        uint8_t *target = is_control(rx->opcode) ? rx->control : rx->message;
        size_t *used = is_control(rx->opcode) ? &rx->control_length : &rx->message_length;
        if (take && !target)
            return EAF_INVALID;
        for (size_t i = 0; i < take; ++i) {
            uint8_t byte = data[i];
            if (rx->masked)
                byte ^= rx->mask[(rx->payload_used + i) & 3u];
            target[*used + i] = byte;
        }
        *used += take;
        rx->payload_used += take;
        data += take;
        length -= take;
        if (rx->payload_used == rx->payload_length) {
            if (is_control(rx->opcode)) {
                if (callback) {
                    int rc = callback(ctx, rx->opcode, rx->control, rx->control_length);
                    if (rc)
                        return rc;
                }
            } else if (rx->fin) {
                if (callback) {
                    int rc = callback(ctx, rx->message_opcode, rx->message, rx->message_length);
                    if (rc)
                        return rc;
                }
                rx->fragmenting = false;
                rx->message_length = 0;
            } else {
                rx->fragmenting = true;
            }
            rx->header_used = 0;
            rx->header_needed = 0;
            rx->payload_length = 0;
            rx->payload_used = 0;
            rx->frame_ready = false;
        }
    }
    return EAF_OK;
}
