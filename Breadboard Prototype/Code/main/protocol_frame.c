/* Wire-format integrity primitives shared by framed media responses. */

#include "protocol_frame.h"

#include <stdio.h>

uint16_t protocol_frame_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    while (len-- > 0) {
        crc ^= (uint16_t)*data++ << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t protocol_frame_encode_hex(uint32_t sequence,
                                 const uint8_t *data,
                                 size_t len,
                                 char *output,
                                 size_t output_size)
{
    if ((data == NULL && len > 0) || output == NULL) return 0;
    /* Prefix, CRC, payload expansion, newline, and NUL terminator. */
    if (output_size < len * 2U + 32U) return 0;

    uint16_t crc = protocol_frame_crc16_ccitt(data, len);
    int prefix_len = snprintf(output, output_size, "DATA %u %u %04X ",
                              (unsigned)sequence, (unsigned)len, (unsigned)crc);
    if (prefix_len <= 0 || (size_t)prefix_len >= output_size) return 0;

    static const char hex[] = "0123456789ABCDEF";
    size_t cursor = (size_t)prefix_len;
    for (size_t i = 0; i < len; ++i) {
        output[cursor++] = hex[data[i] >> 4];
        output[cursor++] = hex[data[i] & 0x0fU];
    }
    output[cursor++] = '\n';
    output[cursor] = '\0';
    return cursor;
}
