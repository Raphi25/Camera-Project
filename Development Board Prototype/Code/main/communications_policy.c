/* Decide whether radio/command services should run for this wake cycle. */

#include "communications_policy.h"

#include <stddef.h>

bool communications_policy_should_start(const communications_policy_input_t *input)
{
    if (input == NULL) return false;
    if (input->usb_present || input->allow_on_timer_wake) return true;
    return !(input->timer_wake && input->autonomous_capture_enabled);
}
