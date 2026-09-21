/* Table-driven command lookup kept independent of concrete transports. */

#include "command_dispatcher.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

bool command_dispatch_table(
    const command_entry_t *entries,
    size_t entry_count,
    app_context_t *ctx,
    char *command,
    command_reply_t *reply
)
{
    if (entries == NULL || command == NULL || *command == '\0') {
        return false;
    }

    for (size_t i = 0; i < entry_count; ++i) {
        const command_entry_t *entry = &entries[i];
        size_t name_len = strlen(entry->name);
        if (strncasecmp(command, entry->name, name_len) != 0) {
            continue;
        }

        char boundary = command[name_len];
        if (boundary != '\0' && !isspace((unsigned char)boundary)) {
            continue;
        }

        const char *args = command + name_len;
        while (isspace((unsigned char)*args)) {
            ++args;
        }
        if (!entry->accepts_args && *args != '\0') {
            return false;
        }

        entry->handler(ctx, command, args, reply);
        return true;
    }
    return false;
}
