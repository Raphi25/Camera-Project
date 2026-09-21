/* Adapt IMG_* textual commands to the transport-neutral media service. */

#include "media_commands.h"

#include <stdio.h>

#include "camera.h"
#include "media_transfer.h"

void media_command_delete_all(app_context_t *ctx, char *cmd, const char *args,
                              command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_delete_all(&output);
}

void media_command_pause(app_context_t *ctx, char *cmd, const char *args,
                         command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_pause(&output);
}

void media_command_resume(app_context_t *ctx, char *cmd, const char *args,
                          command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_resume(&output);
}

void media_command_formats(app_context_t *ctx, char *cmd, const char *args,
                           command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args; (void)reply;
    (void)camera_print_v4l2_formats();
}

void media_command_count(app_context_t *ctx, char *cmd, const char *args,
                         command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_count(&output);
}

void media_command_list(app_context_t *ctx, char *cmd, const char *args,
                        command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_list(&output);
}

static void media_command_get_encoded(const char *args, bool base64, bool binary,
                                      command_reply_t *reply)
{
    if (args == NULL || *args == '\0') {
        command_reply_printf(reply, "ERR IMG_GET invalid_name\n");
        return;
    }
    char file_name[256];
    unsigned offset = 0;
    char extra = '\0';
    int fields = sscanf(args, "%255s %u %c", file_name, &offset, &extra);
    if (fields < 1 || fields > 2) {
        command_reply_printf(reply, "ERR IMG_GET invalid_offset\n");
        return;
    }
    media_transfer_output_t output = command_media_output(reply);
    media_transfer_command_get(file_name, (size_t)offset, base64, binary, &output);
}

void media_command_get(app_context_t *ctx, char *cmd, const char *args,
                       command_reply_t *reply)
{
    (void)ctx; (void)cmd;
    media_command_get_encoded(args, false, false, reply);
}

void media_command_get_base64(app_context_t *ctx, char *cmd, const char *args,
                              command_reply_t *reply)
{
    (void)ctx; (void)cmd;
    media_command_get_encoded(args, true, false, reply);
}

void media_command_get_binary(app_context_t *ctx, char *cmd, const char *args,
                              command_reply_t *reply)
{
    (void)ctx; (void)cmd;
    media_command_get_encoded(args, false, true, reply);
}
