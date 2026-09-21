/* Register summary inventory/read/delete commands. */

#pragma once

#include "command_dispatcher.h"

int summary_commands_count(void);

void summary_command_list(app_context_t *, char *, const char *, command_reply_t *);
void summary_command_get(app_context_t *, char *, const char *, command_reply_t *);
void summary_command_delete_all(app_context_t *, char *, const char *, command_reply_t *);
