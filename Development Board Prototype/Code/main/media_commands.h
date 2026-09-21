/* Register media retrieval and deletion commands. */

#pragma once

#include "command_dispatcher.h"

void media_command_delete_all(app_context_t *, char *, const char *, command_reply_t *);
void media_command_pause(app_context_t *, char *, const char *, command_reply_t *);
void media_command_resume(app_context_t *, char *, const char *, command_reply_t *);
void media_command_formats(app_context_t *, char *, const char *, command_reply_t *);
void media_command_count(app_context_t *, char *, const char *, command_reply_t *);
void media_command_list(app_context_t *, char *, const char *, command_reply_t *);
void media_command_get(app_context_t *, char *, const char *, command_reply_t *);
void media_command_get_base64(app_context_t *, char *, const char *, command_reply_t *);
void media_command_get_binary(app_context_t *, char *, const char *, command_reply_t *);
