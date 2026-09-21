/* Inputs and decision API for power-aware communications startup. */

#pragma once

#include <stdbool.h>

typedef struct {
    bool timer_wake;
    bool autonomous_capture_enabled;
    bool usb_present;
    bool allow_on_timer_wake;
} communications_policy_input_t;

bool communications_policy_should_start(const communications_policy_input_t *input);
