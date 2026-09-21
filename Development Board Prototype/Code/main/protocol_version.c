/* Negotiate the highest mutually supported application protocol version. */

#include "protocol_version.h"

#include <stddef.h>

bool protocol_version_negotiate(uint32_t client_max_version,
                                uint32_t *selected_version)
{
    if (selected_version == NULL ||
        client_max_version < DEVICE_PROTOCOL_MIN_VERSION) {
        return false;
    }
    *selected_version = client_max_version < DEVICE_PROTOCOL_CURRENT_VERSION
        ? client_max_version
        : DEVICE_PROTOCOL_CURRENT_VERSION;
    return true;
}
