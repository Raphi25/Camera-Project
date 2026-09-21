/* Version bounds and capability advertisement for desktop clients. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_PROTOCOL_CURRENT_VERSION 3U
#define DEVICE_PROTOCOL_MIN_VERSION     1U
#define DEVICE_PROTOCOL_FRAME_VERSION   3U
#define DEVICE_PROTOCOL_CAPABILITIES \
    "hex_v2,base64_v2,binary_media_v3,media_sha256,media_resume,media_categories,summaries,health_heartbeats"

bool protocol_version_negotiate(uint32_t client_max_version,
                                uint32_t *selected_version);
