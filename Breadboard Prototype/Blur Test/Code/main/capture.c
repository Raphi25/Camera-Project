/* Reduced from the SC2336 and OV5647 prototypes: CSI -> ISP -> RGB565 -> JPEG. */
#include "blur.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Keep one spare buffer queued while retaining the three burst candidates.
   Without it, the CSI driver can stall before delivering candidate 3. */
#define BUFFERS 4
static const char *TAG="capture";
static int fd=-1;
static void *frames[BUFFERS];
static size_t lengths[BUFFERS];
static uint32_t width, height;
static jpeg_encoder_handle_t encoder;
static uint8_t *jpeg;
static size_t jpeg_capacity;
static bool streaming, buffers_queued, failed;

static esp_err_t stream(bool on)
{
    if (on == streaming) return ESP_OK;
    if (on) {
        if (!buffers_queued) {
            for (unsigned i=0; i<BUFFERS; i++) {
                struct v4l2_buffer b={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory=V4L2_MEMORY_MMAP, .index=i};
                if (ioctl(fd, VIDIOC_QBUF, &b)) return ESP_FAIL;
            }
            buffers_queued=true;
        }
    }
    int type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type)) return ESP_FAIL;
    streaming=on;
    if (!on) buffers_queued=false;
    return ESP_OK;
}

static esp_err_t dequeue(struct v4l2_buffer *b)
{
    *b=(struct v4l2_buffer){.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,.memory=V4L2_MEMORY_MMAP};
    int64_t deadline=esp_timer_get_time()+3000000;
    while (esp_timer_get_time()<deadline) {
        /* Camera dequeue can wait while the sensor produces a frame. */
        (void)esp_task_wdt_reset();
        if (ioctl(fd, VIDIOC_DQBUF, b)==0)
            return b->index<BUFFERS && b->bytesused>0 && b->bytesused<=lengths[b->index]
                && !(b->flags & V4L2_BUF_FLAG_ERROR) ? ESP_OK : ESP_FAIL;
        if (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t capture_init(void)
{
    esp_video_init_csi_config_t csi={
        .sccb_config={.init_sccb=true, .i2c_config={.port=0,.sda_pin=7,.scl_pin=8},
                      .freq=100000}, .reset_pin=-1,.pwdn_pin=-1};
    esp_video_init_config_t config={.csi=&csi};
    /* Both enabled drivers probe their chip ID; esp_video selects the matching
       sensor and its own default format and IPA tuning profile. */
    ESP_RETURN_ON_ERROR(esp_video_init(&config),TAG,"camera detection/init");
    fd=open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,O_RDONLY|O_NONBLOCK);
    ESP_RETURN_ON_FALSE(fd>=0,ESP_FAIL,TAG,"open CSI");
    struct v4l2_format fmt={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE};
    ESP_RETURN_ON_FALSE(ioctl(fd,VIDIOC_G_FMT,&fmt)==0,ESP_FAIL,TAG,"get format");
    fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_RGB565;
    ESP_RETURN_ON_FALSE(ioctl(fd,VIDIOC_S_FMT,&fmt)==0,ESP_FAIL,TAG,"RGB565");
    ESP_RETURN_ON_FALSE(ioctl(fd,VIDIOC_G_FMT,&fmt)==0 &&
        fmt.fmt.pix.pixelformat==V4L2_PIX_FMT_RGB565,ESP_FAIL,TAG,"verify format");
    width=fmt.fmt.pix.width; height=fmt.fmt.pix.height;
    struct v4l2_requestbuffers req={.count=BUFFERS,.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                  .memory=V4L2_MEMORY_MMAP};
    ESP_RETURN_ON_FALSE(ioctl(fd,VIDIOC_REQBUFS,&req)==0 && req.count==BUFFERS,
                        ESP_FAIL,TAG,"buffers");
    for (unsigned i=0;i<BUFFERS;i++) {
        struct v4l2_buffer b={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
                             .memory=V4L2_MEMORY_MMAP,.index=i};
        ESP_RETURN_ON_FALSE(ioctl(fd,VIDIOC_QUERYBUF,&b)==0,ESP_FAIL,TAG,"query buffer");
        lengths[i]=b.length;
        frames[i]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
        ESP_RETURN_ON_FALSE(frames[i]!=MAP_FAILED && frames[i],ESP_ERR_NO_MEM,TAG,"mmap");
    }
    jpeg_encode_engine_cfg_t engine={.timeout_ms=30000};
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine,&encoder),TAG,"JPEG engine");
    jpeg_encode_memory_alloc_cfg_t mem={.buffer_direction=JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    jpeg=jpeg_alloc_encoder_mem(3*1024*1024,&mem,&jpeg_capacity);
    ESP_RETURN_ON_FALSE(jpeg,ESP_ERR_NO_MEM,TAG,"JPEG output");
    ESP_LOGI(TAG,"Ready: %lux%lu RGB565; sensor identity is in detection log",
             (unsigned long)width,(unsigned long)height);
    return ESP_OK;
}

esp_err_t capture_save(const char *kind)
{
    if (failed) return ESP_ERR_INVALID_STATE;
    int64_t started_us=esp_timer_get_time();
    ESP_LOGI(TAG,"capture %s: stream on",kind);
    esp_err_t err=stream(true);
    struct v4l2_buffer b={0};
    /* Discard restart frames for AE/AWB and ensure no stale pre-motion frame. */
    for (unsigned i=0;err==ESP_OK && i<5;i++) {
        err=dequeue(&b);
        if (err==ESP_OK && ioctl(fd,VIDIOC_QBUF,&b)) err=ESP_FAIL;
        (void)esp_task_wdt_reset();
    }
    ESP_LOGI(TAG,"capture %s: settling complete (%lld ms)",kind,
             (long long)((esp_timer_get_time()-started_us)/1000));
    if (err==ESP_OK) err=dequeue(&b);
    int64_t frame_us=esp_timer_get_time();
    if (err!=ESP_OK) {
        failed=true; /* Unknown queue ownership: require reboot, never double-queue. */
        return err;
    }
    ESP_LOGI(TAG,"capture %s: frame ready; encoding",kind);
    (void)esp_task_wdt_reset();
    ESP_RETURN_ON_ERROR(esp_cache_msync(frames[b.index],b.bytesused,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C),TAG,"DMA cache sync");
    /* ESP32-P4 rev 1 cannot reliably run CSI/ISP and JPEG GDMA together. */
    ESP_RETURN_ON_ERROR(stream(false),TAG,"stop CSI before JPEG");
    jpeg_encode_cfg_t cfg={.src_type=JPEG_ENC_SRC_RGB565,
        .sub_sample=JPEG_DOWN_SAMPLING_YUV420,.image_quality=90,
        .width=width,.height=height};
    uint32_t bytes=0;
    ESP_RETURN_ON_ERROR(jpeg_encoder_process(encoder,&cfg,frames[b.index],b.bytesused,
                        jpeg,jpeg_capacity,&bytes),TAG,"encode");
    ESP_LOGI(TAG,"capture %s: encoded %lu bytes; queueing",kind,(unsigned long)bytes);
    (void)esp_task_wdt_reset();
    esp_err_t save=storage_save(jpeg,bytes,kind,width,height,frame_us);
    (void)esp_task_wdt_reset();
    ESP_LOGI(TAG,"capture %s: queued (%lld ms)",kind,
             (long long)((esp_timer_get_time()-started_us)/1000));
    return save;
}

static inline unsigned rgb565_luma(const uint8_t *row, unsigned x)
{
    uint16_t pixel=(uint16_t)row[2*x] | ((uint16_t)row[2*x+1]<<8);
    unsigned r=(pixel>>11)&0x1f, g=(pixel>>5)&0x3f, b=pixel&0x1f;
    return (77*(r*255/31)+150*(g*255/63)+29*(b*255/31))>>8;
}

/* A fast Tenengrad-like score on the raw frame. It is used only to rank the
   three consecutive exposures; the selected frame is still JPEG encoded by
   the normal path below. Sampling every four pixels keeps this bounded on
   both 1920x1080 and 1280x960 sensors. */
static uint64_t frame_detail_score(const uint8_t *data, size_t bytes)
{
    size_t needed=(size_t)width*height*2;
    if (bytes<needed) return 0;
    uint64_t score=0;
    for (unsigned y=2;y+2<height;y+=4) {
        (void)esp_task_wdt_reset();
        const uint8_t *row=data+(size_t)y*width*2;
        const uint8_t *up=data+(size_t)(y-2)*width*2;
        const uint8_t *down=data+(size_t)(y+2)*width*2;
        for (unsigned x=2;x+2<width;x+=4) {
            int gx=(int)rgb565_luma(row,x+2)-(int)rgb565_luma(row,x-2);
            int gy=(int)rgb565_luma(down,x)-(int)rgb565_luma(up,x);
            score+=(uint64_t)(gx*gx+gy*gy);
        }
    }
    return score;
}

esp_err_t capture_save_best_burst(const char *kind)
{
    if (failed) return ESP_ERR_INVALID_STATE;
    int64_t started_us=esp_timer_get_time();
    ESP_LOGI(TAG,"burst %s: stream on; acquiring 3 consecutive frames",kind);
    esp_err_t err=stream(true);
    struct v4l2_buffer b={0};
    for (unsigned i=0;err==ESP_OK && i<5;i++) {
        err=dequeue(&b);
        if (err==ESP_OK && ioctl(fd,VIDIOC_QBUF,&b)) err=ESP_FAIL;
        (void)esp_task_wdt_reset();
    }
    struct v4l2_buffer burst[3]={0};
    int64_t frame_times[3]={0};
    uint64_t scores[3]={0};
    unsigned count=0;
    while (err==ESP_OK && count<3) {
        err=dequeue(&burst[count]);
        if (err==ESP_OK) {
            frame_times[count]=esp_timer_get_time();
            (void)esp_cache_msync(frames[burst[count].index],burst[count].bytesused,
                                   ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            ESP_LOGI(TAG,"burst %s: frame %u acquired",kind,count+1);
            ++count;
        }
        (void)esp_task_wdt_reset();
    }
    esp_err_t stop=stream(false);
    unsigned best=0;
    if (err!=ESP_OK || stop!=ESP_OK || count!=3) {
        failed=true;
        return err!=ESP_OK ? err : (stop!=ESP_OK ? stop : ESP_FAIL);
    }
    for (unsigned i=0;i<3;i++) {
        scores[i]=frame_detail_score(frames[burst[i].index],burst[i].bytesused);
        ESP_LOGI(TAG,"burst %s: frame %u detail=%llu",kind,i+1,
                 (unsigned long long)scores[i]);
    }
    for (unsigned i=1;i<3;i++) if (scores[i]>scores[best]) best=i;
    ESP_LOGI(TAG,"burst %s: detail scores %llu, %llu, %llu; selected %u",
             kind,(unsigned long long)scores[0],(unsigned long long)scores[1],
             (unsigned long long)scores[2],best+1);
    (void)esp_task_wdt_reset();
    jpeg_encode_cfg_t cfg={.src_type=JPEG_ENC_SRC_RGB565,
        .sub_sample=JPEG_DOWN_SAMPLING_YUV420,.image_quality=90,
        .width=width,.height=height};
    uint32_t bytes=0;
    struct v4l2_buffer *chosen=&burst[best];
    err=jpeg_encoder_process(encoder,&cfg,frames[chosen->index],chosen->bytesused,
                             jpeg,jpeg_capacity,&bytes);
    if (err!=ESP_OK) return err;
    (void)esp_task_wdt_reset();
    esp_err_t save=storage_save(jpeg,bytes,kind,width,height,frame_times[best]);
    (void)esp_task_wdt_reset();
    ESP_LOGI(TAG,"burst %s: queued selected frame in %lld ms",kind,
             (long long)((esp_timer_get_time()-started_us)/1000));
    return save;
}
