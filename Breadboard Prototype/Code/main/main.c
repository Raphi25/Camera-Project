/* Firmware composition root: boot policy, commands, tasks, and power lifecycle. */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_BT_ENABLED
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#include "camera.h"
#include "app_controller.h"
#include "capture_scheduler.h"
#include "daily_summary.h"
#include "device_credentials.h"
#include "health_diag.h"
#include "image_crypto.h"
#include "log_sink.h"
#include "media_transfer.h"
#include "program_state.h"
#include "program_persistence.h"
#include "protocol_version.h"
#include "rtc.h"
#include "sd_storage.h"
#include "ov5647.h"
#include "watchdog_supervisor.h"
#include "event_breadcrumbs.h"
#include "communications_policy.h"
#include "communications_control.h"
#include "command_transport.h"
#include "command_dispatcher.h"
#include "media_commands.h"
#include "startup_init.h"
#include "diagnostic_commands.h"
#include "summary_commands.h"
#include "usb_msc.h"

/*
 * Application coordinator.
 *
 * Responsibilities are intentionally split by subsystem:
 * - RTC + soft clock: provide timestamps even if a transient RTC read fails.
 * - Serial command task: line-oriented control/data protocol for the Python GUI.
 * - Camera scheduler: button-controlled periodic capture and deep-sleep timing.
 * - Storage/logging: encrypted timestamp log and encrypted image transfer.
 *
 * The global flags below are small cross-task state signals. They are kept
 * simple because the system has only a few coarse modes: capture, transfer,
 * paused, and sleep-prep. Longer critical sections use explicit mutexes.
 */

static const char *TAG = "rtc_sd_app";
static SemaphoreHandle_t s_rtc_mutex = NULL;
static rtc_t *s_camera_rtc = NULL;
static imu_orientation_t *s_camera_imu = NULL;
static bool s_skip_initial_idle_once = false;
static bool s_resume_from_timer_wakeup = false;
static bool s_soft_clock_valid = false;
static time_t s_soft_clock_base_epoch = 0;
static int64_t s_soft_clock_base_us = 0;

/* RTC_DATA_ATTR survives deep sleep, which is the normal transition between
 * autonomous captures. The magic values distinguish retained state from a
 * cold boot; NVS is consulted only when retained state is unavailable. */
RTC_DATA_ATTR static uint32_t s_auto_snap_rtc_magic = 0;
RTC_DATA_ATTR static uint8_t s_auto_snap_rtc_enabled = 0;
RTC_DATA_ATTR static uint32_t s_boot_to_capture_est_ms = INITIAL_WAKE_ESTIMATE_MS;
RTC_DATA_ATTR static uint64_t s_previous_cycle_rtc_us = 0;
RTC_DATA_ATTR static uint32_t s_auto_snap_rtc_cycle_count = 0;
RTC_DATA_ATTR static uint32_t s_auto_snap_interval_ms = CAMERA_AUTO_SNAP_INTERVAL_MS;
RTC_DATA_ATTR static uint8_t s_capture_burst_enabled = 0;
RTC_DATA_ATTR static uint32_t s_run_schedule_rtc_magic = 0;
RTC_DATA_ATTR static uint8_t s_run_schedule_enabled = 0;
RTC_DATA_ATTR static uint16_t s_run_schedule_start_min = 8U * 60U;
RTC_DATA_ATTR static uint16_t s_run_schedule_stop_min = 22U * 60U;
RTC_DATA_ATTR static uint16_t s_run_schedule_days = 0;
RTC_DATA_ATTR static int32_t s_run_schedule_start_day = -1;
static program_state_machine_t s_program_state;
static capture_scheduler_t s_capture_scheduler;
static portMUX_TYPE s_program_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_comms_started;
static bool s_comms_power_suppressed;
static bool s_ble_started;
static bool s_c6_bridge_started;

static void program_publish(program_event_t event)
{
    event_breadcrumb_record(BREADCRUMB_PROGRAM_EVENT, (int32_t)event);
    portENTER_CRITICAL(&s_program_state_lock);
    esp_err_t err = program_state_dispatch(&s_program_state, event);
    portEXIT_CRITICAL(&s_program_state_lock);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)health_diag_record_fault("program_state", err);
    }
}

static void program_snapshot(program_state_t *state, uint32_t *transition_count)
{
    portENTER_CRITICAL(&s_program_state_lock);
    if (state != NULL) {
        *state = s_program_state.state;
    }
    if (transition_count != NULL) {
        *transition_count = s_program_state.transition_count;
    }
    portEXIT_CRITICAL(&s_program_state_lock);
}

#define AUTO_SNAP_RTC_MAGIC 0xA5015A10U
#define RUN_SCHEDULE_RTC_MAGIC 0x5C4ED001U

#define LOG_INTERVAL_MS       DEVICE_LOG_INTERVAL_MS
#define SD_RETRY_MS           DEVICE_SD_RETRY_MS
#define AUTO_SNAP_INTERVAL_MIN_S  (AUTO_SNAP_INTERVAL_MIN_MS / 1000U)
#define AUTO_SNAP_INTERVAL_MAX_S  (AUTO_SNAP_INTERVAL_MAX_MS / 1000U)
#define BLE_CONTROL_WAKE_GRACE_MS 1000U
#define RTC_LOG_PATH          SD_CAPTURE_DIR "/rtc_log.enc"
#define COMMAND_HELP_LINE \
    "CMDS: PROTOCOL [CLIENT_MAX_VERSION] | START_PROGRAM [INTERVAL_S] [BURST_0_1] | STOP_PROGRAM | SET_SCHEDULE HH:MM HH:MM DAYS [INTERVAL_S] [BURST_0_1] | START_SCHEDULE HH:MM HH:MM DAYS [INTERVAL_S] [BURST_0_1] | DISABLE_SCHEDULE | CAMERA_MODE [NORMAL|LOW_LIGHT] | DEEP_SLEEP | WAKE_UP | DEVICE_ON | DEVICE_OFF | GETTIME | SETTIME YYYY-MM-DD HH:MM:SS | IMG_SNAP | BURST_3S | MEDIA_EXPORT_KEY | USB_MSC_START | USB_MSC_STOP | IMG_COUNT | IMG_LIST | IMG_GETBIN <name> [offset] | IMG_GET64 <name> [offset] | IMG_GET <name> [offset] | IMG_DELETE_ALL | IMG_PAUSE | IMG_RESUME | V4L2_FORMATS | STATUS | HEALTH | SD_STATUS | TASK_STATS | CORE_STATS | SUMMARY_LIST | SUMMARY_GET <name> | SUMMARY_DELETE_ALL | HELP"


typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint32_t i2c_speed_hz;
    uint32_t log_interval_ms;
    uint32_t sd_retry_ms;
    const char *log_path;
    const char *log_password;
} app_config_t;

typedef struct app_context {
    rtc_t rtc;
    imu_orientation_t imu;
    sdmmc_card_t *card;
    uint32_t seq;
} app_context_t;

static void persist_program_state(bool enabled);

static void controller_enabled_changed(bool enabled)
{
    if (enabled) {
        ESP_LOGI(TAG,
                 "Auto capture schedule toggle: ON (button GPIO%d, burst=%u)",
                 CAMERA_BUTTON_GPIO,
                 (unsigned)s_capture_burst_enabled);
    } else {
        ESP_LOGI(TAG, "Auto capture schedule toggle: OFF (button GPIO%d)", CAMERA_BUTTON_GPIO);
    }
}

static void app_post(app_event_type_t type, bool value)
{
    event_breadcrumb_record(BREADCRUMB_APP_EVENT,
                            ((int32_t)type << 1) | (value ? 1 : 0));
    esp_err_t err = app_controller_post(type, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Application event dropped: type=%d value=%d error=%s",
                 (int)type, value ? 1 : 0, esp_err_to_name(err));
        (void)health_diag_record_fault("app_event_queue", err);
    }
}

static void auto_snap_set_enabled(bool enabled)
{
    bool was_enabled = s_auto_snap_rtc_enabled != 0U;
    s_auto_snap_rtc_magic = AUTO_SNAP_RTC_MAGIC;
    s_auto_snap_rtc_enabled = enabled ? 1U : 0U;
    if (enabled && !was_enabled) {
        s_auto_snap_rtc_cycle_count = 0;
    }
    persist_program_state(enabled);
    app_post(APP_EVENT_SET_ENABLED, enabled);
}

static void media_set_capture_paused(bool paused)
{
    app_post(APP_EVENT_SET_PAUSED, paused);
    camera_set_capture_paused(paused);
}

static bool media_scheduler_is_paused(void)
{
    return app_controller_is_paused();
}

static void media_notify_transfer_state(bool active)
{
    event_breadcrumb_record(BREADCRUMB_TRANSFER, active ? 1 : 0);
    app_post(APP_EVENT_SET_TRANSFER_ACTIVE, active);
    program_publish(active ? PROGRAM_EVENT_TRANSFER_BEGIN : PROGRAM_EVENT_TRANSFER_END);
}

/* Polling beats GPIO interrupts here: button timing is slow, debounce is simple,
 * and polling keeps wake/sleep behavior easier to reason about. */
static void auto_snap_button_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_t heartbeat =
        watchdog_supervisor_register("snap_button", 2000);

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CAMERA_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t cfg_err = gpio_config(&io_cfg);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "Button GPIO config failed: %s", esp_err_to_name(cfg_err));
        vTaskDelete(NULL);
        return;
    }

    int last_raw = gpio_get_level(CAMERA_BUTTON_GPIO);
    int stable_level = last_raw;
    int64_t last_change_us = esp_timer_get_time();

    ESP_LOGI(TAG,
             "Button armed on GPIO%d (active=%d, debounce=%dms)",
             CAMERA_BUTTON_GPIO,
             CAMERA_BUTTON_ACTIVE_LEVEL,
             CAMERA_BUTTON_DEBOUNCE_MS);

    while (true) {
        watchdog_supervisor_beat(heartbeat);
        int raw_level = gpio_get_level(CAMERA_BUTTON_GPIO);
        int64_t now_us = esp_timer_get_time();

        if (raw_level != last_raw) {
            last_raw = raw_level;
            last_change_us = now_us;
        }

        if (raw_level != stable_level &&
            (now_us - last_change_us) >= ((int64_t)CAMERA_BUTTON_DEBOUNCE_MS * 1000LL)) {
            stable_level = raw_level;
            if (stable_level == CAMERA_BUTTON_ACTIVE_LEVEL) {
                auto_snap_set_enabled(!app_controller_is_enabled());
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CAMERA_BUTTON_POLL_MS));
    }
}

static const app_config_t APP_CONFIG = {
    .i2c_port = I2C_NUM_0,
    .sda_gpio = RTC_I2C_SDA_GPIO,
    .scl_gpio = RTC_I2C_SCL_GPIO,
    .i2c_speed_hz = RTC_I2C_PORT_SPEED_HZ,
    .log_interval_ms = LOG_INTERVAL_MS,
    .sd_retry_ms = SD_RETRY_MS,
    .log_path = RTC_LOG_PATH,
    .log_password = NULL,
};

static void app_cleanup(app_context_t *ctx);
static bool command_dispatch(app_context_t *ctx, char *cmd, command_reply_t *reply);
static void trim_newline(char *text);

#if CONFIG_BT_ENABLED
void ble_store_config_init(void);

#define BLE_CONTROL_DEVICE_NAME "RaphasCam"
#define BLE_CONTROL_SERVICE_UUID 0xFFF0
#define BLE_CONTROL_COMMAND_UUID 0xFFF1
#define BLE_CONTROL_RESPONSE_UUID 0xFFF2
#define BLE_CONTROL_MAX_COMMAND_LEN 127

static app_context_t *s_ble_app_ctx = NULL;
static uint8_t s_ble_own_addr_type = 0;
static uint16_t s_ble_conn_handle = 0;
static uint16_t s_ble_response_val_handle = 0;
static bool s_ble_connected = false;
static bool s_ble_response_subscribed = false;

static void ble_control_start_advertising(void);
static int ble_control_gap_event_cb(struct ble_gap_event *event, void *arg);

static void ble_control_notify_text(void *ctx, const char *data, size_t len)
{
    (void)ctx;

    if (!s_ble_connected || !s_ble_response_subscribed || len == 0) {
        return;
    }

    while (len > 0) {
        size_t chunk_len = len > 180 ? 180 : len;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, chunk_len);
        if (om == NULL) {
            ESP_LOGW(TAG, "BLE notify allocation failed");
            return;
        }

        int rc = ble_gatts_notify_custom(
            s_ble_conn_handle,
            s_ble_response_val_handle,
            om
        );
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE notify failed: rc=%d", rc);
            return;
        }

        data += chunk_len;
        len -= chunk_len;
    }
}

static int ble_control_access_cb(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_ble_app_ctx == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char line[BLE_CONTROL_MAX_COMMAND_LEN + 1] = {0};
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, line, BLE_CONTROL_MAX_COMMAND_LEN, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    line[len] = '\0';
    trim_newline(line);

    if (line[0] == '\0') {
        return 0;
    }

    ESP_LOGI(TAG, "BLE command: %s", line);
    command_reply_t reply = {
        .write_cb = ble_control_notify_text,
        .write_cb_ctx = NULL,
    };
    if (!command_dispatch(s_ble_app_ctx, line, &reply)) {
        command_reply_printf(&reply, "ERR Unknown command. Type HELP\n");
    }
    return 0;
}

static const struct ble_gatt_svc_def s_ble_control_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_CONTROL_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CONTROL_COMMAND_UUID),
                .access_cb = ble_control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CONTROL_RESPONSE_UUID),
                .access_cb = ble_control_access_cb,
                .val_handle = &s_ble_response_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0}
        },
    },
    {0}
};

static void ble_control_start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)BLE_CONTROL_DEVICE_NAME;
    fields.name_len = strlen(BLE_CONTROL_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE adv fields failed: rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_ble_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_control_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertising failed: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE control advertising as \"%s\"", BLE_CONTROL_DEVICE_NAME);
}

static int ble_control_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_connected = true;
            s_ble_conn_handle = event->connect.conn_handle;
            s_ble_response_subscribed = false;
            ESP_LOGI(TAG, "BLE GUI connected; handle=%d", s_ble_conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connect failed: status=%d", event->connect.status);
            ble_control_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE GUI disconnected; reason=%d", event->disconnect.reason);
        s_ble_connected = false;
        s_ble_response_subscribed = false;
        ble_control_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble_response_val_handle) {
            s_ble_response_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "BLE response notifications %s",
                     s_ble_response_subscribed ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertising complete; restarting");
        ble_control_start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void ble_control_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset; reason=%d", reason);
}

static void ble_control_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE ensure addr failed: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE infer addr failed: rc=%d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(s_ble_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    ble_control_start_advertising();
}

static void ble_control_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_control_start(app_context_t *ctx)
{
    s_ble_app_ctx = ctx;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_control_on_reset;
    ble_hs_cfg.sync_cb = ble_control_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(BLE_CONTROL_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE name set failed: rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_ble_control_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE GATT count failed: rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_ble_control_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE GATT add failed: rc=%d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_control_host_task);
    ESP_LOGI(TAG, "BLE control service ready: svc=FFF0 write=FFF1 notify=FFF2");
    return ESP_OK;
}
#endif

static void configure_button_deep_sleep_wake(void)
{
    // Keep button wake available while sleeping, even for GUI-requested "Off".
    gpio_config_t wake_btn_cfg = {
        .pin_bit_mask = 1ULL << CAMERA_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&wake_btn_cfg);
    (void)gpio_sleep_set_direction(CAMERA_BUTTON_GPIO, GPIO_MODE_INPUT);
    (void)gpio_sleep_sel_en(CAMERA_BUTTON_GPIO);

    // Active-low button by default: wake on level 0.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    (void)esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << CAMERA_BUTTON_GPIO,
        CAMERA_BUTTON_ACTIVE_LEVEL ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
#else
    (void)esp_deep_sleep_enable_gpio_wakeup(1ULL << CAMERA_BUTTON_GPIO,
                                            CAMERA_BUTTON_ACTIVE_LEVEL ? 1 : 0);
#endif
}

static void configure_usb_deep_sleep_wake(void)
{
#if CAMERA_USB_DETECT_GPIO != GPIO_NUM_NC
    if (!communications_usb_detect_is_configured()) {
        return;
    }

    (void)communications_usb_detect_is_active();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    (void)esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << CAMERA_USB_DETECT_GPIO,
        CAMERA_USB_DETECT_ACTIVE_LEVEL ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
#else
    (void)esp_deep_sleep_enable_gpio_wakeup(1ULL << CAMERA_USB_DETECT_GPIO,
                                            CAMERA_USB_DETECT_ACTIVE_LEVEL ? 1 : 0);
#endif
#endif
}

static void configure_deep_sleep_wake_sources(uint32_t sleep_ms)
{
    configure_button_deep_sleep_wake();
    configure_usb_deep_sleep_wake();
    (void)esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
}

static void enter_device_off_sleep(app_context_t *ctx)
{
    // "Off" means: stop autonomous work, put the camera/storage stack down,
    // and deep-sleep indefinitely. Serial cannot wake a deep-sleeping ESP32-P4,
    // so only the configured wake GPIO/reset/power-cycle can bring it back.
    auto_snap_set_enabled(false);
    app_post(APP_EVENT_SET_PAUSED, true);
    camera_set_capture_paused(true);

    if (app_controller_is_camera_ready()) {
        esp_err_t idle_err = camera_wait_for_idle(10000);
        if (idle_err != ESP_OK) {
            ESP_LOGW(TAG, "Device off requested while camera busy: %s", esp_err_to_name(idle_err));
        }
        camera_deinit();
        app_post(APP_EVENT_SET_CAMERA_READY, false);
    }

    app_cleanup(ctx);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    configure_button_deep_sleep_wake();
    configure_usb_deep_sleep_wake();

    if (communications_usb_detect_is_configured()) {
        ESP_LOGI(TAG,
                 "Device off: entering indefinite deep sleep; wake with button GPIO%d, USB detect GPIO%d, reset, or power",
                 CAMERA_BUTTON_GPIO,
                 CAMERA_USB_DETECT_GPIO);
    } else {
        ESP_LOGI(TAG,
                 "Device off: entering indefinite deep sleep; wake with button GPIO%d, reset, or power",
                 CAMERA_BUTTON_GPIO);
    }
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

// Report runtime security posture at boot for field diagnostics.
static void log_security_config(void)
{
#ifdef CONFIG_SECURE_FLASH_ENC_ENABLED
    ESP_LOGI(TAG, "Security: flash encryption option is enabled in project config");
#else
    ESP_LOGI(TAG, "Security: flash encryption is disabled by project configuration");
#endif

#ifdef CONFIG_SECURE_BOOT
    ESP_LOGI(TAG, "Security: hardware secure boot is enabled in project config");
#else
    ESP_LOGI(TAG, "Security: hardware secure boot is disabled in project config");
#endif

#ifdef CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
    ESP_LOGI(TAG, "Security: signed app verification without hardware secure boot is enabled");
#endif
}

static void format_time_line(const struct tm *value, char *buffer, size_t buffer_size)
{
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", value);
}

// Keep a monotonic software clock fallback when RTC reads are unavailable.
static void soft_clock_set_from_tm(const struct tm *src)
{
    if (src == NULL) {
        return;
    }

    struct tm tmp = *src;
    time_t epoch = mktime(&tmp);
    if (epoch == (time_t)-1) {
        return;
    }

    s_soft_clock_base_epoch = epoch;
    s_soft_clock_base_us = esp_timer_get_time();
    s_soft_clock_valid = true;
}

static esp_err_t soft_clock_get_tm(struct tm *out)
{
    if (!s_soft_clock_valid || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t delta_us = esp_timer_get_time() - s_soft_clock_base_us;
    if (delta_us < 0) {
        delta_us = 0;
    }

    time_t epoch_now = s_soft_clock_base_epoch + (time_t)(delta_us / 1000000LL);
    struct tm *tmp = gmtime(&epoch_now);
    if (tmp == NULL) {
        return ESP_FAIL;
    }

    *out = *tmp;
    return ESP_OK;
}

static esp_err_t read_time_with_fallback(rtc_t *rtc, struct tm *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rtc != NULL) {
        esp_err_t err = rtc_get_time(rtc, out);
        if (err == ESP_OK) {
            soft_clock_set_from_tm(out);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "RTC read failed, using soft clock fallback: %s", esp_err_to_name(err));
    }

    return soft_clock_get_tm(out);
}

/* Timestamp provider passed into camera_capture.c. This avoids making the camera
 * module depend directly on RTC internals while still giving it dated names. */
static esp_err_t camera_timestamp_provider(struct tm *out_time)
{
    if (s_camera_rtc == NULL || out_time == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    }
    err = read_time_with_fallback(s_camera_rtc, out_time);
    if (s_rtc_mutex != NULL) {
        xSemaphoreGive(s_rtc_mutex);
    }
    return err;
}

static esp_err_t camera_orientation_provider(imu_orientation_sample_t *out_sample)
{
    if (s_camera_imu == NULL || out_sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    }
    esp_err_t err = imu_orientation_read(s_camera_imu, out_sample);
    if (s_rtc_mutex != NULL) {
        xSemaphoreGive(s_rtc_mutex);
    }
    return err;
}

static esp_err_t init_rtc(app_context_t *ctx, const app_config_t *cfg)
{
    ESP_LOGI(TAG, "Init I2C for RTC (SDA=%d, SCL=%d)", cfg->sda_gpio, cfg->scl_gpio);
    return rtc_init(&ctx->rtc, cfg->i2c_port, cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_speed_hz);
}

// Try storage once and keep the command/control plane alive if the card is absent.
static esp_err_t mount_storage_with_retry(app_context_t *ctx, const app_config_t *cfg)
{
    esp_err_t err = sd_storage_mount(&ctx->card);
    if (err != ESP_OK) {
        ctx->card = NULL;
        ESP_LOGW(TAG, "SD mount failed (%s); continuing without SD card", esp_err_to_name(err));
        return err;
    }

    err = sd_storage_ensure_capture_dir();
    if (err != ESP_OK) {
        sd_storage_unmount(ctx->card);
        ctx->card = NULL;
        return err;
    }

    return ESP_OK;
}

// Periodic RTC sample path used by the background logger task.
static esp_err_t sample_and_log_time(app_context_t *ctx, const app_config_t *cfg)
{
    int64_t t_start_us = esp_timer_get_time();
    struct tm now = {0};
    char time_line[64] = {0};
    char log_line[96] = {0};

    ESP_LOGI(TAG,
             "sample_and_log_time begin us=%" PRId64 " transfer_active=%d auto_snap_paused=%d",
             t_start_us,
             media_transfer_is_active() ? 1 : 0,
             app_controller_is_paused() ? 1 : 0);

    esp_err_t err;
    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    }
    err = read_time_with_fallback(&ctx->rtc, &now);
    if (s_rtc_mutex != NULL) {
        xSemaphoreGive(s_rtc_mutex);
    }
    if (err != ESP_OK) {
        return err;
    }

    format_time_line(&now, time_line, sizeof(time_line));
    snprintf(log_line, sizeof(log_line), "%" PRIu32 " %s", ctx->seq++, time_line);

    esp_err_t append_err = log_sink_append(log_line);
    int64_t t_end_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "sample_and_log_time end us=%" PRId64 " elapsed_ms=%" PRId64 " result=%s",
             t_end_us,
             (t_end_us - t_start_us) / 1000,
             esp_err_to_name(append_err));

    return append_err;
}

static bool parse_datetime(const char *text, struct tm *out_tm)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = year - 1900;
    out_tm->tm_mon = month - 1;
    out_tm->tm_mday = day;
    out_tm->tm_hour = hour;
    out_tm->tm_min = minute;
    out_tm->tm_sec = second;
    out_tm->tm_isdst = -1;
    return true;
}

static bool parse_hhmm(const char *text, uint16_t *out_minute)
{
    int hour = 0;
    int minute = 0;
    char tail = '\0';

    if (text == NULL || out_minute == NULL) {
        return false;
    }

    if (sscanf(text, "%d:%d%c", &hour, &minute, &tail) != 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }

    *out_minute = (uint16_t)(hour * 60 + minute);
    return true;
}

static void format_hhmm(uint16_t minute_of_day, char *buffer, size_t buffer_size)
{
    snprintf(buffer,
             buffer_size,
             "%02u:%02u",
             (unsigned)(minute_of_day / 60U),
             (unsigned)(minute_of_day % 60U));
}


static void camera_capture_metrics_cb(uint32_t capture_ms, esp_err_t result)
{
    daily_summary_add_capture(capture_ms, result);
    if (result == ESP_OK) {
        program_publish(PROGRAM_EVENT_CAPTURE_COMPLETE);
    } else {
        program_publish(PROGRAM_EVENT_FAULT);
    }
}

static void camera_save_metrics_cb(const camera_save_metrics_t *metrics)
{
    daily_summary_add_save(metrics);
    if (metrics != NULL && metrics->ok) {
        program_publish(PROGRAM_EVENT_SAVE_COMPLETE);
    } else {
        program_publish(PROGRAM_EVENT_FAULT);
    }
}


static bool parse_schedule_args(const char *args,
                                uint16_t *start_minute,
                                uint16_t *stop_minute,
                                uint16_t *days,
                                uint32_t *interval_ms,
                                uint8_t *burst_enabled)
{
    char start_text[16] = {0};
    char stop_text[16] = {0};
    unsigned parsed_days = 0;
    unsigned parsed_interval_s = 0;
    unsigned parsed_burst_enabled = 0;
    int parsed_count = 0;

    if (args == NULL || start_minute == NULL || stop_minute == NULL || days == NULL) {
        return false;
    }

    parsed_count = sscanf(args, "%15s %15s %u %u %u",
                          start_text,
                          stop_text,
                          &parsed_days,
                          &parsed_interval_s,
                          &parsed_burst_enabled);
    if (parsed_count != 3 && parsed_count != 4 && parsed_count != 5) {
        return false;
    }
    if (parsed_days > 3650U) {
        return false;
    }
    if (!parse_hhmm(start_text, start_minute) || !parse_hhmm(stop_text, stop_minute)) {
        return false;
    }

    *days = (uint16_t)parsed_days;
    if (parsed_count >= 4 && interval_ms != NULL) {
        if (parsed_interval_s < AUTO_SNAP_INTERVAL_MIN_S || parsed_interval_s > AUTO_SNAP_INTERVAL_MAX_S) {
            return false;
        }
        uint32_t parsed_ms = parsed_interval_s * 1000U;
        *interval_ms = parsed_ms;
    }
    if (parsed_count == 5 && burst_enabled != NULL) {
        if (parsed_burst_enabled > 1U) {
            return false;
        }
        *burst_enabled = (uint8_t)parsed_burst_enabled;
    }
    return true;
}

static bool parse_start_program_args(const char *args, uint32_t *interval_ms, uint8_t *burst_enabled)
{
    unsigned parsed_interval_s = 0;
    unsigned parsed_burst_enabled = 0;
    int parsed_count = 0;

    if (args == NULL || interval_ms == NULL || burst_enabled == NULL) {
        return false;
    }
    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        return true;
    }
    parsed_count = sscanf(args, "%u %u", &parsed_interval_s, &parsed_burst_enabled);
    if (parsed_count != 1 && parsed_count != 2) {
        return false;
    }

    if (parsed_interval_s < AUTO_SNAP_INTERVAL_MIN_S || parsed_interval_s > AUTO_SNAP_INTERVAL_MAX_S) {
        return false;
    }
    uint32_t parsed_ms = parsed_interval_s * 1000U;
    *interval_ms = parsed_ms;
    if (parsed_count == 2) {
        if (parsed_burst_enabled > 1U) {
            return false;
        }
        *burst_enabled = (uint8_t)parsed_burst_enabled;
    }
    return true;
}

static uint32_t auto_snap_interval_ms(void)
{
    if (s_auto_snap_interval_ms < AUTO_SNAP_INTERVAL_MIN_MS ||
        s_auto_snap_interval_ms > AUTO_SNAP_INTERVAL_MAX_MS) {
        s_auto_snap_interval_ms = CAMERA_AUTO_SNAP_INTERVAL_MS;
    }
    return s_auto_snap_interval_ms;
}

static void persist_program_state(bool enabled)
{
    const program_persistence_state_t state = {
        .enabled = enabled,
        .schedule_enabled = s_run_schedule_enabled != 0U,
        .schedule_start_minute = s_run_schedule_start_min,
        .schedule_stop_minute = s_run_schedule_stop_min,
        .schedule_days = s_run_schedule_days,
        .schedule_start_day = s_run_schedule_start_day,
        .interval_ms = auto_snap_interval_ms(),
        .burst_enabled = s_capture_burst_enabled != 0U,
    };
    esp_err_t err = program_persistence_save(&state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist program state: %s", esp_err_to_name(err));
        (void)health_diag_record_fault("program_persist", err);
    }
}

static void run_schedule_set(uint16_t start_minute, uint16_t stop_minute, uint16_t days)
{
    s_run_schedule_rtc_magic = RUN_SCHEDULE_RTC_MAGIC;
    s_run_schedule_enabled = 1;
    s_run_schedule_start_min = start_minute;
    s_run_schedule_stop_min = stop_minute;
    s_run_schedule_days = days;
    s_run_schedule_start_day = -1;
    capture_scheduler_set(&s_capture_scheduler, start_minute, stop_minute, days);
}

static void run_schedule_disable(void)
{
    s_run_schedule_enabled = 0;
    s_run_schedule_start_day = -1;
    capture_scheduler_disable(&s_capture_scheduler);
    persist_program_state(s_auto_snap_rtc_enabled != 0U);
}

static esp_err_t run_schedule_mark_started(app_context_t *ctx)
{
    if (!s_run_schedule_enabled || s_run_schedule_start_day >= 0) {
        return ESP_OK;
    }

    struct tm now = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
        err = read_time_with_fallback(ctx != NULL ? &ctx->rtc : NULL, &now);
        xSemaphoreGive(s_rtc_mutex);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_capture_scheduler.enabled = s_run_schedule_enabled != 0;
    s_capture_scheduler.start_minute = s_run_schedule_start_min;
    s_capture_scheduler.stop_minute = s_run_schedule_stop_min;
    s_capture_scheduler.days = s_run_schedule_days;
    s_capture_scheduler.start_day = s_run_schedule_start_day;
    err = capture_scheduler_mark_started(&s_capture_scheduler, &now);
    s_run_schedule_start_day = s_capture_scheduler.start_day;
    if (err != ESP_OK) {
        return err;
    }
    daily_summary_reset(s_run_schedule_start_day);
    persist_program_state(s_auto_snap_rtc_enabled != 0U);
    return ESP_OK;
}

static esp_err_t run_schedule_evaluate(app_context_t *ctx,
                                       uint32_t *sleep_ms,
                                       bool *complete,
                                       bool *end_of_day)
{
    if (sleep_ms == NULL || complete == NULL || end_of_day == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm now = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
        err = read_time_with_fallback(ctx != NULL ? &ctx->rtc : NULL, &now);
        xSemaphoreGive(s_rtc_mutex);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_capture_scheduler.enabled = s_run_schedule_enabled != 0;
    s_capture_scheduler.start_minute = s_run_schedule_start_min;
    s_capture_scheduler.stop_minute = s_run_schedule_stop_min;
    s_capture_scheduler.days = s_run_schedule_days;
    s_capture_scheduler.start_day = s_run_schedule_start_day;

    capture_schedule_result_t result = {0};
    err = capture_scheduler_evaluate(&s_capture_scheduler, &now, &result);
    s_run_schedule_start_day = s_capture_scheduler.start_day;
    if (err != ESP_OK) {
        return err;
    }
    if (result.day_changed) {
        daily_summary_reset(result.day_index);
    } else {
        daily_summary_ensure_day(&now);
    }
    *sleep_ms = result.sleep_ms;
    *complete = result.complete;
    *end_of_day = result.end_of_day;
    return ESP_OK;
}

static void enter_scheduled_sleep(app_context_t *ctx, uint32_t sleep_ms, const char *reason)
{
    if (sleep_ms == 0) {
        sleep_ms = 1000U;
    }

    if (app_controller_is_camera_ready()) {
        esp_err_t idle_err = camera_wait_for_idle(10000);
        if (idle_err != ESP_OK) {
            ESP_LOGW(TAG, "Scheduled sleep requested while camera busy: %s", esp_err_to_name(idle_err));
        }
        camera_deinit();
        app_post(APP_EVENT_SET_CAMERA_READY, false);
    }

    ESP_LOGI(TAG,
             "Schedule: entering deep sleep for %u ms (%s)",
             (unsigned)sleep_ms,
             reason != NULL ? reason : "outside run window");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    configure_deep_sleep_wake_sources(sleep_ms);
    esp_deep_sleep_start();
}

static void trim_newline(char *text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static char *trim_spaces_inplace(char *text)
{
    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        len--;
    }

    return text;
}

static char *sanitize_command_inplace(char *text)
{
    if (text == NULL) {
        return NULL;
    }

    size_t write_idx = 0;
    for (size_t read_idx = 0; text[read_idx] != '\0'; read_idx++) {
        unsigned char ch = (unsigned char)text[read_idx];
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch < 32U || ch == 127U) {
            continue;
        }
        text[write_idx++] = (char)ch;
    }
    text[write_idx] = '\0';

    char *cmd = trim_spaces_inplace(text);
    while (cmd != NULL && *cmd != '\0' && !isalpha((unsigned char)*cmd)) {
        cmd++;
    }
    return trim_spaces_inplace(cmd);
}

static bool reply_sd_not_ready(command_reply_t *reply, const char *command_name)
{
    esp_err_t sd_err = sd_storage_require_mounted();
    if (sd_err == ESP_OK) {
        return false;
    }

    command_reply_printf(reply,
                         "ERR %s SD_NOT_READY %s\n",
                         command_name,
                         esp_err_to_name(sd_err));
    return true;
}


static void reply_sd_status(command_reply_t *reply)
{
    if (reply_sd_not_ready(reply, "SD_STATUS")) {
        return;
    }

    int image_count = media_transfer_count();
    int summary_count = summary_commands_count();
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    sd_storage_lock();
    esp_err_t info_err = esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);
    sd_storage_unlock();

    if (info_err != ESP_OK) {
        command_reply_printf(reply, "ERR SD_STATUS %s\n", esp_err_to_name(info_err));
        return;
    }

    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;

    command_reply_printf(
        reply,
        "SD_STATUS total=%" PRIu64 " free=%" PRIu64 " used=%" PRIu64 " images=%d summaries=%d\n",
        total_bytes,
        free_bytes,
        used_bytes,
        image_count,
        summary_count
    );
}


static void command_handle_help(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    command_reply_printf(reply, COMMAND_HELP_LINE "\n");
}

static void command_handle_protocol(app_context_t *ctx, char *cmd, const char *args,
                                    command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    uint32_t client_max = DEVICE_PROTOCOL_CURRENT_VERSION;
    if (*args != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(args, &end, 10);
        while (end != NULL && isspace((unsigned char)*end)) end++;
        if (end == args || end == NULL || *end != '\0' || parsed > UINT32_MAX) {
            command_reply_printf(reply, "ERR PROTOCOL invalid_version\n");
            return;
        }
        client_max = (uint32_t)parsed;
    }

    uint32_t selected = 0;
    if (!protocol_version_negotiate(client_max, &selected)) {
        command_reply_printf(reply,
            "ERR PROTOCOL unsupported client_max=%u minimum=%u\n",
            (unsigned)client_max, (unsigned)DEVICE_PROTOCOL_MIN_VERSION);
        return;
    }
    command_reply_printf(reply,
        "PROTOCOL selected=%u current=%u minimum=%u frame=%u capabilities=%s\n",
        (unsigned)selected,
        (unsigned)DEVICE_PROTOCOL_CURRENT_VERSION,
        (unsigned)DEVICE_PROTOCOL_MIN_VERSION,
        (unsigned)DEVICE_PROTOCOL_FRAME_VERSION,
        DEVICE_PROTOCOL_CAPABILITIES);
}

static void command_handle_get_time(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd; (void)args;
    struct tm now = {0};
    char time_line[64] = {0};
    xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    esp_err_t err = read_time_with_fallback(&ctx->rtc, &now);
    xSemaphoreGive(s_rtc_mutex);
    if (err != ESP_OK) {
        command_reply_printf(reply, "ERR GETTIME %s\n", esp_err_to_name(err));
    } else {
        format_time_line(&now, time_line, sizeof(time_line));
        command_reply_printf(reply, "TIME %s\n", time_line);
    }
}

static void command_handle_set_time(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd;
    struct tm target = {0};
    if (*args == '\0' || !parse_datetime(args, &target)) {
        command_reply_printf(reply, "ERR SETTIME invalid format. Use YYYY-MM-DD HH:MM:SS\n");
        return;
    }
    xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
    esp_err_t err = rtc_set_time(&ctx->rtc, &target);
    xSemaphoreGive(s_rtc_mutex);
    if (err != ESP_OK) {
        command_reply_printf(reply, "ERR SETTIME %s\n", esp_err_to_name(err));
    } else {
        soft_clock_set_from_tm(&target);
        command_reply_printf(reply, "OK SETTIME %s\n", args);
    }
}

static void command_handle_camera_mode(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd;
    if (*args == '\0') {
        command_reply_printf(reply, "CAMERA_MODE %s fps=%u\n",
               startup_camera_mode_name(startup_camera_mode_fps()),
               (unsigned)startup_camera_mode_fps());
        return;
    }

    uint8_t requested_fps;
    if (strcasecmp(args, "NORMAL") == 0) {
        requested_fps = 45;
    } else if (strcasecmp(args, "LOW_LIGHT") == 0 || strcasecmp(args, "LOW-LIGHT") == 0) {
        requested_fps = 15;
    } else {
        command_reply_printf(reply, "ERR CAMERA_MODE invalid mode. Use NORMAL or LOW_LIGHT\n");
        return;
    }
    if (requested_fps == startup_camera_mode_fps()) {
        command_reply_printf(reply, "OK CAMERA_MODE %s already_active fps=%u\n",
               startup_camera_mode_name(requested_fps), (unsigned)requested_fps);
        return;
    }
    esp_err_t err = startup_select_camera_mode(requested_fps);
    if (err != ESP_OK) {
        command_reply_printf(reply, "ERR CAMERA_MODE %s\n", esp_err_to_name(err));
        return;
    }
    command_reply_printf(reply, "OK CAMERA_MODE %s fps=%u restarting\n",
           startup_camera_mode_name(requested_fps), (unsigned)requested_fps);
}

static void command_handle_status(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd; (void)args;
    char schedule_start[8] = {0};
    char schedule_stop[8] = {0};
    struct tm now = {0};
    program_state_t program_state = PROGRAM_STATE_BOOTING;
    uint32_t transition_count = 0;
    char time_text[64] = "unknown";
    program_snapshot(&program_state, &transition_count);
    format_hhmm(s_run_schedule_start_min, schedule_start, sizeof(schedule_start));
    format_hhmm(s_run_schedule_stop_min, schedule_stop, sizeof(schedule_stop));
    if (s_rtc_mutex != NULL) {
        xSemaphoreTake(s_rtc_mutex, portMAX_DELAY);
        if (read_time_with_fallback(ctx != NULL ? &ctx->rtc : NULL, &now) == ESP_OK) {
            format_time_line(&now, time_text, sizeof(time_text));
            for (char *p = time_text; *p != '\0'; p++) {
                if (*p == ' ') *p = '_';
            }
        }
        xSemaphoreGive(s_rtc_mutex);
    }
    command_reply_printf(reply, "STATUS state=%s transitions=%u auto=%d paused=%d transfer=%d camera=%d camera_mode=%s camera_fps=%u forced_awake=%d usb_detect=%d comms=%s ble=%d c6_bridge=%d schedule=%d burst=%d start=%s stop=%s days=%u start_day=%d time=%s interval_ms=%u sd=%d otg=%d otg_ready=%d\n",
           program_state_name(program_state), (unsigned)transition_count,
           app_controller_is_enabled() ? 1 : 0, app_controller_is_paused() ? 1 : 0,
           media_transfer_is_active() ? 1 : 0, app_controller_is_camera_ready() ? 1 : 0,
           startup_camera_mode_name(startup_camera_mode_fps()),
           (unsigned)startup_camera_mode_fps(),
           app_controller_is_forced_awake() ? 1 : 0,
           communications_usb_detect_is_active() ? 1 : 0,
           s_comms_power_suppressed ? "power_save" :
               s_comms_started ? "on" : "unavailable",
           s_ble_started ? 1 : 0,
           s_c6_bridge_started ? 1 : 0,
           s_run_schedule_enabled ? 1 : 0, s_capture_burst_enabled ? 1 : 0,
           schedule_start, schedule_stop, (unsigned)s_run_schedule_days,
           (int)s_run_schedule_start_day, time_text,
           (unsigned)auto_snap_interval_ms(), sd_storage_is_mounted() ? 1 : 0,
           usb_msc_is_connected() ? 1 : 0,
           (usb_msc_is_connected() && ctx != NULL && ctx->card != NULL &&
            (sd_storage_is_mounted() || usb_msc_is_active())) ? 1 : 0);
}

static void command_apply_schedule(app_context_t *ctx, const char *args, bool start_now, command_reply_t *reply)
{
    uint16_t start_minute = 0, stop_minute = 0, days = 0;
    uint32_t interval_ms = auto_snap_interval_ms();
    uint8_t burst_enabled = s_capture_burst_enabled;
    if (!parse_schedule_args(args, &start_minute, &stop_minute, &days,
                             &interval_ms, &burst_enabled)) {
        command_reply_printf(reply, "ERR SCHEDULE invalid format. Use HH:MM HH:MM DAYS [INTERVAL_S] [BURST_0_1]\n");
        return;
    }

    run_schedule_set(start_minute, stop_minute, days);
    s_auto_snap_interval_ms = interval_ms;
    s_capture_burst_enabled = burst_enabled;
    persist_program_state(s_auto_snap_rtc_enabled != 0U);
    char start[8] = {0}, stop[8] = {0};
    format_hhmm(start_minute, start, sizeof(start));
    format_hhmm(stop_minute, stop, sizeof(stop));

    if (!start_now) {
        command_reply_printf(reply, "OK SET_SCHEDULE %s %s %u interval_s=%u burst=%u\n",
               start, stop, (unsigned)days,
               (unsigned)(auto_snap_interval_ms() / 1000U),
               (unsigned)s_capture_burst_enabled);
        return;
    }

    (void)run_schedule_mark_started(ctx);
    app_post(APP_EVENT_SET_FORCED_AWAKE, false);
    auto_snap_set_enabled(true);
    program_publish(PROGRAM_EVENT_START);
    app_post(APP_EVENT_SET_PAUSED, false);
    camera_set_capture_paused(false);
    esp_err_t err = app_controller_is_camera_ready() ? camera_resume_stream() : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        command_reply_printf(reply, "OK START_SCHEDULE %s %s %u interval_s=%u burst=%u\n",
               start, stop, (unsigned)days,
               (unsigned)(auto_snap_interval_ms() / 1000U),
               (unsigned)s_capture_burst_enabled);
    } else {
        command_reply_printf(reply, "ERR START_SCHEDULE %s\n", esp_err_to_name(err));
    }
}

static void command_handle_set_schedule(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd;
    command_apply_schedule(ctx, args, false, reply);
}

static void command_handle_start_schedule(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd;
    command_apply_schedule(ctx, args, true, reply);
}

static void command_handle_disable_schedule(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    run_schedule_disable();
    command_reply_printf(reply, "OK DISABLE_SCHEDULE\n");
}

static void command_handle_start_program(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd;
    uint32_t interval_ms = auto_snap_interval_ms();
    uint8_t burst_enabled = s_capture_burst_enabled;
    if (!parse_start_program_args(args, &interval_ms, &burst_enabled)) {
        command_reply_printf(reply, "ERR START_PROGRAM invalid args. Use [INTERVAL_S] [BURST_0_1], seconds from %u to %u\n",
               (unsigned)AUTO_SNAP_INTERVAL_MIN_S, (unsigned)AUTO_SNAP_INTERVAL_MAX_S);
        return;
    }
    s_auto_snap_interval_ms = interval_ms;
    s_capture_burst_enabled = burst_enabled;
    if (s_run_schedule_enabled) {
        (void)run_schedule_mark_started(ctx);
        app_post(APP_EVENT_SET_FORCED_AWAKE, false);
    } else {
        app_post(APP_EVENT_SET_FORCED_AWAKE, true);
    }
    auto_snap_set_enabled(true);
    program_publish(PROGRAM_EVENT_START);
    app_post(APP_EVENT_SET_PAUSED, false);
    camera_set_capture_paused(false);
    esp_err_t err = app_controller_is_camera_ready() ? camera_resume_stream() : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        command_reply_printf(reply, "OK START_PROGRAM interval_s=%u burst=%u\n",
               (unsigned)(auto_snap_interval_ms() / 1000U),
               (unsigned)s_capture_burst_enabled);
    } else {
        command_reply_printf(reply, "ERR START_PROGRAM %s\n", esp_err_to_name(err));
    }
}

static void command_handle_deep_sleep(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)cmd; (void)args;
    command_reply_printf(reply, "OK DEEP_SLEEP sleeping\n");
    enter_device_off_sleep(ctx);
}

static void command_handle_img_snap(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    if (reply_sd_not_ready(reply, "IMG_SNAP")) return;
    esp_err_t err = camera_capture_once();
    if (err == ESP_OK) {
        command_reply_printf(reply, "OK IMG_SNAP\n");
    } else {
        command_reply_printf(reply, "ERR IMG_SNAP %s\n", esp_err_to_name(err));
    }
}

static void command_handle_burst(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    if (reply_sd_not_ready(reply, "BURST_3S")) return;

    ESP_LOGI(TAG, "Manual still burst requested: %" PRIu32 " ms",
             (uint32_t)CAMERA_BURST_WINDOW_MS);
    app_post(APP_EVENT_SET_PAUSED, false);
    camera_set_capture_paused(false);
    esp_err_t err = camera_resume_stream();
    if (err != ESP_OK) {
        command_reply_printf(reply, "ERR BURST_3S resume_%s\n", esp_err_to_name(err));
        return;
    }
    uint32_t count = 0;
    err = camera_capture_burst(CAMERA_BURST_WINDOW_MS, &count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Manual still burst complete (%" PRIu32 " images)", count);
        command_reply_printf(reply, "OK BURST_3S images=%" PRIu32 "\n", count);
    } else {
        ESP_LOGW(TAG, "Manual still burst failed: %s", esp_err_to_name(err));
        command_reply_printf(reply, "ERR BURST_3S %s\n", esp_err_to_name(err));
    }
}

static void command_handle_sd_status(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    reply_sd_status(reply);
}

static void command_handle_stop_program(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    auto_snap_set_enabled(false);
    program_publish(PROGRAM_EVENT_STOP);
    app_post(APP_EVENT_SET_PAUSED, true);
    camera_set_capture_paused(true);
    command_reply_printf(reply, "OK STOP_PROGRAM\n");
}

static void command_handle_wake_up(app_context_t *ctx, char *cmd, const char *args, command_reply_t *reply)
{
    (void)reply; (void)ctx; (void)cmd; (void)args;
    command_reply_printf(reply, "OK WAKE_UP already_awake\n");
}

static bool s_msc_restore_paused;

static void command_handle_media_export_key(app_context_t *ctx, char *cmd,
                                            const char *args, command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    (void)args;
    const char *key = device_credentials_media_password();
    if (key == NULL) {
        command_reply_printf(reply, "ERR MEDIA_EXPORT_KEY unavailable\n");
        return;
    }
    /* Physical serial access already permits plaintext IMG_GET transfers. This
     * transient key lets the same trusted GUI decrypt raw USB-MSC copies. */
    command_reply_printf(reply, "MEDIA_EXPORT_KEY %s\n", key);
}

static void command_handle_usb_msc_start(app_context_t *ctx, char *cmd,
                                         const char *args, command_reply_t *reply)
{
    (void)cmd;
    (void)args;
    if (usb_msc_is_active()) {
        command_reply_printf(reply, "OK USB_MSC_START already_active\n");
        return;
    }
    if (ctx->card == NULL || !sd_storage_is_mounted()) {
        command_reply_printf(reply, "ERR USB_MSC_START SD_NOT_READY\n");
        return;
    }

    s_msc_restore_paused = app_controller_is_paused();
    media_set_capture_paused(true);
    media_notify_transfer_state(true);
    esp_err_t err = camera_wait_for_idle(15000);
    if (err == ESP_OK) err = camera_suspend_stream();
    if (err == ESP_OK) {
        /* Close the encrypted RTC log before Windows receives block-level
         * ownership of the FAT volume. */
        log_sink_deinit();
        err = usb_msc_start(ctx->card);
    }
    if (err != ESP_OK) {
        (void)log_sink_init(RTC_LOG_PATH, device_credentials_log_password());
        media_set_capture_paused(s_msc_restore_paused);
        if (!s_msc_restore_paused) (void)camera_resume_stream();
        media_notify_transfer_state(false);
        command_reply_printf(reply, "ERR USB_MSC_START %s\n", esp_err_to_name(err));
        return;
    }
    command_reply_printf(reply,
                         "OK USB_MSC_START connect_otg_usb_c eject_before_stop\n");
}

static void command_handle_usb_msc_stop(app_context_t *ctx, char *cmd,
                                        const char *args, command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    (void)args;
    if (!usb_msc_is_active()) {
        command_reply_printf(reply, "ERR USB_MSC_STOP not_active\n");
        return;
    }
    esp_err_t err = usb_msc_stop();
    if (err == ESP_OK) {
        err = log_sink_init(RTC_LOG_PATH, device_credentials_log_password());
    }
    if (err != ESP_OK) {
        command_reply_printf(reply, "ERR USB_MSC_STOP %s\n", esp_err_to_name(err));
        return;
    }
    media_set_capture_paused(s_msc_restore_paused);
    if (!s_msc_restore_paused) (void)camera_resume_stream();
    media_notify_transfer_state(false);
    command_reply_printf(reply, "OK USB_MSC_STOP\n");
}

static const command_entry_t s_command_table[] = {
    { "PROTOCOL", true, command_handle_protocol },
    { "GETTIME", false, command_handle_get_time },
    { "SETTIME", true, command_handle_set_time },
    { "HELP", false, command_handle_help },
    { "CAMERA_MODE", true, command_handle_camera_mode },
    { "STATUS", false, command_handle_status },
    { "HEALTH", false, diagnostic_command_health },
    { "SD_STATUS", false, command_handle_sd_status },
    { "TASK_STATS", false, diagnostic_command_task_stats },
    { "CORE_STATS", false, diagnostic_command_task_stats },
    { "SUMMARY_LIST", false, summary_command_list },
    { "SUMMARY_GET", true, summary_command_get },
    { "SUMMARY_DELETE_ALL", false, summary_command_delete_all },
    { "SET_SCHEDULE", true, command_handle_set_schedule },
    { "START_SCHEDULE", true, command_handle_start_schedule },
    { "DISABLE_SCHEDULE", false, command_handle_disable_schedule },
    { "START_PROGRAM", true, command_handle_start_program },
    { "DEVICE_ON", true, command_handle_start_program },
    { "STOP_PROGRAM", false, command_handle_stop_program },
    { "WAKE_UP", false, command_handle_wake_up },
    { "DEEP_SLEEP", false, command_handle_deep_sleep },
    { "DEVICE_OFF", false, command_handle_deep_sleep },
    { "IMG_SNAP", false, command_handle_img_snap },
    { "BURST_3S", false, command_handle_burst },
    { "USB_MSC_START", false, command_handle_usb_msc_start },
    { "USB_MSC_STOP", false, command_handle_usb_msc_stop },
    { "MEDIA_EXPORT_KEY", false, command_handle_media_export_key },
    { "IMG_DELETE_ALL", false, media_command_delete_all },
    { "IMG_PAUSE", false, media_command_pause },
    { "IMG_RESUME", false, media_command_resume },
    { "V4L2_FORMATS", false, media_command_formats },
    { "IMG_COUNT", false, media_command_count },
    { "IMG_LIST", false, media_command_list },
    { "IMG_GETBIN", true, media_command_get_binary },
    { "IMG_GET64", true, media_command_get_base64 },
    { "IMG_GET", true, media_command_get },
};

static bool command_dispatch(app_context_t *ctx, char *cmd, command_reply_t *reply)
{
    /* Normalize transport noise once, then route every USB/BLE/UART command
     * through the same table so command semantics cannot drift by transport. */
    cmd = sanitize_command_inplace(cmd);
    if (cmd == NULL || *cmd == '\0') {
        return false;
    }
    return command_dispatch_table(
        s_command_table,
        sizeof(s_command_table) / sizeof(s_command_table[0]),
        ctx,
        cmd,
        reply
    );
}


static void serial_command_task(void *arg)
{
    /* stdin/stdout are the USB Serial/JTAG console. Media commands temporarily
     * switch that VFS to byte-transparent output for binary payloads. */
    app_context_t *ctx = (app_context_t *)arg;
    char line[128] = {0};
    command_reply_t reply = {.stdio_out = stdout};

    command_reply_printf(&reply, "CMD READY protocol=%u. %s\n",
                         (unsigned)DEVICE_PROTOCOL_CURRENT_VERSION,
                         COMMAND_HELP_LINE);

    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        trim_newline(line);
        char *cmd = sanitize_command_inplace(line);
        if (cmd == NULL || cmd[0] == '\0') {
            continue;
        }


        if (command_dispatch(ctx, cmd, &reply)) {
            continue;
        }

        command_reply_printf(&reply, "ERR Unknown command. Type HELP\n");
    }
}

#if CAMERA_C6_BLE_BRIDGE_ENABLE
static void c6_bridge_uart_task(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;
    const uart_port_t uart_port = (uart_port_t)CAMERA_C6_BRIDGE_UART_PORT;
    char line[128] = {0};
    size_t line_len = 0;
    command_reply_t reply = {
        .uart_port = (uart_port_t)CAMERA_C6_BRIDGE_UART_PORT,
        .use_uart = true,
    };

    if (CAMERA_C6_BRIDGE_TX_GPIO == GPIO_NUM_NC || CAMERA_C6_BRIDGE_RX_GPIO == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "C6 BLE bridge enabled but TX/RX GPIOs are not configured");
        vTaskDelete(NULL);
        return;
    }

    const uart_config_t uart_config = {
        .baud_rate = CAMERA_C6_BRIDGE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(uart_port, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_port, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(
        uart_port,
        CAMERA_C6_BRIDGE_TX_GPIO,
        CAMERA_C6_BRIDGE_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_LOGI(TAG, "C6 BLE bridge UART armed on UART%d @ %d", CAMERA_C6_BRIDGE_UART_PORT, CAMERA_C6_BRIDGE_UART_BAUD);

    while (true) {
        uint8_t ch = 0;
        int n = uart_read_bytes(uart_port, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            line[line_len] = '\0';
            if (line_len > 0) {
                if (!command_dispatch(ctx, line, &reply)) {
                    command_reply_printf(&reply, "ERR Unknown command. Type HELP\n");
                }
            }
            line_len = 0;
            continue;
        }

        if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)ch;
        } else {
            line_len = 0;
            uart_write_bytes(uart_port, "ERR line_too_long\n", 18);
        }
    }
}
#endif

static void scheduler_watchdog_service(watchdog_heartbeat_t heartbeat)
{
    /* The project supervisor records subsystem health; ESP-IDF's task WDT is
     * the final panic/reboot guard. Scheduler waits must satisfy both. */
    watchdog_supervisor_beat(heartbeat);
    (void)esp_task_wdt_reset();
}

static bool scheduler_delay_while_enabled(uint32_t delay_ms,
                                          watchdog_heartbeat_t heartbeat)
{
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < delay_ms && app_controller_is_enabled()) {
        scheduler_watchdog_service(heartbeat);

        uint32_t step_ms = 100;
        uint32_t remaining_ms = delay_ms - elapsed_ms;
        if (remaining_ms < step_ms) {
            step_ms = remaining_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed_ms += step_ms;
    }
    return app_controller_is_enabled();
}

static esp_err_t scheduler_wait_for_camera_idle(uint32_t timeout_ms,
                                                watchdog_heartbeat_t heartbeat)
{
    /* Poll rather than block for the full timeout. A slow SD card can keep the
     * asynchronous writer busy longer than the task-watchdog period. */
    const uint32_t poll_ms = 1000;
    uint32_t elapsed_ms = 0;

    while (timeout_ms == 0 || elapsed_ms < timeout_ms) {
        uint32_t wait_ms = poll_ms;
        if (timeout_ms > 0 && timeout_ms - elapsed_ms < wait_ms) {
            wait_ms = timeout_ms - elapsed_ms;
        }

        esp_err_t err = camera_wait_for_idle(wait_ms);
        scheduler_watchdog_service(heartbeat);
        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
        elapsed_ms += wait_ms;
    }
    return ESP_ERR_TIMEOUT;
}

// Camera scheduler task for periodic stills and optional bursts.
static void auto_snapshot_task(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;
    watchdog_heartbeat_t heartbeat =
        watchdog_supervisor_register("capture_sched", 45000);
    esp_err_t watchdog_err = esp_task_wdt_add(NULL);
    if (watchdog_err != ESP_OK && watchdog_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Scheduler watchdog registration failed: %s", esp_err_to_name(watchdog_err));
        (void)health_diag_record_fault("scheduler_wdt", watchdog_err);
    }

    uint32_t cycles_done = 0;
    bool was_enabled = false;
    ESP_LOGI(TAG, "Auto capture schedule armed and waiting for button press on GPIO%d", CAMERA_BUTTON_GPIO);

    while (true) {
        scheduler_watchdog_service(heartbeat);
        if (!app_controller_is_enabled()) {
            if (was_enabled) {
                ESP_LOGI(TAG, "Auto capture schedule stopped after %u cycle(s)", (unsigned)cycles_done);
                was_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!was_enabled) {
            if (s_resume_from_timer_wakeup) {
                cycles_done = s_auto_snap_rtc_cycle_count;
            } else {
                cycles_done = 0;
                s_auto_snap_rtc_cycle_count = 0;
            }
            was_enabled = true;
            if (s_skip_initial_idle_once) {
                s_skip_initial_idle_once = false;
                ESP_LOGI(TAG, "Auto capture schedule resumed from deep sleep; skipping initial idle");
            } else if (CAMERA_AUTO_SNAP_INITIAL_IDLE_MS > 0) {
                uint32_t initial_idle_ms = CAMERA_AUTO_SNAP_INITIAL_IDLE_MS;
#if CONFIG_BT_ENABLED
                if (initial_idle_ms < BLE_CONTROL_WAKE_GRACE_MS) {
                    initial_idle_ms = BLE_CONTROL_WAKE_GRACE_MS;
                }
#endif
                ESP_LOGI(TAG,
                         "Auto capture schedule started; initial idle %u ms, burst=%u",
                         (unsigned)initial_idle_ms,
                         (unsigned)s_capture_burst_enabled);
                if (!scheduler_delay_while_enabled(initial_idle_ms, heartbeat)) {
                    continue;
                }
            }
        }

        if (s_run_schedule_enabled && !app_controller_is_paused() && !media_transfer_is_active()) {
            uint32_t scheduled_sleep_ms = 0;
            bool schedule_complete = false;
            bool end_of_day = false;
            esp_err_t schedule_err = run_schedule_evaluate(ctx, &scheduled_sleep_ms, &schedule_complete, &end_of_day);
            if (schedule_err != ESP_OK) {
                ESP_LOGW(TAG, "Schedule check failed, continuing capture loop: %s",
                         esp_err_to_name(schedule_err));
            } else if (schedule_complete) {
                ESP_LOGI(TAG, "Schedule complete after configured %u day(s); stopping program",
                         (unsigned)s_run_schedule_days);
                (void)camera_wait_for_idle(15000);
                (void)daily_summary_write_file("schedule complete", s_run_schedule_days);
                run_schedule_disable();
                enter_device_off_sleep(ctx);
            } else if (scheduled_sleep_ms > 0) {
                if (end_of_day) {
                    (void)camera_wait_for_idle(15000);
                    (void)daily_summary_write_file("end of scheduled run window", s_run_schedule_days);
                }
                enter_scheduled_sleep(ctx, scheduled_sleep_ms, "outside run window");
            }
        }

        if (app_controller_is_camera_ready() && !app_controller_is_paused() && !media_transfer_is_active()) {
            if (!sd_storage_is_mounted()) {
                esp_err_t sd_err = sd_storage_require_mounted();
                ESP_LOGW(TAG, "Auto capture skipped because SD is not mounted: %s", esp_err_to_name(sd_err));
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            int64_t cycle_start_us = esp_timer_get_time();
            uint64_t cycle_start_rtc_us = esp_rtc_get_time_us();
            (void)daily_summary_note_start();

            if (s_resume_from_timer_wakeup) {
                uint32_t observed_boot_to_capture_ms = (uint32_t)(cycle_start_us / 1000);
                uint32_t observed_interval_ms = 0;
                uint32_t previous_estimate_ms = s_boot_to_capture_est_ms;
                if (s_previous_cycle_rtc_us > 0 && cycle_start_rtc_us > s_previous_cycle_rtc_us) {
                    uint64_t interval_us = cycle_start_rtc_us - s_previous_cycle_rtc_us;
                    if (interval_us / 1000U <= UINT32_MAX) {
                        observed_interval_ms = (uint32_t)(interval_us / 1000U);
                        s_boot_to_capture_est_ms = capture_scheduler_adjust_wake_estimate(
                            s_boot_to_capture_est_ms,
                            observed_interval_ms,
                            auto_snap_interval_ms(),
                            BOOT_TO_CAPTURE_EST_MIN_MS,
                            BOOT_TO_CAPTURE_EST_MAX_MS,
                            CAPTURE_INTERVAL_FEEDBACK_DIVISOR,
                            CAPTURE_INTERVAL_MAX_ADJUST_MS);
                    }
                }
                /* cycle_start_us is measured from this timer wake and therefore
                 * directly captures the camera/SD initialization cost that must
                 * be subtracted from the next sleep.  The interval feedback above
                 * remains useful for RTC and scheduling error, but its deliberately
                 * small adjustment must not take dozens of captures to learn a
                 * roughly 15-second cold-start time. */
                if (observed_boot_to_capture_ms < BOOT_TO_CAPTURE_EST_MIN_MS) {
                    observed_boot_to_capture_ms = BOOT_TO_CAPTURE_EST_MIN_MS;
                } else if (observed_boot_to_capture_ms > BOOT_TO_CAPTURE_EST_MAX_MS) {
                    observed_boot_to_capture_ms = BOOT_TO_CAPTURE_EST_MAX_MS;
                }
                s_boot_to_capture_est_ms = observed_boot_to_capture_ms;
                ESP_LOGI(TAG,
                         "Cadence feedback: interval=%u ms, target=%u ms, wake->cycle=%u ms, estimate=%u->%u ms",
                         (unsigned)observed_interval_ms,
                         (unsigned)auto_snap_interval_ms(),
                         (unsigned)observed_boot_to_capture_ms,
                         (unsigned)previous_estimate_ms,
                         (unsigned)s_boot_to_capture_est_ms);
                s_resume_from_timer_wakeup = false;
            }
            s_previous_cycle_rtc_us = cycle_start_rtc_us;

            program_publish(PROGRAM_EVENT_CAPTURE_DUE);
            uint32_t burst_count = 0;
            esp_err_t snap_err = s_capture_burst_enabled
                ? camera_capture_burst(CAMERA_BURST_WINDOW_MS, &burst_count)
                : camera_capture_once();
            event_breadcrumb_record(BREADCRUMB_CAPTURE_RESULT, (int32_t)snap_err);
            if (snap_err != ESP_OK) {
                ESP_LOGW(TAG, "Auto %s failed: %s",
                         s_capture_burst_enabled ? "timed burst" : "still image",
                         esp_err_to_name(snap_err));
                (void)health_diag_record_fault("camera_capture", snap_err);
            } else if (s_capture_burst_enabled) {
                ESP_LOGI(TAG, "Auto burst captured (%" PRIu32 " images)", burst_count);
            } else {
                ESP_LOGI(TAG, "Auto still image captured");
            }

            if (snap_err == ESP_OK) {
                cycles_done++;
                s_auto_snap_rtc_cycle_count = cycles_done;
                ESP_LOGI(TAG, "Auto capture cycle complete (cycle=%u, burst=%u)",
                         (unsigned)cycles_done,
                         (unsigned)s_capture_burst_enabled);
            }

            int64_t elapsed_ms = (esp_timer_get_time() - cycle_start_us) / 1000;
            uint32_t current_interval_ms = auto_snap_interval_ms();
            uint32_t sleep_ms = 0;
            if (elapsed_ms < current_interval_ms) {
                sleep_ms = current_interval_ms - (uint32_t)elapsed_ms;
            }

            ESP_LOGI(TAG, "Capture cycle %u complete; idling %u ms before next capture",
                     (unsigned)cycles_done,
                     (unsigned)sleep_ms);

#if CAMERA_AUTO_SNAP_DEEP_SLEEP
            /* USB Serial/JTAG cannot wake the P4 from deep sleep, so check for
             * host SOF traffic while the chip is awake. Once a GUI cable is
             * seen, keep the controller awake instead of immediately
             * disappearing again after this capture. */
            if (!app_controller_is_forced_awake() &&
                usb_serial_jtag_is_connected()) {
                app_post(APP_EVENT_SET_FORCED_AWAKE, true);
                ESP_LOGI(TAG, "USB GUI host detected; deep sleep deferred");
            }
            if (!app_controller_is_forced_awake() &&
                sleep_ms > 0 && app_controller_is_enabled() && !app_controller_is_paused() && !media_transfer_is_active()) {
                // Do not sleep until asynchronous media write work is finished.
                esp_err_t idle_err = scheduler_wait_for_camera_idle(sleep_ms + 15000, heartbeat);
                if (idle_err == ESP_OK) {
                    /* IMG_PAUSE/IMG_GET may have arrived while idle wait was
                     * blocking. Re-check before tearing down serial and camera. */
                    if (!app_controller_is_enabled() || app_controller_is_paused() || media_transfer_is_active()) {
                        ESP_LOGI(TAG, "Deep sleep deferred: transfer/pause became active during idle wait");
                        continue;
                    }

                    /* Include camera shutdown in the awake-time budget. It can
                     * take over 100 ms, which otherwise lengthens every
                     * capture-to-capture interval by the same amount. */
                    camera_deinit();
                    app_post(APP_EVENT_SET_CAMERA_READY, false);
                    vTaskDelay(pdMS_TO_TICKS(20));

                    uint32_t pre_sleep_ms = (uint32_t)((esp_timer_get_time() - cycle_start_us) / 1000);
                    uint32_t compensated_sleep_ms = 0;
                    uint32_t wake_compensation_ms = s_boot_to_capture_est_ms;
                    current_interval_ms = auto_snap_interval_ms();
                    if (pre_sleep_ms < current_interval_ms) {
                        compensated_sleep_ms = current_interval_ms - pre_sleep_ms;
                        if (compensated_sleep_ms > wake_compensation_ms) {
                            compensated_sleep_ms -= wake_compensation_ms;
                        } else {
                            compensated_sleep_ms = 0;
                        }
                    }

                    /* Both modes use the latest measured timer-wake cost so
                     * trigger-to-trigger cadence stays close to target. */
                    ESP_LOGI(TAG,
                             "Entering deep sleep for %u ms (pre_sleep=%u ms, wake_est=%u ms, target_interval=%u ms)",
                             (unsigned)compensated_sleep_ms,
                             (unsigned)pre_sleep_ms,
                             (unsigned)wake_compensation_ms,
                             (unsigned)current_interval_ms);
                    event_breadcrumb_record(BREADCRUMB_SLEEP,
                                            (int32_t)compensated_sleep_ms);
                    if (compensated_sleep_ms > 0) {
                        configure_deep_sleep_wake_sources(compensated_sleep_ms);
                    } else {
                        // If processing overran the schedule budget, wake quickly and record ASAP.
                        configure_deep_sleep_wake_sources(1);
                    }
                    esp_deep_sleep_start();
                }
                ESP_LOGW(TAG, "Deep sleep skipped this cycle (camera idle wait: %s)", esp_err_to_name(idle_err));
            }
            if (app_controller_is_forced_awake() && sleep_ms > 0 && app_controller_is_enabled() && !app_controller_is_paused()) {
                ESP_LOGI(TAG,
                         "USB interactive mode: staying awake for %u ms before next capture cycle",
                         (unsigned)sleep_ms);
            }
#endif

            (void)scheduler_delay_while_enabled(sleep_ms, heartbeat);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void app_cleanup(app_context_t *ctx)
{
    log_sink_deinit();
    if (ctx != NULL) {
        sd_storage_unmount(ctx->card);
        ctx->card = NULL;
        imu_orientation_deinit(&ctx->imu);
        rtc_deinit(&ctx->rtc);
    }
}

void app_main(void)
{
    /* Boot order matters: persistent state and storage precede command tasks;
     * the shared RTC/I2C bus precedes camera initialization; background
     * capture starts only after controller state has been restored. */
    app_context_t ctx = {
        .rtc = {0},
        .imu = {0},
        .card = NULL,
        .seq = 0,
    };
    app_config_t runtime_config = APP_CONFIG;
    const app_config_t *cfg = &runtime_config;

    program_state_init(&s_program_state);
    log_security_config();

    esp_err_t camera_mode_err = startup_init_camera_mode();
    ESP_ERROR_CHECK(program_persistence_init());
    ESP_ERROR_CHECK(device_credentials_init());
    runtime_config.log_password = device_credentials_log_password();
    ESP_ERROR_CHECK(health_diag_init());
    event_breadcrumbs_init((int32_t)esp_reset_reason());
    ESP_ERROR_CHECK(watchdog_supervisor_init());
    program_publish(PROGRAM_EVENT_BOOT_COMPLETE);
    if (camera_mode_err != ESP_OK) {
        ESP_LOGE(TAG, "Camera mode load failed: %s; using compiled default",
                 esp_err_to_name(camera_mode_err));
    }

    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    const bool timer_wake = (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0U;
    bool usb_present_at_boot = communications_usb_detect_is_active();
    bool rtc_program_state_valid =
        s_auto_snap_rtc_magic == AUTO_SNAP_RTC_MAGIC &&
        s_run_schedule_rtc_magic == RUN_SCHEDULE_RTC_MAGIC;
    /* Deep-sleep wakes use retained RTC state directly. Cold starts restore a
     * validated NVS record so a power interruption does not silently alter the
     * user's schedule, interval, or burst selection. */
    if (!rtc_program_state_valid) {
        s_auto_snap_rtc_magic = AUTO_SNAP_RTC_MAGIC;
        s_auto_snap_rtc_enabled = 0;
        s_auto_snap_rtc_cycle_count = 0;
        s_run_schedule_rtc_magic = RUN_SCHEDULE_RTC_MAGIC;
        s_run_schedule_enabled = 0;
        s_run_schedule_start_min = 8U * 60U;
        s_run_schedule_stop_min = 22U * 60U;
        s_run_schedule_days = 0;
        s_run_schedule_start_day = -1;

        program_persistence_state_t persisted = {0};
        bool persisted_found = false;
        esp_err_t persisted_err = program_persistence_load(&persisted, &persisted_found);
        if (persisted_err == ESP_OK && persisted_found &&
            persisted.schedule_start_minute < 1440U &&
            persisted.schedule_stop_minute < 1440U &&
            persisted.interval_ms >= AUTO_SNAP_INTERVAL_MIN_MS &&
            persisted.interval_ms <= AUTO_SNAP_INTERVAL_MAX_MS &&
            persisted.schedule_start_day >= -1) {
            s_auto_snap_rtc_enabled = persisted.enabled ? 1U : 0U;
            s_run_schedule_enabled = persisted.schedule_enabled ? 1U : 0U;
            s_run_schedule_start_min = persisted.schedule_start_minute;
            s_run_schedule_stop_min = persisted.schedule_stop_minute;
            s_run_schedule_days = persisted.schedule_days;
            s_run_schedule_start_day = persisted.schedule_start_day;
            s_auto_snap_interval_ms = persisted.interval_ms;
            s_capture_burst_enabled = persisted.burst_enabled ? 1U : 0U;
            ESP_LOGI(TAG,
                     "Restored program after power loss: enabled=%u schedule=%u interval=%u ms burst=%u",
                     (unsigned)s_auto_snap_rtc_enabled,
                     (unsigned)s_run_schedule_enabled,
                     (unsigned)s_auto_snap_interval_ms,
                     (unsigned)s_capture_burst_enabled);
        } else if (persisted_err != ESP_OK) {
            ESP_LOGE(TAG, "Persistent program state could not be loaded: %s",
                     esp_err_to_name(persisted_err));
        } else if (persisted_found) {
            ESP_LOGE(TAG, "Persistent program state is invalid; autonomous capture remains disabled");
        }
    }
    s_capture_scheduler = (capture_scheduler_t) {
        .enabled = s_run_schedule_enabled != 0,
        .start_minute = s_run_schedule_start_min,
        .stop_minute = s_run_schedule_stop_min,
        .days = s_run_schedule_days,
        .start_day = s_run_schedule_start_day,
    };
    bool initial_enabled = s_auto_snap_rtc_enabled != 0U;
    bool initial_forced_awake = usb_present_at_boot;
    /* Timer wakes suppress interactive radios unless policy explicitly allows
     * them. USB/interactive boots keep communications available for the GUI. */
    const communications_policy_input_t communications_input = {
        .timer_wake = timer_wake,
        .autonomous_capture_enabled = initial_enabled,
        .usb_present = usb_present_at_boot,
        .allow_on_timer_wake = CAMERA_COMMS_START_ON_TIMER_WAKE != 0,
    };
    bool start_communications =
        communications_policy_should_start(&communications_input);
    s_comms_power_suppressed = !start_communications;
    const app_controller_config_t controller_config = {
        .enabled = initial_enabled,
        .paused = false,
        .camera_ready = false,
        .forced_awake = initial_forced_awake,
        .enabled_changed = controller_enabled_changed,
    };
    ESP_ERROR_CHECK(app_controller_init(&controller_config));
    if (initial_enabled) {
        /* A deep-sleep wake or power-loss restore starts with an already-active
         * controller, so no command event exists to advance the lifecycle. */
        program_publish(PROGRAM_EVENT_WAKE);
    }
    if (timer_wake && initial_enabled) {
        s_skip_initial_idle_once = true;
        s_resume_from_timer_wakeup = true;
    } else {
        /* A flash/reset can retain an obsolete RTC estimate. Start the first
         * post-reset interval from the configured measured baseline. */
        s_boot_to_capture_est_ms = INITIAL_WAKE_ESTIMATE_MS;
    }
    ESP_LOGI(TAG,
             "Wake causes=0x%08" PRIx32 ", auto-schedule persisted state=%s, usb_detect=%d, gui_forced_awake=%d, wake->capture estimate=%u ms",
             wakeup_causes,
             app_controller_is_enabled() ? "ON" : "OFF",
             usb_present_at_boot ? 1 : 0,
             app_controller_is_forced_awake() ? 1 : 0,
             (unsigned)s_boot_to_capture_est_ms);

    setenv("TZ", "UTC0", 1);
    tzset();

    esp_err_t err = init_rtc(&ctx, cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC init failed: %s", esp_err_to_name(err));
        app_cleanup(&ctx);
        return;
    }

    err = mount_storage_with_retry(&ctx, cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Storage unavailable at boot (%s); SD commands will fail fast until reboot with a card",
                 esp_err_to_name(err));
    }

    s_rtc_mutex = xSemaphoreCreateMutex();
    if (s_rtc_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create RTC mutex");
        app_cleanup(&ctx);
        return;
    }

    s_camera_rtc = &ctx.rtc;
    camera_set_timestamp_provider(camera_timestamp_provider);
    ESP_LOGI(TAG, "Init BMI323 I2C (SDA=%d, SCL=%d, INT1=%d, INT2=%d)",
             IMU_I2C_SDA_GPIO, IMU_I2C_SCL_GPIO, IMU_INT1_GPIO, IMU_INT2_GPIO);
    err = imu_orientation_init(&ctx.imu, IMU_I2C_PORT,
                               IMU_I2C_SDA_GPIO, IMU_I2C_SCL_GPIO,
                               IMU_I2C_PORT_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMI323 init failed: %s; captures will log IMU status unavailable",
                 esp_err_to_name(err));
    }
    s_camera_imu = &ctx.imu;
    camera_set_orientation_provider(camera_orientation_provider);
    daily_summary_init(camera_timestamp_provider);
    camera_set_metrics_callbacks(camera_capture_metrics_cb, camera_save_metrics_cb);
    const media_transfer_camera_ops_t media_camera_ops = {
        .set_capture_paused = media_set_capture_paused,
        .wait_for_camera_idle = camera_wait_for_idle,
        .suspend_camera_stream = camera_suspend_stream,
        .resume_camera_stream = camera_resume_stream,
        .scheduler_is_paused = media_scheduler_is_paused,
        .notify_transfer_state = media_notify_transfer_state,
    };
    ESP_ERROR_CHECK(media_transfer_init(&media_camera_ops));

    {
        struct tm now = {0};
        if (read_time_with_fallback(&ctx.rtc, &now) == ESP_OK) {
            soft_clock_set_from_tm(&now);
        }
    }

    /* Keep the wired command path available on every wake, as in the known-
     * good Prototype-22 build. Radio/C6 startup remains governed by the power
     * policy below, so autonomous wakes do not pay the BLE startup cost. */
    if (xTaskCreate(serial_command_task, "serial_cmd", 16384, &ctx, 5, NULL) == pdPASS) {
        s_comms_started = true;
    } else {
        ESP_LOGE(TAG, "Failed to start serial command task");
        (void)health_diag_record_fault("serial_command_task", ESP_ERR_NO_MEM);
    }

    if (!start_communications) {
        communications_hold_coprocessor_in_reset();
        ESP_LOGI(TAG,
                 "BLE/C6 startup skipped: autonomous timer wake (power-save mode)");
    }
#if CAMERA_C6_BLE_BRIDGE_ENABLE
    if (start_communications &&
        xTaskCreate(c6_bridge_uart_task, "c6_ble_bridge", 4096, &ctx, 5, NULL) == pdPASS) {
        s_c6_bridge_started = true;
        s_comms_started = true;
    }
#endif
#if CONFIG_BT_ENABLED
    if (start_communications && CAMERA_ESP_HOSTED_BLE_ENABLE) {
        err = ble_control_start(&ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLE control init failed: %s", esp_err_to_name(err));
            (void)health_diag_record_fault("ble_start", err);
        } else {
            s_ble_started = true;
            s_comms_started = true;
        }
    } else if (start_communications) {
        communications_hold_coprocessor_in_reset();
        ESP_LOGI(TAG, "ESP-Hosted BLE/C6 startup disabled; wired serial and OTG remain available");
    }
#endif

    err = camera_init(ctx.rtc.bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s — continuing without camera", esp_err_to_name(err));
    } else {
        app_post(APP_EVENT_SET_CAMERA_READY, true);
        ESP_LOGI(TAG, "Camera ready for still images and optional bursts; use BURST_3S for a manual 3-second still burst");
        ESP_LOGI(TAG,
                 "Auto capture schedule: %s at boot, toggle with button on GPIO%d, interval %d ms, burst=%u, deep_sleep=%d",
                 app_controller_is_enabled() ? "ON" : "OFF",
                 CAMERA_BUTTON_GPIO,
                 (int)auto_snap_interval_ms(),
                 (unsigned)s_capture_burst_enabled,
                 CAMERA_AUTO_SNAP_DEEP_SLEEP);
        xTaskCreate(auto_snap_button_task, "snap_button", 3072, NULL, 6, NULL);
        /*
         * Keep the capture scheduler away from CPU0.
         *
         * Still bursts are encrypted and flushed to SD one image at a time.
         * That path is intentionally simple and reliable, but it can
         * keep the scheduler busy enough that CPU0's idle task trips the task
         * watchdog. Pinning the high-level scheduler to CPU1 leaves CPU0 free
         * for system/USB/BT/idle housekeeping during clip finalization.
         */
        xTaskCreatePinnedToCore(auto_snapshot_task, "auto_snap", 8192, &ctx, 4, NULL, 1);
    }

    ESP_LOGI(TAG, "Logging RTC timestamps to %s", cfg->log_path);

    if (start_communications) {
        /* PBKDF2 for the optional encrypted RTC log is intentionally deferred
         * until the interactive camera/control plane is ready.  It takes many
         * seconds on this target and is unnecessary on autonomous timer wakes,
         * where camera timestamps are written with each capture instead. */
        err = log_sink_init(cfg->log_path, cfg->log_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Log sink init failed (%s); continuing with SD mounted",
                     esp_err_to_name(err));
        }
    }

    const TickType_t log_ticks = pdMS_TO_TICKS(cfg->log_interval_ms);
    watchdog_heartbeat_t main_heartbeat = watchdog_supervisor_register(
        "app_main", cfg->log_interval_ms + 60000U);

    while (true) {
        watchdog_supervisor_beat(main_heartbeat);
        if (!media_transfer_is_active() && !app_controller_is_camera_ready()) {
            err = sample_and_log_time(&ctx, cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Sample/log failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(log_ticks);
    }
}
