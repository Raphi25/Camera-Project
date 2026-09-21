/* Explicit program state machine; invalid transitions fail without fallback. */

#include "program_state.h"

#include <stddef.h>

typedef enum {
    TRANSITION_SET_STATE,
    TRANSITION_SAVE_AND_SET_STATE,
    TRANSITION_RESTORE_STATE,
} transition_action_t;

typedef struct {
    program_state_t from;
    program_event_t event;
    program_state_t to;
    transition_action_t action;
} transition_rule_t;

/* A PROGRAM_STATE_COUNT source is a wildcard. Exact-state rules are listed
 * first so a future state-specific exception takes precedence over a
 * catch-all rule for the same event. */
static const transition_rule_t TRANSITIONS[] = {
    {PROGRAM_STATE_BOOTING,   PROGRAM_EVENT_BOOT_COMPLETE,   PROGRAM_STATE_IDLE,         TRANSITION_SET_STATE},
    {PROGRAM_STATE_WAITING,   PROGRAM_EVENT_CAPTURE_DUE,     PROGRAM_STATE_CAPTURING,    TRANSITION_SET_STATE},
    {PROGRAM_STATE_CAPTURING, PROGRAM_EVENT_CAPTURE_COMPLETE, PROGRAM_STATE_SAVING,       TRANSITION_SET_STATE},
    {PROGRAM_STATE_SAVING,    PROGRAM_EVENT_SAVE_COMPLETE,    PROGRAM_STATE_WAITING,      TRANSITION_SET_STATE},
    {PROGRAM_STATE_TRANSFERRING, PROGRAM_EVENT_TRANSFER_END,  PROGRAM_STATE_COUNT,        TRANSITION_RESTORE_STATE},
    {PROGRAM_STATE_ERROR,     PROGRAM_EVENT_CLEAR_FAULT,      PROGRAM_STATE_COUNT,        TRANSITION_RESTORE_STATE},

    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_START,          PROGRAM_STATE_WAITING,      TRANSITION_SET_STATE},
    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_WAKE,           PROGRAM_STATE_WAITING,      TRANSITION_SET_STATE},
    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_STOP,           PROGRAM_STATE_IDLE,         TRANSITION_SET_STATE},
    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_TRANSFER_BEGIN, PROGRAM_STATE_TRANSFERRING, TRANSITION_SAVE_AND_SET_STATE},
    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_SLEEP_BEGIN,    PROGRAM_STATE_SLEEP_PREP,   TRANSITION_SAVE_AND_SET_STATE},
    {PROGRAM_STATE_COUNT, PROGRAM_EVENT_FAULT,          PROGRAM_STATE_ERROR,        TRANSITION_SAVE_AND_SET_STATE},
};

void program_state_init(program_state_machine_t *machine)
{
    if (machine == NULL) return;
    machine->state = PROGRAM_STATE_BOOTING;
    machine->resume_state = PROGRAM_STATE_IDLE;
    machine->transition_count = 0;
}

esp_err_t program_state_dispatch(program_state_machine_t *machine, program_event_t event)
{
    if (machine == NULL) return ESP_ERR_INVALID_ARG;
    if (machine->state >= PROGRAM_STATE_COUNT || event >= PROGRAM_EVENT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const transition_rule_t *matched = NULL;
    for (size_t i = 0; i < sizeof(TRANSITIONS) / sizeof(TRANSITIONS[0]); ++i) {
        const transition_rule_t *rule = &TRANSITIONS[i];
        if (rule->event == event &&
            (rule->from == machine->state || rule->from == PROGRAM_STATE_COUNT)) {
            matched = rule;
            break;
        }
    }
    if (matched == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    program_state_t next = matched->to;
    if (matched->action == TRANSITION_SAVE_AND_SET_STATE) {
        /* A nested transfer would overwrite its useful return state. */
        if (matched->event == PROGRAM_EVENT_TRANSFER_BEGIN &&
            machine->state == PROGRAM_STATE_TRANSFERRING) {
            return ESP_ERR_INVALID_STATE;
        }
        machine->resume_state = machine->state;
    } else if (matched->action == TRANSITION_RESTORE_STATE) {
        if (machine->resume_state >= PROGRAM_STATE_COUNT) {
            return ESP_ERR_INVALID_STATE;
        }
        next = machine->resume_state;
    }

    if (next != machine->state) {
        machine->state = next;
        machine->transition_count++;
    }
    return ESP_OK;
}

const char *program_state_name(program_state_t state)
{
    static const char *const names[] = {
        "BOOTING", "IDLE", "WAITING", "CAPTURING", "SAVING",
        "TRANSFERRING", "SLEEP_PREP", "ERROR"
    };
    return state < PROGRAM_STATE_COUNT ? names[state] : "UNKNOWN";
}
