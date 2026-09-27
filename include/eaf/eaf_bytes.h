#pragma once
#include <stddef.h>
#include <stdint.h>

static inline uint16_t eaf_bytes_read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t eaf_bytes_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint64_t eaf_bytes_read_u64(const uint8_t *p) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = value << 8 | p[i];
    return value;
}

static inline void eaf_bytes_write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline void eaf_bytes_write_u32(uint8_t *p, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> ((3u - i) * 8u));
}

static inline void eaf_bytes_write_u64(uint8_t *p, uint64_t value) {
    for (size_t i = 0; i < 8; ++i)
        p[i] = (uint8_t)(value >> (56u - 8u * i));
}

static inline size_t eaf_bytes_write_u64_dec(char *dst, size_t capacity, uint64_t value) {
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while (value);
    if (n > capacity)
        return 0;
    for (size_t i = 0; i < n; ++i)
        dst[i] = digits[n - 1u - i];
    return n;
}
