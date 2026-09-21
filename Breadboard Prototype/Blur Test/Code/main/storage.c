#include "blur.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "esp_mac.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_vfs_fat.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG="storage";
static char directory[64];
static uint32_t sequence;
static uint8_t *chunk;
static sdmmc_card_t card;
static tinyusb_msc_storage_handle_t usb_storage;
static bool usb_owned;
static char device_id[32];
typedef struct {
    float amplitude_deg;
    float velocity_deg_s;
    float exposure_s;
    float focal_px;
} motion_metrics_t;

typedef struct {
    uint8_t *data;
    size_t size;
    char kind[32];
    uint32_t width, height;
    int64_t frame_us;
    motion_metrics_t metrics;
} storage_job_t;
static QueueHandle_t storage_queue;
static volatile bool storage_busy;
static void storage_writer_task(void *arg);

bool storage_usb_active(void) { return usb_owned; }
const char *storage_device_id(void) { return device_id; }

esp_err_t storage_usb_start(void)
{
    ESP_RETURN_ON_FALSE(usb_storage, ESP_ERR_INVALID_STATE, TAG, "SD unavailable");
    if (usb_owned) {
        tinyusb_msc_mount_point_t owner;
        ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(usb_storage,&owner),TAG,"Check SD owner");
        if (owner==TINYUSB_MSC_STORAGE_MOUNT_USB) return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(tud_connected(), ESP_ERR_INVALID_STATE, TAG, "Connect OTG USB-C cable");
    ESP_RETURN_ON_ERROR(storage_wait_idle(10000), TAG, "Wait for SD writer");
    /* The command task owns all file I/O and invokes this only while idle. */
    usb_owned=true; /* Keep capture blocked even if a partial handoff fails. */
    return tinyusb_msc_set_storage_mount_point(usb_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
}

esp_err_t storage_usb_stop(void)
{
    ESP_RETURN_ON_FALSE(usb_storage, ESP_ERR_INVALID_STATE, TAG, "SD unavailable");
    ESP_RETURN_ON_ERROR(tinyusb_msc_set_storage_mount_point(
        usb_storage,TINYUSB_MSC_STORAGE_MOUNT_APP),TAG,"Return SD to application");
    tinyusb_msc_mount_point_t owner;
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(usb_storage,&owner),TAG,"Check SD owner");
    ESP_RETURN_ON_FALSE(owner==TINYUSB_MSC_STORAGE_MOUNT_APP,ESP_FAIL,TAG,"SD still on USB");
    usb_owned=false;
    return ESP_OK;
}

static motion_metrics_t current_metrics;

void storage_set_motion_metrics(float amplitude_deg, float velocity_deg_s,
                                float exposure_s, float focal_px)
{
    current_metrics=(motion_metrics_t){
        .amplitude_deg=amplitude_deg,
        .velocity_deg_s=velocity_deg_s,
        .exposure_s=exposure_s,
        .focal_px=focal_px,
    };
}

esp_err_t storage_init(void)
{
    gpio_config_t gpio={.pin_bit_mask=1ULL<<45,.mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&gpio),TAG,"SD power GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(45,0),TAG,"SD power on");
    vTaskDelay(pdMS_TO_TICKS(100));
    sd_pwr_ctrl_ldo_config_t ldo={.ldo_chan_id=4};
    sd_pwr_ctrl_handle_t power=NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo,&power),TAG,"SD LDO");
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();
    host.slot=SDMMC_HOST_SLOT_0; host.max_freq_khz=SDMMC_FREQ_DEFAULT;
    host.pwr_ctrl_handle=power;
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width=4; slot.clk=43; slot.cmd=44;
    slot.d0=39; slot.d1=40; slot.d2=41; slot.d3=42;
    slot.flags|=SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ESP_RETURN_ON_ERROR(sdmmc_host_init(),TAG,"SD host");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot,&slot),TAG,"SD slot");
    ESP_RETURN_ON_ERROR(sdmmc_card_init(&host,&card),TAG,"SD card");
    tinyusb_msc_driver_config_t driver={.user_flags.auto_mount_off=1};
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver),TAG,"MSC driver");
    tinyusb_msc_storage_config_t storage={
        .medium.card=&card,
        .fat_fs={.base_path="/sdcard",
            .config={.format_if_mount_failed=false,.max_files=5,.allocation_unit_size=16*1024},
            .do_not_format=true},
        .mount_point=TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage,&usb_storage),TAG,"Mount SD via TinyUSB");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac),TAG,"Device identity");
    snprintf(device_id,sizeof(device_id),"BLUR_%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    FILE *identity=fopen("/sdcard/BLUR_DEVICE.TXT","w");
    ESP_RETURN_ON_FALSE(identity,ESP_FAIL,TAG,"Write USB identity");
    bool id_ok=fprintf(identity,"%s\n",device_id)>0;
    if (fflush(identity)!=0 || fsync(fileno(identity))!=0) id_ok=false;
    if (fclose(identity)!=0) id_ok=false;
    ESP_RETURN_ON_FALSE(id_ok,ESP_FAIL,TAG,"Flush USB identity");
    tinyusb_config_t usb=TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb),TAG,"OTG USB driver");
    if (mkdir("/sdcard/blur",0777)!=0 && errno!=EEXIST) return ESP_FAIL;
    for (unsigned i=0;i<100000;i++) {
        snprintf(directory,sizeof(directory),"/sdcard/blur/run_%05u",i);
        if (mkdir(directory,0777)==0) break;
        if (errno!=EEXIST || i==99999) return ESP_FAIL;
    }
    chunk=heap_caps_aligned_alloc(64,4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(chunk,ESP_ERR_NO_MEM,TAG,"SD staging buffer");
    storage_queue=xQueueCreate(4,sizeof(storage_job_t));
    ESP_RETURN_ON_FALSE(storage_queue,ESP_ERR_NO_MEM,TAG,"SD writer queue");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(storage_writer_task,"sd_writer",8192,NULL,2,NULL,1)==pdPASS,
                        ESP_ERR_NO_MEM,TAG,"SD writer task");
    ESP_LOGI(TAG,"Saving plain JPEGs in %s",directory);
    return ESP_OK;
}

static esp_err_t storage_save_sync(const void *data,size_t size,const char *kind,
                       uint32_t width,uint32_t height,int64_t frame_us,
                       const motion_metrics_t *metrics)
{
    ESP_RETURN_ON_FALSE(!usb_owned,ESP_ERR_INVALID_STATE,TAG,"SD belongs to USB host");
    char path[112],temporary[116];
    snprintf(path,sizeof(path),"%s/%06lu_%s.jpg",directory,(unsigned long)sequence++,kind);
    snprintf(temporary,sizeof(temporary),"%s.tmp",path);
    FILE *f=fopen(temporary,"wb");
    if (!f) return ESP_FAIL;
    bool ok=true;
    size_t chunks=0;
    for (size_t offset=0;offset<size;) {
        /* Short writes keep the SD/FAT driver from holding the capture task
           in one long transaction on cards that occasionally pause. */
        size_t n=size-offset; if (n>4096) n=4096;
        memcpy(chunk,(const uint8_t *)data+offset,n);
        if (fwrite(chunk,1,n,f)!=n) { ok=false; break; }
        offset+=n;
        (void)esp_task_wdt_reset();
        if ((++chunks & 15U)==0U && fflush(f)!=0) { ok=false; break; }
        vTaskDelay(1);
    }
    if (fflush(f)!=0) ok=false;
    /* Do not call fsync for every image. On this board an SD-card sync can
       block the main task for longer than the watchdog period after several
       consecutive captures. fclose still flushes the FAT data, and the
       temporary-file rename prevents partially written images being exposed. */
    if (fclose(f)!=0) ok=false;
    if (!ok || rename(temporary,path)!=0) { unlink(temporary); return ESP_FAIL; }
    char csv[120]; snprintf(csv,sizeof(csv),"%s.csv",path);
    FILE *m=fopen(csv,"w");
    if (m) {
        float blur_px=(metrics->velocity_deg_s*(float)M_PI/180.0f)*
                      metrics->exposure_s*metrics->focal_px;
        fprintf(m,"amplitude_deg,velocity_deg_s,exposure_s,focal_px,expected_blur_px,frame_us,width,height\n");
        fprintf(m,"%.3f,%.3f,%.6f,%.1f,%.3f,%lld,%lu,%lu\n",
                metrics->amplitude_deg,metrics->velocity_deg_s,
                metrics->exposure_s,metrics->focal_px,blur_px,
                (long long)frame_us,(unsigned long)width,(unsigned long)height);
        fclose(m);
    }
    ESP_LOGI(TAG,"Saved %s (%u bytes, %lux%lu, dequeue_us=%lld)",path,
        (unsigned)size,(unsigned long)width,(unsigned long)height,(long long)frame_us);
    return ESP_OK;
}

static void storage_writer_task(void *arg)
{
    /* This task is the only code that writes capture files. A queued JPEG owns
       its PSRAM allocation until this task writes and frees it. */
    (void)arg;
    storage_job_t job;
    for (;;) {
        if (xQueueReceive(storage_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        storage_busy=true;
        esp_err_t err=storage_save_sync(job.data,job.size,job.kind,
                                        job.width,job.height,job.frame_us,&job.metrics);
        if (err!=ESP_OK) ESP_LOGE(TAG,"Queued image write failed: %s",esp_err_to_name(err));
        heap_caps_free(job.data);
        storage_busy=false;
    }
}

esp_err_t storage_save(const void *data,size_t size,const char *kind,
                       uint32_t width,uint32_t height,int64_t frame_us)
{
    ESP_RETURN_ON_FALSE(storage_queue,ESP_ERR_INVALID_STATE,TAG,"SD writer unavailable");
    ESP_RETURN_ON_FALSE(!usb_owned,ESP_ERR_INVALID_STATE,TAG,"SD belongs to USB host");
    storage_job_t job={0};
    job.data=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(job.data,ESP_ERR_NO_MEM,TAG,"queued JPEG copy");
    memcpy(job.data,data,size);
    job.size=size; job.width=width; job.height=height; job.frame_us=frame_us;
    /* Snapshot metadata with the JPEG. The writer can run after the main task
       has already advanced to the next velocity profile. */
    job.metrics=current_metrics;
    strlcpy(job.kind,kind,sizeof(job.kind));
    if (xQueueSend(storage_queue,&job,pdMS_TO_TICKS(1000))!=pdTRUE) {
        heap_caps_free(job.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t storage_wait_idle(uint32_t timeout_ms)
{
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    while (storage_busy || (storage_queue && uxQueueMessagesWaiting(storage_queue))) {
        if (esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}
