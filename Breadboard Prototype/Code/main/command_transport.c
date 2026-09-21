/* Route command replies to stdio, UART, BLE, or media-transfer writers. */

#include "command_transport.h"

#include <stdarg.h>
#include <stdio.h>

void command_reply_write(command_reply_t *reply, const char *data, size_t len)
{
    if (reply != NULL && reply->write_cb != NULL) {
        reply->write_cb(reply->write_cb_ctx, data, len);
        return;
    }
    if (reply != NULL && reply->use_uart) {
        uart_write_bytes(reply->uart_port, data, len);
        return;
    }
    FILE *out = (reply != NULL && reply->stdio_out != NULL) ? reply->stdio_out : stdout;
    fwrite(data, 1, len, out);
    fflush(out);
}

void command_reply_printf(command_reply_t *reply, const char *fmt, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }
    size_t output_len = (size_t)len;
    if (output_len >= sizeof(buffer)) {
        output_len = sizeof(buffer) - 1;
    }
    command_reply_write(reply, buffer, output_len);
}

static void command_media_write(void *ctx, const char *data, size_t len)
{
    command_reply_write((command_reply_t *)ctx, data, len);
}

media_transfer_output_t command_media_output(command_reply_t *reply)
{
    return (media_transfer_output_t) {
        .write = command_media_write,
        .ctx = reply,
    };
}
