/* Command callback table and dispatch contract shared by USB and BLE. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "command_transport.h"

typedef struct app_context app_context_t;

typedef void (*command_handler_t)(
    app_context_t *ctx,
    char *command,
    const char *args,
    command_reply_t *reply
);

typedef struct {
    const char *name;
    bool accepts_args;
    command_handler_t handler;
} command_entry_t;

bool command_dispatch_table(
    const command_entry_t *entries,
    size_t entry_count,
    app_context_t *ctx,
    char *command,
    command_reply_t *reply
);
