/* Read-only diagnostic command handlers for tasks, heap, faults, and events. */

#include "diagnostic_commands.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_core_dump.h"
#include "esp_err.h"
#include "event_breadcrumbs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_diag.h"
#include "watchdog_supervisor.h"

static const char *task_state_text(eTaskState state)
{
    switch (state) {
    case eRunning: return "run";
    case eReady: return "ready";
    case eBlocked: return "block";
    case eSuspended: return "susp";
    case eDeleted: return "del";
    default: return "inv";
    }
}

void diagnostic_command_task_stats(app_context_t *ctx, char *cmd, const char *args,
                                   command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = calloc(capacity, sizeof(TaskStatus_t));
    if (tasks == NULL) {
        command_reply_printf(reply, "ERR TASK_STATS no_mem tasks=%u\n", (unsigned)capacity);
        return;
    }
#if (configGENERATE_RUN_TIME_STATS == 1)
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, capacity, &total_runtime);
#else
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, capacity, NULL);
#endif
    command_reply_printf(reply,
        "TASK_STATS tasks=%u total_runtime_us=%" PRIu64 " cores=%u current_core=%d\n",
        (unsigned)count, (uint64_t)total_runtime,
        (unsigned)configNUMBER_OF_CORES, (int)xPortGetCoreID());
    command_reply_printf(reply, "TASK name state prio core stack_hwm runtime_us cpu_pct\n");

    for (UBaseType_t i = 0; i < count; ++i) {
        const TaskStatus_t *task = &tasks[i];
        char core_text[16] = "?";
#if (configTASKLIST_INCLUDE_COREID == 1)
        if (task->xCoreID == tskNO_AFFINITY) {
            snprintf(core_text, sizeof(core_text), "any");
        } else {
            snprintf(core_text, sizeof(core_text), "%d", (int)task->xCoreID);
        }
#endif
        uint64_t cpu_pct_x100 = 0;
#if (configGENERATE_RUN_TIME_STATS == 1)
        if (total_runtime > 0) {
            cpu_pct_x100 =
                ((uint64_t)task->ulRunTimeCounter * 10000ULL) / (uint64_t)total_runtime;
        }
#endif
        command_reply_printf(reply,
            "TASK %-16s %-5s %u %-4s %u %" PRIu64 " %" PRIu64 ".%02" PRIu64 "\n",
            task->pcTaskName != NULL ? task->pcTaskName : "?",
            task_state_text(task->eCurrentState),
            (unsigned)task->uxCurrentPriority,
            core_text,
            (unsigned)task->usStackHighWaterMark,
            (uint64_t)task->ulRunTimeCounter,
            cpu_pct_x100 / 100ULL,
            cpu_pct_x100 % 100ULL);
    }
    free(tasks);
    command_reply_printf(reply, "OK TASK_STATS\n");
#else
    command_reply_printf(reply, "ERR TASK_STATS trace_disabled\n");
#endif
}

void diagnostic_command_health(app_context_t *ctx, char *cmd, const char *args,
                               command_reply_t *reply)
{
    (void)ctx; (void)cmd; (void)args;
    health_diag_status_t health;
    health_diag_get_status(&health);
    watchdog_supervisor_status_t watchdog;
    watchdog_supervisor_get_status(&watchdog);
    size_t core_addr = 0;
    size_t core_size = 0;
    esp_err_t core_err = esp_core_dump_image_get(&core_addr, &core_size);
    const char *core_state = core_err == ESP_OK ? "present" :
                             core_err == ESP_ERR_NOT_FOUND ? "none" : "error";
    command_reply_printf(reply,
        "HEALTH boots=%u reset_reason=%d last_subsystem=%s last_error=%s heartbeats=%u overdue=%u misses=%u coredump=%s coredump_size=%u\n",
        (unsigned)health.boot_count, (int)health.reset_reason,
        health.last_subsystem, esp_err_to_name(health.last_error),
        (unsigned)watchdog.registered_count, (unsigned)watchdog.overdue_count,
        (unsigned)watchdog.total_misses, core_state,
        core_err == ESP_OK ? (unsigned)core_size : 0U);

    for (uint32_t i = 0; i < watchdog.registered_count; ++i) {
        const watchdog_heartbeat_status_t *task = &watchdog.tasks[i];
        command_reply_printf(reply,
            "HEARTBEAT task=%s age_ms=%u deadline_ms=%u overdue=%u misses=%u\n",
            task->name, (unsigned)task->age_ms, (unsigned)task->deadline_ms,
            task->overdue ? 1U : 0U, (unsigned)task->missed_count);
    }
    event_breadcrumb_t breadcrumbs[EVENT_BREADCRUMB_CAPACITY];
    size_t count = event_breadcrumbs_snapshot(breadcrumbs, EVENT_BREADCRUMB_CAPACITY);
    for (size_t i = 0; i < count; ++i) {
        const event_breadcrumb_t *item = &breadcrumbs[i];
        command_reply_printf(reply,
            "BREADCRUMB seq=%u uptime_ms=%u event=%s value=%d\n",
            (unsigned)item->sequence, (unsigned)item->uptime_ms,
            event_breadcrumb_name(item->code), (int)item->value);
    }
}
