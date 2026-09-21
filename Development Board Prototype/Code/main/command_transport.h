/* Transport-neutral command output abstraction. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "driver/uart.h"
#include "media_transfer.h"

typedef void (*command_reply_write_cb_t)(void *ctx, const char *data, size_t len);

typedef struct {
    FILE *stdio_out;
    command_reply_write_cb_t write_cb;
    void *write_cb_ctx;
    uart_port_t uart_port;
    bool use_uart;
} command_reply_t;

void command_reply_write(command_reply_t *reply, const char *data, size_t len);
void command_reply_printf(command_reply_t *reply, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
media_transfer_output_t command_media_output(command_reply_t *reply);
