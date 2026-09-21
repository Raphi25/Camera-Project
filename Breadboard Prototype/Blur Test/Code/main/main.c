#include "blur.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#define CAPTURE_INTERVAL_US 3000000LL
#define CAPTURES_PER_CONDITION 10
#define CONDITION_DURATION_US (CAPTURE_INTERVAL_US * CAPTURES_PER_CONDITION)
#define EXPERIMENT_TRIALS 3
#define SERVO_NEUTRAL_US 1500
#define SERVO_NEUTRAL_SETTLE_MS 500
#define START_BUTTON_GPIO GPIO_NUM_5
#define ASSUMED_EXPOSURE_S (1.0f / 60.0f)
#define HORIZONTAL_FOCAL_PX (960.0f / (2.0f * tanf(35.0f * (float)M_PI / 180.0f)))
static bool motion_active;
static bool transfer_done;
static int64_t next_capture_us, profile_end_us;
static unsigned trial_index;
static bool baseline_active;
typedef struct { const char *name; unsigned amplitude_deg; float velocity_deg_s; unsigned period_ms; } motion_profile_t;
static const motion_profile_t profiles[] = {
    /* Velocity sweep at a fixed +/-30 degree amplitude. Period is the full
       left-to-right-to-left cycle: T = 2*pi*A / angular velocity. */
    {"v010_a30",30,10.0f,18850},
    {"v025_a30",30,25.0f,7540},
    {"v050_a30",30,50.0f,3770},
    {"v100_a30",30,100.0f,1885},
    {"v150_a30",30,150.0f,1257},
};
static unsigned profile_index;
static bool burst_mode=true;
static void command(const char *line);

static esp_err_t capture_selected(const char *kind)
{
    /* The main loop is normally idle and unsubscribed from the task watchdog.
       Subscribe only around capture, whose driver calls reset the watchdog. */
    bool subscribed=(esp_task_wdt_status(NULL)==ESP_OK);
    if (!subscribed && esp_task_wdt_add(NULL)==ESP_OK) subscribed=true;
    esp_err_t err=burst_mode ? capture_save_best_burst(kind) : capture_save(kind);
    if (subscribed && esp_task_wdt_status(NULL)==ESP_OK) {
        (void)esp_task_wdt_delete(NULL);
    }
    return err;
}

static esp_err_t wait_for_start_button(void)
{
    gpio_config_t config={.pin_bit_mask=1ULL<<START_BUTTON_GPIO,
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&config),"button","GPIO5 configuration");
    int initial=gpio_get_level(START_BUTTON_GPIO);
    printf("Ready. GPIO5 initial level=%d; press the button to change its level and start.\n", initial);
    /* Ignore the boot level. Start only after a later debounced transition,
       supporting either active-low or active-high wiring. */
    char line[96]; size_t used=0; bool overflow=false;
    while (storage_usb_active() || gpio_get_level(START_BUTTON_GPIO)==initial) {
        if (storage_usb_active()) initial=gpio_get_level(START_BUTTON_GPIO);
        int ch=getchar();
        if (ch==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (ch=='\r' || ch=='\n') {
            line[used]=0;
            if (!overflow && used) command(line);
            used=0; overflow=false;
        } else if (ch>=32 && ch<127 && !overflow) {
            if (used<sizeof(line)-1) line[used++]=(char)ch; else overflow=true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(START_BUTTON_GPIO)==initial) return wait_for_start_button();
    puts("Start button pressed; beginning motion matrix.");
    return ESP_OK;
}

static esp_err_t start_profile(void)
{
    const motion_profile_t *p=&profiles[profile_index];
    unsigned offset=(unsigned)lroundf((float)p->amplitude_deg*500.0f/60.0f);
    float velocity=p->velocity_deg_s;
    float focal=HORIZONTAL_FOCAL_PX;
    esp_err_t err=servo_position(SERVO_NEUTRAL_US);
    if (err==ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SERVO_NEUTRAL_SETTLE_MS));
        storage_set_motion_metrics((float)p->amplitude_deg,velocity,ASSUMED_EXPOSURE_S,focal);
        next_capture_us=esp_timer_get_time(); profile_end_us=next_capture_us+CONDITION_DURATION_US;
        err=servo_sweep_until(SERVO_NEUTRAL_US-offset,SERVO_NEUTRAL_US+offset,
                              p->period_ms,profile_end_us);
    }
    if (err==ESP_OK) ESP_LOGI("motion","Trial %u/%u, profile %u/%u: %s +/- %u deg, %u ms cycle, max %.1f deg/s, 10 captures, expected blur %.2f px (assumes 1/60 s, fpx %.0f)", trial_index+1,EXPERIMENT_TRIALS,profile_index+1,(unsigned)(sizeof(profiles)/sizeof(profiles[0])),p->name,p->amplitude_deg,p->period_ms,velocity,(velocity*(float)M_PI/180.0f)*ASSUMED_EXPOSURE_S*focal,focal);
    return err;
}

static esp_err_t start_baseline(void)
{
    float focal=HORIZONTAL_FOCAL_PX;
    esp_err_t err=servo_position(SERVO_NEUTRAL_US);
    if (err==ESP_OK) {
        storage_set_motion_metrics(0.0f,0.0f,ASSUMED_EXPOSURE_S,focal);
        next_capture_us=esp_timer_get_time(); profile_end_us=next_capture_us+CONDITION_DURATION_US;
        ESP_LOGI("motion","Trial %u/%u baseline: stationary, 10 captures",trial_index+1,EXPERIMENT_TRIALS);
    }
    return err;
}

static esp_err_t motion_start(void)
{
    esp_err_t err=servo_position(SERVO_NEUTRAL_US);
    if (err==ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SERVO_NEUTRAL_SETTLE_MS));
        trial_index=0; profile_index=0; baseline_active=true;
        err=start_baseline();
    }
    if (err==ESP_OK) {
        motion_active=true;
        puts("Three trials started: stationary baseline plus 10/25/50/100/150 deg/s; 10 photos per condition, every 3 seconds.");
    }
    return err;
}

static void motion_stop(void)
{
    motion_active=false;
    servo_stop();
    puts("Motion capture stopped.");
}

static void motion_poll(void)
{
    if (motion_active && esp_timer_get_time()>=profile_end_us) {
        servo_stop();
        if (baseline_active) {
            baseline_active=false;
            if (start_profile()!=ESP_OK) { motion_stop(); return; }
        } else {
            profile_index++;
            if (profile_index < sizeof(profiles)/sizeof(profiles[0])) {
                if (start_profile()!=ESP_OK) { motion_stop(); return; }
            } else {
                trial_index++;
                if (trial_index >= EXPERIMENT_TRIALS) {
                    motion_stop();
                    if (!transfer_done) { transfer_done=true; puts("Experiment complete: 3 trials, 180 images planned. Use Retrieve Media to download images."); }
                    return;
                }
                profile_index=0; baseline_active=true;
                if (start_baseline()!=ESP_OK) { motion_stop(); return; }
            }
        }
    }
    if (motion_active && esp_timer_get_time()>=next_capture_us) {
        ESP_LOGI("motion","Starting scheduled capture");
        esp_err_t err=capture_selected(
            baseline_active ? "baseline_a0" : profiles[profile_index].name);
        if (err!=ESP_OK) {
            ESP_LOGE("motion","Capture failed: %s",esp_err_to_name(err));
            motion_stop();
            return;
        }
        /* Keep start-to-start cadence; skip overruns instead of taking a burst. */
        next_capture_us+=CAPTURE_INTERVAL_US;
        int64_t now=esp_timer_get_time();
        if (next_capture_us<=now) {
            int64_t missed=(now-next_capture_us)/CAPTURE_INTERVAL_US+1;
            next_capture_us+=missed*CAPTURE_INTERVAL_US;
            ESP_LOGW("motion","Capture exceeded interval; skipped %lld slot(s)",(long long)missed);
        }
    }
}

static void help(void)
{
    puts("Commands (newline terminated):\n"
         "  snap                       Save a still at the current servo state\n"
         "  servo 1500                 Hold pulse width, 1000..2000 us\n"
         "  sweep 1300 1700 2000       Min/max us, full cycle ms (400..60000)\n"
         "  stop                       Stop automatic capture and disable servo pulses\n"
         "  mode normal                Use one exposure per scheduled capture\n"
         "  mode burst                 Capture 3 exposures and save the sharpest\n"
         "  test 5                     Stationary baseline + 5 moving images (1..100)\n"
         "  help\n"
         "  USB_MSC_START              Export SD on the OTG USB-C port\n"
         "  USB_MSC_STOP               Return SD after host safely ejects it\n"
         "Experiment starts after the physical button is pressed.");
}

static esp_err_t run_test(unsigned count)
{
    esp_err_t err=servo_position(SERVO_NEUTRAL_US);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (err==ESP_OK) err=capture_selected("baseline");
    if (err==ESP_OK) err=servo_sweep(1300,1700,2000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    for (unsigned i=0;i<count && err==ESP_OK;i++) err=capture_selected("motion");
    servo_stop();
    return err;
}

static void command(const char *line)
{
    char name[16],extra;
    unsigned a,b,c;
    esp_err_t err=ESP_ERR_INVALID_ARG;
    if (sscanf(line,"%15s",name)!=1) return;
    if (strcmp(line,"help")==0) { help(); return; }
    if (strcmp(line,"stop")==0) { motion_stop(); return; }
    if (strcmp(name,"mode")==0) {
        if (strcmp(line,"mode normal")==0) { burst_mode=false; puts("Capture mode: normal (single exposure)."); }
        else if (strcmp(line,"mode burst")==0) { burst_mode=true; puts("Capture mode: burst (3 exposures; sharpest saved)."); }
        else puts("Usage: mode normal | mode burst");
        return;
    }
    if (motion_active) { puts("MEDIA_BUSY: Experiment running; wait until it finishes."); return; }
    if (strcmp(line,"USB_MSC_START")==0) {
        err=storage_usb_start();
        if (err==ESP_OK) printf("MSC_READY %s\n",storage_device_id());
        else printf("MEDIA_ERROR: Cannot export SD (%s). Connect the OTG USB-C cable.\n",esp_err_to_name(err));
        return;
    }
    if (strcmp(line,"USB_MSC_STOP")==0) {
        err=storage_usb_stop();
        puts(err==ESP_OK ? "MSC_STOPPED" : "MEDIA_ERROR: SD remount failed; reset after safely ejecting the drive.");
        return;
    }
    if (storage_usb_active()) { puts("MEDIA_BUSY: SD card belongs to USB host."); return; }
    if (strcmp(line,"snap")==0) err=capture_selected("snap");
    else if (strcmp(name,"servo")==0 && sscanf(line,"%*s %u %c",&a,&extra)==1)
        err=servo_position(a);
    else if (strcmp(name,"sweep")==0 && sscanf(line,"%*s %u %u %u %c",&a,&b,&c,&extra)==3)
        err=servo_sweep(a,b,c);
    else if (strcmp(name,"test")==0 && sscanf(line,"%*s %u %c",&a,&extra)==1 && a>=1 && a<=100)
        err=run_test(a);
    if (err!=ESP_OK) servo_stop();
    printf("Result: %s\n",esp_err_to_name(err));
}

void app_main(void)
{
    setvbuf(stdin,NULL,_IONBF,0);
    setvbuf(stdout,NULL,_IONBF,0);
    esp_err_t err=servo_init();
    const char *startup_stage="servo";
    if (err==ESP_OK) startup_stage="SD card";
    if (err==ESP_OK) err=storage_init();
    if (err==ESP_OK) startup_stage="camera";
    if (err==ESP_OK) err=capture_init();
    if (err!=ESP_OK) {
        ESP_LOGE("blur","Startup failed: %s. Check camera/SD and reboot.",esp_err_to_name(err));
        /* Keep draining USB input even when initialization fails, so the GUI
           gets the actual error instead of eventually blocking on a full FIFO. */
        bool received=false;
        for (;;) {
            int ch=getchar();
            if (ch==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            if (ch=='\r' || ch=='\n') {
                if (received) printf("MEDIA_ERROR: %s initialization failed (%s). Check its connection and reset the board.\n",startup_stage,esp_err_to_name(err));
                received=false;
            } else received=true;
        }
    }
    help();
    err=wait_for_start_button();
    if (err!=ESP_OK) { ESP_LOGE("button","Start button setup failed: %s",esp_err_to_name(err)); return; }
    err=motion_start();
    if (err!=ESP_OK) { ESP_LOGE("motion","Start failed: %s",esp_err_to_name(err)); return; }
    char line[96]; size_t used=0; bool overflow=false;
    for (;;) {
        motion_poll();
        int ch=getchar();
        if (ch==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (ch=='\r' || ch=='\n') {
            line[used]=0;
            if (overflow) puts("Command too long; discarded.");
            else if (used) command(line);
            used=0; overflow=false;
        } else if (ch==8 || ch==127) { if (used && !overflow) used--; }
        else if (ch>=32 && ch<127 && !overflow) {
            if (used<sizeof(line)-1) line[used++]=(char)ch;
            else overflow=true;
        }
    }
}
