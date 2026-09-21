/* Capture-program states, events, and transition API. */

#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PROGRAM_STATE_BOOTING,
    PROGRAM_STATE_IDLE,
    PROGRAM_STATE_WAITING,
    PROGRAM_STATE_CAPTURING,
    PROGRAM_STATE_SAVING,
    PROGRAM_STATE_TRANSFERRING,
    PROGRAM_STATE_SLEEP_PREP,
    PROGRAM_STATE_ERROR,
    PROGRAM_STATE_COUNT,
} program_state_t;

typedef enum {
    PROGRAM_EVENT_BOOT_COMPLETE,
    PROGRAM_EVENT_START,
    PROGRAM_EVENT_STOP,
    PROGRAM_EVENT_CAPTURE_DUE,
    PROGRAM_EVENT_CAPTURE_COMPLETE,
    PROGRAM_EVENT_SAVE_COMPLETE,
    PROGRAM_EVENT_TRANSFER_BEGIN,
    PROGRAM_EVENT_TRANSFER_END,
    PROGRAM_EVENT_SLEEP_BEGIN,
    PROGRAM_EVENT_WAKE,
    PROGRAM_EVENT_FAULT,
    PROGRAM_EVENT_CLEAR_FAULT,
    PROGRAM_EVENT_COUNT,
} program_event_t;

typedef struct {
    program_state_t state;
    program_state_t resume_state;
    uint32_t transition_count;
} program_state_machine_t;

void program_state_init(program_state_machine_t *machine);
esp_err_t program_state_dispatch(program_state_machine_t *machine, program_event_t event);
const char *program_state_name(program_state_t state);
