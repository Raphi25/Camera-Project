/* Register diagnostic commands with the common dispatcher. */

#pragma once

#include "command_dispatcher.h"

void diagnostic_command_health(app_context_t *, char *, const char *, command_reply_t *);
void diagnostic_command_task_stats(app_context_t *, char *, const char *, command_reply_t *);
