/* Hardware controls used when entering low-power autonomous operation. */

#pragma once

#include <stdbool.h>

bool communications_usb_detect_is_configured(void);
bool communications_usb_detect_is_active(void);
void communications_hold_coprocessor_in_reset(void);
