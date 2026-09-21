/* CRC and binary frame constants for host/device interoperability. */

#pragma once

#include <stddef.h>
#include <stdint.h>

uint16_t protocol_frame_crc16_ccitt(const uint8_t *data, size_t len);

/* Formats one V2 hexadecimal payload line, including its trailing newline.
 * Returns the number of bytes written, or 0 if the output buffer is too small. */
size_t protocol_frame_encode_hex(uint32_t sequence,
                                 const uint8_t *data,
                                 size_t len,
                                 char *output,
                                 size_t output_size);
