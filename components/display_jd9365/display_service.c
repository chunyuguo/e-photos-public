#include "display_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "clock_service.h"
#include "display_lvgl_backend.h"
#include "boot_image_service.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gallery_service.h"
#include "hal/color_types.h"
#include "jerror.h"
#include "jpeglib.h"
#include "png.h"
#include "storage_service.h"
#include "settings_service.h"

static const char *TAG = "display_service";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb888_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t aligned_width;
    uint16_t aligned_height;
    uint16_t *pixels;
} decoded_jpeg_t;

typedef enum {
    IMAGE_TYPE_UNKNOWN = 0,
    IMAGE_TYPE_JPEG,
    IMAGE_TYPE_PNG,
    IMAGE_TYPE_BMP,
} image_type_t;

typedef struct {
    uint32_t source_width;
    uint32_t source_height;
    int logical_width;
    int logical_height;
    int render_width;
    int render_height;
    int offset_x;
    int offset_y;
    uint32_t *source_x0_map;
    uint32_t *source_x1_map;
    uint16_t *source_x_weight_map;
    uint32_t *source_y0_map;
    uint32_t *source_y1_map;
    uint16_t *source_y_weight_map;
} image_render_plan_t;

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    char message[JMSG_LENGTH_MAX];
} software_jpeg_error_mgr_t;

static const board_profile_t *s_profile;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static esp_ldo_channel_handle_t s_dsi_phy_ldo;
static SemaphoreHandle_t s_display_mutex;
static jpeg_encoder_handle_t s_jpeg_encoder;
static color_pixel_rgb565_data_t *s_frame_buffer;
static color_pixel_rgb565_data_t *s_base_buffer;
static color_pixel_rgb565_data_t *s_stage_buffer;
static color_pixel_rgb565_data_t *s_retired_buffers[2];
static uint32_t s_retired_generations[2];
static size_t s_framebuffer_pixels;
static size_t s_framebuffer_bytes;
static jpeg_decoder_handle_t s_jpeg_decoder;
static uint8_t *s_jpeg_input_buffer;
static size_t s_jpeg_input_capacity;
static uint8_t *s_jpeg_output_buffer;
static size_t s_jpeg_output_capacity;
static bool s_initialized;
static bool s_display_on;
static uint16_t s_rotation_deg = UINT16_MAX;
static ephoto_brightness_t s_brightness = (ephoto_brightness_t)-1;
static bool s_applied_screen_on_valid;
static bool s_applied_screen_on;
static uint16_t s_base_photo_rotation_deg = UINT16_MAX;
static ephoto_fit_mode_t s_base_photo_fit_mode = (ephoto_fit_mode_t)-1;
static char s_base_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN];
static uint32_t s_buffer_generation;
static uint32_t s_last_rendered_generation;
static bool s_render_busy;
static uint8_t *s_file_io_buffer;
static bool s_backlight_pwm_ready;
static const esp_partition_t *s_bootimg_partition;
static const esp_partition_t *s_bootimg_default_partition;
static const void *s_bootimg_pixels;

static uint16_t resolve_startup_rotation_deg(const board_profile_t *profile,
                                             const ephoto_settings_t *settings,
                                             bool have_settings)
{
    uint16_t manual_rotation = profile ? profile->default_rotation_deg : 0U;
    bool auto_rotation_enabled = false;

    if (have_settings && settings) {
        manual_rotation = settings->manual_rotation_deg == 90 ? 90U : 0U;
        auto_rotation_enabled = settings->auto_rotation_enabled;
    }

    if (!auto_rotation_enabled ||
        !profile ||
        profile->rotation_switch_gpio == GPIO_NUM_NC) {
        return manual_rotation;
    }

    int level = gpio_get_level(profile->rotation_switch_gpio);
    bool active = profile->rotation_switch_active_low ? (level == 0) : (level != 0);
    return active ? 0U : 90U;
}

#define EPHOTO_BOOT_IMAGE_WIDTH 1280U
#define EPHOTO_BOOT_IMAGE_HEIGHT 800U
#define EPHOTO_BOOT_IMAGE_SIZE_BYTES ((size_t)EPHOTO_BOOT_IMAGE_WIDTH * EPHOTO_BOOT_IMAGE_HEIGHT * 2U)
#define EPHOTO_BOOT_IMAGE_PARTITION_LABEL "bootimg_b"
#define EPHOTO_BOOT_IMAGE_DEFAULT_PARTITION_LABEL "bootimg_a"

#define EPHOTO_BACKLIGHT_PWM_FREQ_HZ   20000
#define EPHOTO_BACKLIGHT_PWM_RES       LEDC_TIMER_10_BIT
#define EPHOTO_BACKLIGHT_PWM_MODE      LEDC_LOW_SPEED_MODE
#define EPHOTO_BACKLIGHT_PWM_TIMER     LEDC_TIMER_0
#define EPHOTO_BACKLIGHT_PWM_CHANNEL   LEDC_CHANNEL_0
#define EPHOTO_BACKLIGHT_PWM_DUTY_MAX  ((1U << 10) - 1U)

#define EPHOTO_CACHE_MAGIC 0x31465045U
#define EPHOTO_CACHE_VERSION 7U
#define EPHOTO_CACHE_FAILURE_MAGIC 0x31464645U
#define EPHOTO_CACHE_FAILURE_VERSION 1U
#define EPHOTO_DISPLAY_CACHE_ENABLED 1
#define EPHOTO_CACHE_FORMAT_SCREEN_JPEG 1U
#define EPHOTO_CACHE_FORMAT_WEB_JPEG 2U
#define EPHOTO_SCREEN_CACHE_JPEG_QUALITY 92U
#define EPHOTO_WEB_THUMB_JPEG_QUALITY 86U
#define EPHOTO_SCREEN_CACHE_JPEG_MAX_BYTES (4U * 1024U * 1024U)
#define EPHOTO_WEB_THUMB_COVER_SIZE 240U
#define EPHOTO_WEB_THUMB_PREVIEW_MAX_EDGE 720U
#define EPHOTO_WEB_THUMB_MAX_BYTES (512U * 1024U)
#define EPHOTO_LARGE_JPEG_RELAX_AREA_PIXELS (12U * 1000U * 1000U)
#define EPHOTO_HW_JPEG_MAX_OUTPUT_BYTES (8U * 1024U * 1024U)
#define EPHOTO_NEW_JPEG_MIN_SOURCE_PIXELS (2U * 1000U * 1000U)
#define EPHOTO_LONG_DECODE_TIMEOUT_MS (90000U)
#define EPHOTO_CACHE_DEFER_SIZE_BYTES (4U * 1024U * 1024U)
#define EPHOTO_CACHE_DEFER_PIXELS (5U * 1000U * 1000U)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t format;
    uint16_t rotation_deg;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t reserved;
    uint64_t source_size;
    int64_t source_mtime;
    uint64_t source_hash;
    uint32_t payload_size;
    uint32_t payload_crc32;
} ephoto_cache_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t fit_mode;
    uint8_t reserved0;
    uint16_t rotation_deg;
    uint16_t reserved1;
    uint64_t source_size;
    int64_t source_mtime;
    uint64_t source_hash;
    int32_t error_code;
} ephoto_cache_failure_header_t;

typedef struct {
    bool valid;
    bool progressive_mode;
    bool arith_code;
    int num_components;
    J_COLOR_SPACE jpeg_color_space;
    uint32_t width;
    uint32_t height;
} jpeg_software_header_info_t;

void *ephoto_jpeg_external_alloc(size_t sizeofobject)
{
    void *ptr = heap_caps_malloc(sizeofobject, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr;
}

void ephoto_jpeg_external_free(void *object)
{
    heap_caps_free(object);
}

static void map_logical_to_physical(int logical_x, int logical_y, uint16_t rotation_deg, int *physical_x, int *physical_y);
static bool probe_jpeg_software_header(const char *path, jpeg_software_header_info_t *out_info);
static image_type_t sniff_image_type_from_file(const char *path);
static const char *choose_software_jpeg_reason(const jpeg_software_header_info_t *info);
static bool should_defer_foreground_render(const char *path,
                                           size_t size_bytes,
                                           uint32_t width,
                                           uint32_t height,
                                           ephoto_fit_mode_t fit_mode,
                                           uint16_t rotation_deg);
static esp_err_t build_cache_path_with_ext(const char *source_path,
                                           uint16_t rotation_deg,
                                           ephoto_fit_mode_t fit_mode,
                                           const char *ext,
                                           char *out_path,
                                           size_t out_len);
static esp_err_t build_failure_cache_path(const char *source_path,
                                          uint16_t rotation_deg,
                                          ephoto_fit_mode_t fit_mode,
                                          char *out_path,
                                          size_t out_len);
static esp_err_t build_web_thumbnail_cache_path(const char *source_path,
                                                ephoto_fit_mode_t fit_mode,
                                                char *out_path,
                                                size_t out_len);
static esp_err_t build_web_thumbnail_failure_path(const char *source_path,
                                                  ephoto_fit_mode_t fit_mode,
                                                  char *out_path,
                                                  size_t out_len);
static esp_err_t load_cache_failure_header(const char *source_path,
                                           uint16_t rotation_deg,
                                           ephoto_fit_mode_t fit_mode,
                                           ephoto_cache_failure_header_t *out_header,
                                           char *out_cache_path,
                                           size_t out_cache_path_len);
static esp_err_t load_web_thumbnail_header(const char *source_path,
                                           ephoto_fit_mode_t fit_mode,
                                           ephoto_cache_header_t *out_header,
                                           char *out_cache_path,
                                           size_t out_cache_path_len);
static esp_err_t load_web_thumbnail_failure_header(const char *source_path,
                                                   ephoto_fit_mode_t fit_mode,
                                                   ephoto_cache_failure_header_t *out_header,
                                                   char *out_cache_path,
                                                   size_t out_cache_path_len);
static void clear_cache_failure_marker(const char *source_path,
                                       uint16_t rotation_deg,
                                       ephoto_fit_mode_t fit_mode);
static void save_cache_failure_marker(const char *source_path,
                                      uint16_t rotation_deg,
                                      ephoto_fit_mode_t fit_mode,
                                      esp_err_t err_code);
static void clear_web_thumbnail_failure_marker(const char *source_path, ephoto_fit_mode_t fit_mode);
static void save_web_thumbnail_failure_marker(const char *source_path,
                                              ephoto_fit_mode_t fit_mode,
                                              esp_err_t err_code);
static void release_hardware_jpeg_buffers(void);
static void blit_rgb888_row_with_plan_to(const image_render_plan_t *plan,
                                         const uint8_t *source_row0,
                                         const uint8_t *source_row1,
                                         int channels,
                                         uint16_t rotation_deg,
                                         int render_y,
                                         color_pixel_rgb565_data_t *target);
static void render_rgb888_pair_rows_to(const image_render_plan_t *plan,
                                       const uint8_t *source_row0,
                                       const uint8_t *source_row1,
                                       int channels,
                                       uint16_t rotation_deg,
                                       uint32_t source_y0,
                                       uint32_t source_y1,
                                       int *render_y_cursor,
                                       color_pixel_rgb565_data_t *target);
static esp_err_t blit_decoded_image_to_buffer(const decoded_jpeg_t *image,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              uint16_t source_rotation_deg,
                                              color_pixel_rgb565_data_t *target);
static bool probe_image_dimensions(const char *path, uint32_t *out_width, uint32_t *out_height);
static esp_err_t scale_rgb565_rect(const color_pixel_rgb565_data_t *source,
                                   uint16_t source_width,
                                   uint16_t source_height,
                                   int crop_x,
                                   int crop_y,
                                   int crop_width,
                                   int crop_height,
                                   uint16_t target_width,
                                   uint16_t target_height,
                                   color_pixel_rgb565_data_t *target);
static esp_err_t save_web_thumbnail_buffer(const char *source_path,
                                           ephoto_fit_mode_t fit_mode,
                                           uint16_t width,
                                           uint16_t height,
                                           const color_pixel_rgb565_data_t *buffer);
static esp_err_t build_render_plan(uint32_t source_width,
                                   uint32_t source_height,
                                   ephoto_fit_mode_t fit_mode,
                                   uint16_t rotation_deg,
                                   image_render_plan_t *out_plan);
static void free_render_plan(image_render_plan_t *plan);
static esp_err_t ensure_matching_screen_cache_ready(const char *source_path, ephoto_fit_mode_t fit_mode);
static esp_err_t save_decoded_jpeg_cache(const char *path,
                                         const decoded_jpeg_t *image,
                                         ephoto_fit_mode_t fit_mode,
                                         uint16_t rotation_deg,
                                         uint16_t source_rotation_deg,
                                         color_pixel_rgb565_data_t *cache_buffer);
static esp_err_t decode_jpeg_file_sw_to_decoded_rgb565(const char *path,
                                                       const jpeg_software_header_info_t *header_info,
                                                       ephoto_fit_mode_t fit_mode,
                                                       uint16_t rotation_deg,
                                                       decoded_jpeg_t *out_image);
static esp_err_t render_jpeg_file_sw_scanline_to_owned_buffer(const char *path,
                                                              ephoto_fit_mode_t fit_mode,
                                                              uint16_t rotation_deg,
                                                              unsigned scale_denom,
                                                              color_pixel_rgb565_data_t **out_target);
static unsigned clamp_software_stream_scale_denom(uint32_t image_width,
                                                  uint32_t image_height,
                                                  unsigned scale_denom);
static esp_err_t render_png_file_sw_to_buffer(const char *path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              color_pixel_rgb565_data_t *target);
static esp_err_t render_bmp_file_sw_to_buffer(const char *path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              color_pixel_rgb565_data_t *target);
static uint16_t lookup_photo_source_rotation_deg(const char *source_path);
static esp_err_t decode_screen_jpeg_cache_to_buffer(const uint8_t *jpeg_data,
                                                    size_t jpeg_size,
                                                    uint16_t rotation_deg,
                                                    color_pixel_rgb565_data_t *target);
static esp_err_t decode_web_thumbnail_jpeg_to_allocated_rgb565(const uint8_t *jpeg_data,
                                                               size_t jpeg_size,
                                                               uint16_t max_width,
                                                               uint16_t max_height,
                                                               void **out_pixels,
                                                               uint16_t *out_width,
                                                               uint16_t *out_height);
static esp_err_t decode_jpeg_memory_software_to_allocated_rgb565(const uint8_t *jpeg_data,
                                                                 size_t jpeg_size,
                                                                 uint16_t max_width,
                                                                 uint16_t max_height,
                                                                 void **out_pixels,
                                                                 uint16_t *out_width,
                                                                 uint16_t *out_height);
static esp_err_t ensure_boot_image_mapped(void);
static esp_err_t load_cached_photo_into_buffer(const char *source_path,
                                               ephoto_fit_mode_t fit_mode,
                                               uint16_t rotation_deg,
                                               color_pixel_rgb565_data_t *target,
                                               uint32_t *out_payload_size,
                                               int64_t *out_read_time_ms,
                                               int64_t *out_decode_time_ms);
static bool base_photo_matches_locked(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg);
static color_pixel_rgb565_data_t *claim_reusable_buffer_locked(void);
static void retire_buffer_locked(color_pixel_rgb565_data_t *buffer);
static bool should_render_photo(const ephoto_app_state_t *state);

static void log_spiram_state(const char *label)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "spiram[%s]: free=%u largest=%u",
             label ? label : "-",
             (unsigned)free_bytes,
             (unsigned)largest_block);
}

static uint16_t rgb888_to_rgb565(rgb888_t color)
{
    return (uint16_t)(((color.r & 0xF8) << 8) |
                      ((color.g & 0xFC) << 3) |
                      ((color.b & 0xF8) >> 3));
}

static rgb888_t rgb565_to_rgb888(uint16_t color565)
{
    rgb888_t color = {
        .r = (uint8_t)(((color565 >> 11) & 0x1F) * 255 / 31),
        .g = (uint8_t)(((color565 >> 5) & 0x3F) * 255 / 63),
        .b = (uint8_t)((color565 & 0x1F) * 255 / 31),
    };
    return color;
}

static void get_logical_dimensions(uint16_t rotation_deg, int *width, int *height)
{
    if (rotation_deg == 90) {
        *width = s_profile->lcd_v_res;
        *height = s_profile->lcd_h_res;
    } else {
        *width = s_profile->lcd_h_res;
        *height = s_profile->lcd_v_res;
    }
}

static uint32_t align_up_u32(uint32_t value, uint32_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static uint16_t read_le16_mem(const uint8_t *ptr)
{
    return (uint16_t)(((uint16_t)ptr[1] << 8) | (uint16_t)ptr[0]);
}

static uint32_t read_le32_mem(const uint8_t *ptr)
{
    return ((uint32_t)ptr[3] << 24) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[1] << 8) |
           (uint32_t)ptr[0];
}

static int32_t read_le32s_mem(const uint8_t *ptr)
{
    return (int32_t)read_le32_mem(ptr);
}

static uint32_t read_be32_mem(const uint8_t *ptr)
{
    return ((uint32_t)ptr[0] << 24) |
           ((uint32_t)ptr[1] << 16) |
           ((uint32_t)ptr[2] << 8) |
           (uint32_t)ptr[3];
}

static uint32_t task_wdt_idle_core_mask(void)
{
    uint32_t mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= 1U << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    mask |= 1U << 1;
#endif
    return mask;
}

static void get_cache_frame_dimensions(uint16_t rotation_deg, uint16_t *width, uint16_t *height)
{
    int logical_width = 0;
    int logical_height = 0;
    get_logical_dimensions(rotation_deg, &logical_width, &logical_height);
    if (width) {
        *width = (uint16_t)logical_width;
    }
    if (height) {
        *height = (uint16_t)logical_height;
    }
}

static bool begin_long_decode_wdt_guard(void)
{
    esp_task_wdt_config_t config = {
        .timeout_ms = EPHOTO_LONG_DECODE_TIMEOUT_MS,
        .idle_core_mask = task_wdt_idle_core_mask(),
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "extend task wdt failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void end_long_decode_wdt_guard(bool applied)
{
    if (!applied) {
        return;
    }

    esp_task_wdt_config_t config = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = task_wdt_idle_core_mask(),
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "restore task wdt failed: %s", esp_err_to_name(err));
    }
}

static inline void cooperative_decode_pause(uint32_t step)
{
    if ((step & 0x1FU) == 0U) {
        vTaskDelay(1);
    }
}

static uint64_t fnv1a64_update(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t hash_source_key(const char *path, uint16_t rotation_deg, ephoto_fit_mode_t fit_mode)
{
    uint64_t hash = 1469598103934665603ULL;
    uint16_t source_rotation_deg = lookup_photo_source_rotation_deg(path);
    hash = fnv1a64_update(hash, path, strlen(path));
    hash = fnv1a64_update(hash, &rotation_deg, sizeof(rotation_deg));
    hash = fnv1a64_update(hash, &fit_mode, sizeof(fit_mode));
    hash = fnv1a64_update(hash, &source_rotation_deg, sizeof(source_rotation_deg));
    hash = fnv1a64_update(hash, &s_profile->lcd_h_res, sizeof(s_profile->lcd_h_res));
    hash = fnv1a64_update(hash, &s_profile->lcd_v_res, sizeof(s_profile->lcd_v_res));
    return hash;
}

static uint64_t hash_named_cache_key(const char *path,
                                     const char *cache_tag,
                                     uint16_t rotation_deg,
                                     ephoto_fit_mode_t fit_mode)
{
    uint64_t hash = hash_source_key(path, rotation_deg, fit_mode);
    return fnv1a64_update(hash, cache_tag, strlen(cache_tag));
}

static esp_err_t ensure_cache_directory(void)
{
    const char *mount_path = storage_service_get_mount_path();
    if (!mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    char cache_dir[EPHOTO_MAX_PHOTO_PATH_LEN];
    int written = snprintf(cache_dir, sizeof(cache_dir), "%s/.ephoto_cache", mount_path);
    if (written <= 0 || written >= (int)sizeof(cache_dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(cache_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(cache_dir, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t build_cache_path(const char *source_path,
                                  uint16_t rotation_deg,
                                  ephoto_fit_mode_t fit_mode,
                                  char *out_path,
                                  size_t out_len)
{
    return build_cache_path_with_ext(source_path, rotation_deg, fit_mode, "epf", out_path, out_len);
}

static esp_err_t build_failure_cache_path(const char *source_path,
                                          uint16_t rotation_deg,
                                          ephoto_fit_mode_t fit_mode,
                                          char *out_path,
                                          size_t out_len)
{
    return build_cache_path_with_ext(source_path, rotation_deg, fit_mode, "fail", out_path, out_len);
}

static esp_err_t build_web_thumbnail_cache_path(const char *source_path,
                                                ephoto_fit_mode_t fit_mode,
                                                char *out_path,
                                                size_t out_len)
{
    if (!source_path || !source_path[0] || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *mount_path = storage_service_get_mount_path();
    if (!mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *variant = fit_mode == EPHOTO_FIT_COVER ? "cover" : "contain";
    uint64_t key = hash_named_cache_key(source_path, "web-thumb", 0, fit_mode);
    int written = snprintf(out_path,
                           out_len,
                           "%s/.ephoto_cache/%016llx_web_%s.epf",
                           mount_path,
                           (unsigned long long)key,
                           variant);
    if (written <= 0 || written >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t build_web_thumbnail_failure_path(const char *source_path,
                                                  ephoto_fit_mode_t fit_mode,
                                                  char *out_path,
                                                  size_t out_len)
{
    if (!source_path || !source_path[0] || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *mount_path = storage_service_get_mount_path();
    if (!mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *variant = fit_mode == EPHOTO_FIT_COVER ? "cover" : "contain";
    uint64_t key = hash_named_cache_key(source_path, "web-thumb", 0, fit_mode);
    int written = snprintf(out_path,
                           out_len,
                           "%s/.ephoto_cache/%016llx_web_%s.fail",
                           mount_path,
                           (unsigned long long)key,
                           variant);
    if (written <= 0 || written >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t build_cache_path_with_ext(const char *source_path,
                                           uint16_t rotation_deg,
                                           ephoto_fit_mode_t fit_mode,
                                           const char *ext,
                                           char *out_path,
                                           size_t out_len)
{
    if (!source_path || !ext || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *mount_path = storage_service_get_mount_path();
    if (!mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t key = hash_source_key(source_path, rotation_deg, fit_mode);
    int written = snprintf(out_path,
                           out_len,
                           "%s/.ephoto_cache/%016llx_r%u.%s",
                           mount_path,
                           (unsigned long long)key,
                           rotation_deg,
                           ext);
    if (written <= 0 || written >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool is_large_photo_candidate(size_t size_bytes, uint32_t width, uint32_t height)
{
    uint64_t pixels = (uint64_t)width * height;
    return size_bytes >= EPHOTO_CACHE_DEFER_SIZE_BYTES || pixels >= EPHOTO_CACHE_DEFER_PIXELS;
}

static bool should_defer_uncached_photo_path(const char *path, size_t size_bytes, uint32_t width, uint32_t height)
{
    if (!path || !path[0]) {
        return false;
    }

    image_type_t sniffed_type = sniff_image_type_from_file(path);
    if (sniffed_type == IMAGE_TYPE_JPEG || sniffed_type == IMAGE_TYPE_UNKNOWN) {
        jpeg_software_header_info_t header_info = {0};
        if (probe_jpeg_software_header(path, &header_info) && choose_software_jpeg_reason(&header_info)) {
            return true;
        }

        if (sniffed_type == IMAGE_TYPE_JPEG) {
            return is_large_photo_candidate(size_bytes, width, height);
        }
    }

    return false;
}

static esp_err_t load_cache_header(const char *source_path,
                                   uint16_t rotation_deg,
                                   ephoto_fit_mode_t fit_mode,
                                   ephoto_cache_header_t *out_header,
                                   char *out_cache_path,
                                   size_t out_cache_path_len)
{
    if (!source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_cache_path(source_path, rotation_deg, fit_mode, cache_path, sizeof(cache_path)),
                        TAG,
                        "cache path build failed");

    struct stat cache_stat;
    if (stat(cache_path, &cache_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ephoto_cache_header_t header = {0};
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    close(fd);

    uint16_t expected_frame_width = 0;
    uint16_t expected_frame_height = 0;
    get_cache_frame_dimensions(rotation_deg, &expected_frame_width, &expected_frame_height);

    bool header_ok = bytes_read == (ssize_t)sizeof(header) &&
                     header.magic == EPHOTO_CACHE_MAGIC &&
                     header.version == EPHOTO_CACHE_VERSION &&
                     header.format == EPHOTO_CACHE_FORMAT_SCREEN_JPEG &&
                     header.rotation_deg == rotation_deg &&
                     header.frame_width == expected_frame_width &&
                     header.frame_height == expected_frame_height &&
                     header.source_size == (uint64_t)src_stat.st_size &&
                     header.source_mtime == (int64_t)src_stat.st_mtime &&
                     header.payload_size > 0 &&
                     (size_t)cache_stat.st_size == sizeof(ephoto_cache_header_t) + (size_t)header.payload_size &&
                     header.source_hash == hash_source_key(source_path, rotation_deg, fit_mode);
    if (!header_ok) {
        unlink(cache_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_header) {
        *out_header = header;
    }
    if (out_cache_path && out_cache_path_len > 0) {
        strlcpy(out_cache_path, cache_path, out_cache_path_len);
    }
    return ESP_OK;
}

static esp_err_t load_cache_failure_header(const char *source_path,
                                           uint16_t rotation_deg,
                                           ephoto_fit_mode_t fit_mode,
                                           ephoto_cache_failure_header_t *out_header,
                                           char *out_cache_path,
                                           size_t out_cache_path_len)
{
    if (!source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_failure_cache_path(source_path, rotation_deg, fit_mode, fail_path, sizeof(fail_path)),
                        TAG,
                        "failure path build failed");

    struct stat fail_stat;
    if (stat(fail_path, &fail_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int fd = open(fail_path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ephoto_cache_failure_header_t header = {0};
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    close(fd);

    bool header_ok = bytes_read == (ssize_t)sizeof(header) &&
                     header.magic == EPHOTO_CACHE_FAILURE_MAGIC &&
                     header.version == EPHOTO_CACHE_FAILURE_VERSION &&
                     header.fit_mode == (uint8_t)fit_mode &&
                     header.rotation_deg == rotation_deg &&
                     header.source_size == (uint64_t)src_stat.st_size &&
                     header.source_mtime == (int64_t)src_stat.st_mtime &&
                     header.source_hash == hash_source_key(source_path, rotation_deg, fit_mode);
    if (!header_ok) {
        unlink(fail_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_header) {
        *out_header = header;
    }
    if (out_cache_path && out_cache_path_len > 0) {
        strlcpy(out_cache_path, fail_path, out_cache_path_len);
    }
    return ESP_OK;
}

static esp_err_t load_web_thumbnail_header(const char *source_path,
                                           ephoto_fit_mode_t fit_mode,
                                           ephoto_cache_header_t *out_header,
                                           char *out_cache_path,
                                           size_t out_cache_path_len)
{
    if (!source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_web_thumbnail_cache_path(source_path, fit_mode, cache_path, sizeof(cache_path)),
                        TAG,
                        "web thumbnail path build failed");

    struct stat cache_stat;
    if (stat(cache_path, &cache_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ephoto_cache_header_t header = {0};
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    close(fd);

    bool header_ok = bytes_read == (ssize_t)sizeof(header) &&
                     header.magic == EPHOTO_CACHE_MAGIC &&
                     header.version == EPHOTO_CACHE_VERSION &&
                     header.format == EPHOTO_CACHE_FORMAT_WEB_JPEG &&
                     header.source_size == (uint64_t)src_stat.st_size &&
                     header.source_mtime == (int64_t)src_stat.st_mtime &&
                     header.payload_size > 0 &&
                     header.frame_width > 0 &&
                     header.frame_height > 0 &&
                     (size_t)cache_stat.st_size == sizeof(ephoto_cache_header_t) + (size_t)header.payload_size &&
                     header.source_hash == hash_named_cache_key(source_path, "web-thumb", 0, fit_mode);
    if (!header_ok) {
        unlink(cache_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_header) {
        *out_header = header;
    }
    if (out_cache_path && out_cache_path_len > 0) {
        strlcpy(out_cache_path, cache_path, out_cache_path_len);
    }
    return ESP_OK;
}

static esp_err_t load_web_thumbnail_failure_header(const char *source_path,
                                                   ephoto_fit_mode_t fit_mode,
                                                   ephoto_cache_failure_header_t *out_header,
                                                   char *out_cache_path,
                                                   size_t out_cache_path_len)
{
    if (!source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_web_thumbnail_failure_path(source_path, fit_mode, fail_path, sizeof(fail_path)),
                        TAG,
                        "web thumbnail failure path build failed");

    struct stat fail_stat;
    if (stat(fail_path, &fail_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int fd = open(fail_path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ephoto_cache_failure_header_t header = {0};
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    close(fd);

    bool header_ok = bytes_read == (ssize_t)sizeof(header) &&
                     header.magic == EPHOTO_CACHE_FAILURE_MAGIC &&
                     header.version == EPHOTO_CACHE_FAILURE_VERSION &&
                     header.fit_mode == (uint8_t)fit_mode &&
                     header.rotation_deg == 0 &&
                     header.source_size == (uint64_t)src_stat.st_size &&
                     header.source_mtime == (int64_t)src_stat.st_mtime &&
                     header.source_hash == hash_named_cache_key(source_path, "web-thumb", 0, fit_mode);
    if (!header_ok) {
        unlink(fail_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_header) {
        *out_header = header;
    }
    if (out_cache_path && out_cache_path_len > 0) {
        strlcpy(out_cache_path, fail_path, out_cache_path_len);
    }
    return ESP_OK;
}

static void clear_cache_failure_marker(const char *source_path,
                                       uint16_t rotation_deg,
                                       ephoto_fit_mode_t fit_mode)
{
    if (!source_path || !source_path[0]) {
        return;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (build_failure_cache_path(source_path, rotation_deg, fit_mode, fail_path, sizeof(fail_path)) == ESP_OK) {
        unlink(fail_path);
    }
}

static void save_cache_failure_marker(const char *source_path,
                                      uint16_t rotation_deg,
                                      ephoto_fit_mode_t fit_mode,
                                      esp_err_t err_code)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !source_path || !source_path[0]) {
        return;
    }
    if (ensure_cache_directory() != ESP_OK) {
        return;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (build_failure_cache_path(source_path, rotation_deg, fit_mode, fail_path, sizeof(fail_path)) != ESP_OK) {
        return;
    }

    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", fail_path);
    if (temp_written <= 0 || temp_written >= (int)sizeof(temp_path)) {
        return;
    }

    unlink(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        return;
    }

    ephoto_cache_failure_header_t header = {
        .magic = EPHOTO_CACHE_FAILURE_MAGIC,
        .version = EPHOTO_CACHE_FAILURE_VERSION,
        .fit_mode = (uint8_t)fit_mode,
        .rotation_deg = rotation_deg,
        .source_size = (uint64_t)src_stat.st_size,
        .source_mtime = (int64_t)src_stat.st_mtime,
        .source_hash = hash_source_key(source_path, rotation_deg, fit_mode),
        .error_code = err_code,
    };

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        fflush(file);
        fsync(fileno(file));
    }
    fclose(file);
    if (!ok) {
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, fail_path) != 0) {
        unlink(temp_path);
    }
}

static void clear_web_thumbnail_failure_marker(const char *source_path, ephoto_fit_mode_t fit_mode)
{
    if (!source_path || !source_path[0]) {
        return;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (build_web_thumbnail_failure_path(source_path, fit_mode, fail_path, sizeof(fail_path)) == ESP_OK) {
        unlink(fail_path);
    }
}

static void save_web_thumbnail_failure_marker(const char *source_path,
                                              ephoto_fit_mode_t fit_mode,
                                              esp_err_t err_code)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !source_path || !source_path[0]) {
        return;
    }
    if (ensure_cache_directory() != ESP_OK) {
        return;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return;
    }

    char fail_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (build_web_thumbnail_failure_path(source_path, fit_mode, fail_path, sizeof(fail_path)) != ESP_OK) {
        return;
    }

    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", fail_path);
    if (temp_written <= 0 || temp_written >= (int)sizeof(temp_path)) {
        return;
    }

    unlink(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        return;
    }

    ephoto_cache_failure_header_t header = {
        .magic = EPHOTO_CACHE_FAILURE_MAGIC,
        .version = EPHOTO_CACHE_FAILURE_VERSION,
        .fit_mode = (uint8_t)fit_mode,
        .rotation_deg = 0,
        .source_size = (uint64_t)src_stat.st_size,
        .source_mtime = (int64_t)src_stat.st_mtime,
        .source_hash = hash_named_cache_key(source_path, "web-thumb", 0, fit_mode),
        .error_code = err_code,
    };

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        fflush(file);
        fsync(fileno(file));
    }
    fclose(file);
    if (!ok) {
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, fail_path) != 0) {
        unlink(temp_path);
    }
}

static esp_err_t save_cached_buffer(const char *source_path,
                                    uint16_t rotation_deg,
                                    ephoto_fit_mode_t fit_mode,
                                    const color_pixel_rgb565_data_t *buffer)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!source_path || !source_path[0] || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_cache_directory(), TAG, "cache dir unavailable");

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_jpeg_encoder) {
        jpeg_encode_engine_cfg_t encode_eng_cfg = {
            .intr_priority = 0,
            .timeout_ms = 1000,
        };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&encode_eng_cfg, &s_jpeg_encoder),
                            TAG,
                            "create screen cache JPEG encoder failed");
    }

    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_capacity = 0;
    uint8_t *jpeg_buffer = jpeg_alloc_encoder_mem(EPHOTO_SCREEN_CACHE_JPEG_MAX_BYTES,
                                                  &out_mem_cfg,
                                                  &jpeg_capacity);
    ESP_RETURN_ON_FALSE(jpeg_buffer, ESP_ERR_NO_MEM, TAG, "allocate screen cache JPEG output failed");

    uint16_t frame_width = 0;
    uint16_t frame_height = 0;
    get_cache_frame_dimensions(rotation_deg, &frame_width, &frame_height);

    jpeg_encode_cfg_t encode_cfg = {
        .height = frame_height,
        .width = frame_width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = EPHOTO_SCREEN_CACHE_JPEG_QUALITY,
    };
    uint32_t jpeg_size = 0;
    esp_err_t encode_err = jpeg_encoder_process(s_jpeg_encoder,
                                                &encode_cfg,
                                                (const uint8_t *)buffer,
                                                (uint32_t)s_framebuffer_bytes,
                                                jpeg_buffer,
                                                (uint32_t)jpeg_capacity,
                                                &jpeg_size);
    if (encode_err != ESP_OK || jpeg_size == 0 || jpeg_size > jpeg_capacity) {
        free(jpeg_buffer);
        ESP_LOGW(TAG,
                 "screen cache JPEG encode failed for %s: %s size=%" PRIu32 " cap=%u",
                 source_path,
                 esp_err_to_name(encode_err),
                 jpeg_size,
                 (unsigned)jpeg_capacity);
        return encode_err == ESP_OK ? ESP_FAIL : encode_err;
    }

    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    esp_err_t path_err = build_cache_path(source_path, rotation_deg, fit_mode, cache_path, sizeof(cache_path));
    if (path_err != ESP_OK) {
        free(jpeg_buffer);
        return path_err;
    }

    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path);
    if (temp_written <= 0 || temp_written >= (int)sizeof(temp_path)) {
        free(jpeg_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        free(jpeg_buffer);
        return ESP_FAIL;
    }
    if (s_file_io_buffer) {
        setvbuf(file, (char *)s_file_io_buffer, _IOFBF, 64U * 1024U);
    }

    ephoto_cache_header_t header = {
        .magic = EPHOTO_CACHE_MAGIC,
        .version = EPHOTO_CACHE_VERSION,
        .format = EPHOTO_CACHE_FORMAT_SCREEN_JPEG,
        .rotation_deg = rotation_deg,
        .frame_width = frame_width,
        .frame_height = frame_height,
        .source_size = (uint64_t)src_stat.st_size,
        .source_mtime = (int64_t)src_stat.st_mtime,
        .source_hash = hash_source_key(source_path, rotation_deg, fit_mode),
        .payload_size = jpeg_size,
        .payload_crc32 = 0,
    };

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(jpeg_buffer, 1, jpeg_size, file) == jpeg_size;
    if (ok) {
        fflush(file);
        fsync(fileno(file));
    }
    fclose(file);
    free(jpeg_buffer);
    if (!ok) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    if (rename(temp_path, cache_path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    clear_cache_failure_marker(source_path, rotation_deg, fit_mode);

    ESP_LOGI(TAG,
             "saved JPEG display cache for %s: %" PRIu32 " bytes (raw=%u, %.1f%%)",
             source_path,
             jpeg_size,
             (unsigned)s_framebuffer_bytes,
             (double)jpeg_size * 100.0 / (double)s_framebuffer_bytes);
    return ESP_OK;
}

static bool probe_image_dimensions(const char *path, uint32_t *out_width, uint32_t *out_height)
{
    if (!path || !out_width || !out_height) {
        return false;
    }

    *out_width = 0;
    *out_height = 0;

    int index = gallery_service_find_index_by_path(path);
    if (index >= 0) {
        ephoto_photo_t photo = {0};
        if (gallery_service_get_item(index, &photo) == ESP_OK && photo.width > 0 && photo.height > 0) {
            *out_width = photo.width;
            *out_height = photo.height;
            return true;
        }
    }

    image_type_t image_type = sniff_image_type_from_file(path);
    if (image_type == IMAGE_TYPE_JPEG) {
        jpeg_software_header_info_t info = {0};
        if (probe_jpeg_software_header(path, &info) && info.width > 0 && info.height > 0) {
            *out_width = info.width;
            *out_height = info.height;
            return true;
        }
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    if (image_type == IMAGE_TYPE_PNG) {
        uint8_t header[24] = {0};
        bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                  memcmp(header, "\x89PNG\r\n\x1A\n", 8) == 0 &&
                  memcmp(header + 12, "IHDR", 4) == 0;
        fclose(file);
        if (!ok) {
            return false;
        }
        *out_width = read_be32_mem(&header[16]);
        *out_height = read_be32_mem(&header[20]);
        return *out_width > 0 && *out_height > 0;
    }

    if (image_type == IMAGE_TYPE_BMP) {
        uint8_t file_header[14] = {0};
        uint8_t dib_header[40] = {0};
        bool ok = fread(file_header, 1, sizeof(file_header), file) == sizeof(file_header) &&
                  fread(dib_header, 1, sizeof(dib_header), file) == sizeof(dib_header);
        fclose(file);
        if (!ok || file_header[0] != 'B' || file_header[1] != 'M') {
            return false;
        }
        int32_t width_signed = read_le32s_mem(&dib_header[4]);
        int32_t height_signed = read_le32s_mem(&dib_header[8]);
        if (width_signed <= 0 || height_signed == 0) {
            return false;
        }
        *out_width = (uint32_t)width_signed;
        *out_height = (uint32_t)(height_signed < 0 ? -height_signed : height_signed);
        return *out_width > 0 && *out_height > 0;
    }

    fclose(file);
    return false;
}

static esp_err_t scale_rgb565_rect(const color_pixel_rgb565_data_t *source,
                                   uint16_t source_width,
                                   uint16_t source_height,
                                   int crop_x,
                                   int crop_y,
                                   int crop_width,
                                   int crop_height,
                                   uint16_t target_width,
                                   uint16_t target_height,
                                   color_pixel_rgb565_data_t *target)
{
    if (!source || !target || source_width == 0 || source_height == 0 ||
        crop_width <= 0 || crop_height <= 0 || target_width == 0 || target_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (crop_x < 0 || crop_y < 0 ||
        crop_x + crop_width > source_width ||
        crop_y + crop_height > source_height) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t y = 0; y < target_height; ++y) {
        uint32_t src_y = crop_y + ((uint64_t)y * crop_height) / target_height;
        if (src_y >= (uint32_t)(crop_y + crop_height)) {
            src_y = (uint32_t)(crop_y + crop_height - 1);
        }
        for (uint16_t x = 0; x < target_width; ++x) {
            uint32_t src_x = crop_x + ((uint64_t)x * crop_width) / target_width;
            if (src_x >= (uint32_t)(crop_x + crop_width)) {
                src_x = (uint32_t)(crop_x + crop_width - 1);
            }
            target[(size_t)y * target_width + x].val = source[(size_t)src_y * source_width + src_x].val;
        }
    }
    return ESP_OK;
}

static esp_err_t save_web_thumbnail_buffer(const char *source_path,
                                           ephoto_fit_mode_t fit_mode,
                                           uint16_t width,
                                           uint16_t height,
                                           const color_pixel_rgb565_data_t *buffer)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!source_path || !source_path[0] || !buffer || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_cache_directory(), TAG, "cache dir unavailable");

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_jpeg_encoder) {
        jpeg_encode_engine_cfg_t encode_eng_cfg = {
            .intr_priority = 0,
            .timeout_ms = 1000,
        };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&encode_eng_cfg, &s_jpeg_encoder),
                            TAG,
                            "create web thumbnail JPEG encoder failed");
    }

    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t jpeg_capacity = 0;
    uint8_t *jpeg_buffer = jpeg_alloc_encoder_mem(EPHOTO_WEB_THUMB_MAX_BYTES,
                                                  &out_mem_cfg,
                                                  &jpeg_capacity);
    ESP_RETURN_ON_FALSE(jpeg_buffer, ESP_ERR_NO_MEM, TAG, "allocate web thumbnail JPEG output failed");

    size_t raw_size = (size_t)width * height * sizeof(color_pixel_rgb565_data_t);
    jpeg_encode_cfg_t encode_cfg = {
        .height = height,
        .width = width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = EPHOTO_WEB_THUMB_JPEG_QUALITY,
    };
    uint32_t jpeg_size = 0;
    esp_err_t encode_err = jpeg_encoder_process(s_jpeg_encoder,
                                                &encode_cfg,
                                                (const uint8_t *)buffer,
                                                (uint32_t)raw_size,
                                                jpeg_buffer,
                                                (uint32_t)jpeg_capacity,
                                                &jpeg_size);
    if (encode_err != ESP_OK || jpeg_size == 0 || jpeg_size > jpeg_capacity) {
        free(jpeg_buffer);
        return encode_err == ESP_OK ? ESP_FAIL : encode_err;
    }

    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    esp_err_t path_err = build_web_thumbnail_cache_path(source_path, fit_mode, cache_path, sizeof(cache_path));
    if (path_err != ESP_OK) {
        free(jpeg_buffer);
        return path_err;
    }

    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    int temp_written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path);
    if (temp_written <= 0 || temp_written >= (int)sizeof(temp_path)) {
        free(jpeg_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    unlink(temp_path);
    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        free(jpeg_buffer);
        return ESP_FAIL;
    }
    if (s_file_io_buffer) {
        setvbuf(file, (char *)s_file_io_buffer, _IOFBF, 32U * 1024U);
    }

    ephoto_cache_header_t header = {
        .magic = EPHOTO_CACHE_MAGIC,
        .version = EPHOTO_CACHE_VERSION,
        .format = EPHOTO_CACHE_FORMAT_WEB_JPEG,
        .rotation_deg = 0,
        .frame_width = width,
        .frame_height = height,
        .source_size = (uint64_t)src_stat.st_size,
        .source_mtime = (int64_t)src_stat.st_mtime,
        .source_hash = hash_named_cache_key(source_path, "web-thumb", 0, fit_mode),
        .payload_size = jpeg_size,
        .payload_crc32 = 0,
    };

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(jpeg_buffer, 1, jpeg_size, file) == jpeg_size;
    if (ok) {
        fflush(file);
        fsync(fileno(file));
    }
    fclose(file);
    free(jpeg_buffer);
    if (!ok) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    if (rename(temp_path, cache_path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }

    clear_web_thumbnail_failure_marker(source_path, fit_mode);
    return ESP_OK;
}

static esp_err_t build_web_thumbnail_from_screen_buffer(const char *source_path,
                                                        ephoto_fit_mode_t fit_mode,
                                                        const color_pixel_rgb565_data_t *screen_buffer)
{
    if (!source_path || !source_path[0] || !screen_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    get_cache_frame_dimensions(0, &screen_width, &screen_height);

    int crop_x = 0;
    int crop_y = 0;
    int crop_width = screen_width;
    int crop_height = screen_height;
    if (fit_mode == EPHOTO_FIT_CONTAIN) {
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        if (probe_image_dimensions(source_path, &source_width, &source_height)) {
            image_render_plan_t plan;
            memset(&plan, 0, sizeof(plan));
            if (build_render_plan(source_width, source_height, fit_mode, 0, &plan) == ESP_OK) {
                crop_x = plan.offset_x < 0 ? 0 : plan.offset_x;
                crop_y = plan.offset_y < 0 ? 0 : plan.offset_y;
                crop_width = plan.render_width > 0 ? plan.render_width : screen_width;
                crop_height = plan.render_height > 0 ? plan.render_height : screen_height;
                free_render_plan(&plan);
            }
        }
    } else {
        int square = screen_width < screen_height ? screen_width : screen_height;
        crop_width = square;
        crop_height = square;
        crop_x = (screen_width - square) / 2;
        crop_y = (screen_height - square) / 2;
    }

    uint16_t target_width = EPHOTO_WEB_THUMB_COVER_SIZE;
    uint16_t target_height = EPHOTO_WEB_THUMB_COVER_SIZE;
    if (fit_mode == EPHOTO_FIT_CONTAIN) {
        if (crop_width >= crop_height) {
            target_width = crop_width > EPHOTO_WEB_THUMB_PREVIEW_MAX_EDGE ? EPHOTO_WEB_THUMB_PREVIEW_MAX_EDGE
                                                                          : (uint16_t)crop_width;
            target_height = (uint16_t)(((uint32_t)crop_height * target_width + (crop_width / 2)) / crop_width);
        } else {
            target_height = crop_height > EPHOTO_WEB_THUMB_PREVIEW_MAX_EDGE ? EPHOTO_WEB_THUMB_PREVIEW_MAX_EDGE
                                                                            : (uint16_t)crop_height;
            target_width = (uint16_t)(((uint32_t)crop_width * target_height + (crop_height / 2)) / crop_height);
        }
        if (target_width == 0) {
            target_width = 1;
        }
        if (target_height == 0) {
            target_height = 1;
        }
    }

    size_t target_pixels = (size_t)target_width * target_height;
    color_pixel_rgb565_data_t *thumb_buffer =
        heap_caps_malloc(target_pixels * sizeof(color_pixel_rgb565_data_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!thumb_buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = scale_rgb565_rect(screen_buffer,
                                      screen_width,
                                      screen_height,
                                      crop_x,
                                      crop_y,
                                      crop_width,
                                      crop_height,
                                      target_width,
                                      target_height,
                                      thumb_buffer);
    if (err == ESP_OK) {
        err = save_web_thumbnail_buffer(source_path, fit_mode, target_width, target_height, thumb_buffer);
    }

    free(thumb_buffer);
    return err;
}

static esp_err_t ensure_matching_screen_cache_ready(const char *source_path, ephoto_fit_mode_t fit_mode)
{
    if (display_service_has_cache(source_path, fit_mode, 0)) {
        return ESP_OK;
    }
    if (display_service_has_known_cache_failure(source_path, fit_mode, 0)) {
        clear_cache_failure_marker(source_path, 0, fit_mode);
    }
    return display_service_prepare_photo_cache(source_path, fit_mode, 0);
}

static void reset_photo_cache(void)
{
    s_base_photo_rotation_deg = UINT16_MAX;
    s_base_photo_fit_mode = (ephoto_fit_mode_t)-1;
    s_base_photo_path[0] = '\0';
}

static void release_shadow_framebuffers(void)
{
    if (s_base_buffer) {
        free(s_base_buffer);
        s_base_buffer = NULL;
    }
}

static void release_stage_framebuffer(void)
{
    if (s_stage_buffer) {
        free(s_stage_buffer);
        s_stage_buffer = NULL;
    }
}

static void release_retired_framebuffers(void)
{
    for (size_t i = 0; i < 2; ++i) {
        if (s_retired_buffers[i]) {
            free(s_retired_buffers[i]);
            s_retired_buffers[i] = NULL;
            s_retired_generations[i] = 0;
        }
    }
}

static esp_err_t ensure_shadow_framebuffers(void)
{
    if (s_framebuffer_bytes == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_base_buffer) {
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        s_base_buffer = claim_reusable_buffer_locked();
        xSemaphoreGive(s_display_mutex);
        if (!s_base_buffer) {
            s_base_buffer = heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        ESP_RETURN_ON_FALSE(s_base_buffer, ESP_ERR_NO_MEM, TAG, "reallocate base framebuffer failed");
        memset(s_base_buffer, 0, s_framebuffer_bytes);
    }

    return ESP_OK;
}

static esp_err_t ensure_stage_framebuffer(void)
{
    if (s_stage_buffer) {
        return ESP_OK;
    }

    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    s_stage_buffer = claim_reusable_buffer_locked();
    xSemaphoreGive(s_display_mutex);
    if (!s_stage_buffer) {
        s_stage_buffer = heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_stage_buffer, ESP_ERR_NO_MEM, TAG, "reallocate stage framebuffer failed");
    memset(s_stage_buffer, 0, s_framebuffer_bytes);
    return ESP_OK;
}

static void release_runtime_framebuffers_for_cache_build(void)
{
    release_shadow_framebuffers();
    release_stage_framebuffer();
    release_retired_framebuffers();
    release_hardware_jpeg_buffers();
    reset_photo_cache();
}

static bool base_photo_matches_locked(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    return source_path && source_path[0] &&
           strcmp(s_base_photo_path, source_path) == 0 &&
           s_base_photo_fit_mode == fit_mode &&
           s_base_photo_rotation_deg == rotation_deg;
}

static color_pixel_rgb565_data_t *claim_reusable_buffer_locked(void)
{
    for (size_t i = 0; i < 2; ++i) {
        if (s_retired_buffers[i] && s_retired_generations[i] <= s_last_rendered_generation) {
            color_pixel_rgb565_data_t *buffer = s_retired_buffers[i];
            s_retired_buffers[i] = NULL;
            s_retired_generations[i] = 0;
            return buffer;
        }
    }
    return NULL;
}

static void retire_buffer_locked(color_pixel_rgb565_data_t *buffer)
{
    if (!buffer) {
        return;
    }

    for (size_t i = 0; i < 2; ++i) {
        if (!s_retired_buffers[i]) {
            s_retired_buffers[i] = buffer;
            s_retired_generations[i] = s_buffer_generation;
            return;
        }
    }

    for (size_t i = 0; i < 2; ++i) {
        if (s_retired_generations[i] <= s_last_rendered_generation) {
            free(s_retired_buffers[i]);
            s_retired_buffers[i] = buffer;
            s_retired_generations[i] = s_buffer_generation;
            return;
        }
    }

    free(buffer);
}

static void paint_buffer(color_pixel_rgb565_data_t *target, uint16_t color565)
{
    if (!target) {
        return;
    }

    for (size_t i = 0; i < s_framebuffer_pixels; ++i) {
        target[i].val = color565;
    }
}

static void put_pixel_logical_to(color_pixel_rgb565_data_t *target,
                                 int logical_x,
                                 int logical_y,
                                 uint16_t color565,
                                 uint16_t rotation_deg)
{
    if (!target) {
        return;
    }

    int logical_width = 0;
    int logical_height = 0;
    get_logical_dimensions(rotation_deg, &logical_width, &logical_height);
    if (logical_x < 0 || logical_y < 0 || logical_x >= logical_width || logical_y >= logical_height) {
        return;
    }

    int physical_x = 0;
    int physical_y = 0;
    map_logical_to_physical(logical_x, logical_y, rotation_deg, &physical_x, &physical_y);
    target[(size_t)physical_y * (size_t)logical_width + (size_t)physical_x].val = color565;
}

static bool get_clock_compact(char *buffer, size_t buffer_len)
{
    struct tm tm_now = {0};
    if (!buffer || buffer_len == 0) {
        return false;
    }
    buffer[0] = '\0';
    if (!clock_service_get_local_tm(&tm_now)) {
        return false;
    }
    strftime(buffer, buffer_len, "%H:%M", &tm_now);
    return buffer[0] != '\0';
}

static image_type_t detect_image_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return IMAGE_TYPE_UNKNOWN;
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return IMAGE_TYPE_JPEG;
    }
    if (strcasecmp(ext, ".png") == 0) {
        return IMAGE_TYPE_PNG;
    }
    if (strcasecmp(ext, ".bmp") == 0) {
        return IMAGE_TYPE_BMP;
    }
    return IMAGE_TYPE_UNKNOWN;
}

static image_type_t sniff_image_type_from_file(const char *path)
{
    uint8_t header[8] = {0};
    FILE *file = fopen(path, "rb");
    if (!file) {
        return IMAGE_TYPE_UNKNOWN;
    }

    size_t bytes_read = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (bytes_read >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        return IMAGE_TYPE_JPEG;
    }
    if (bytes_read == sizeof(header) &&
        memcmp(header, "\x89PNG\r\n\x1A\n", sizeof(header)) == 0) {
        return IMAGE_TYPE_PNG;
    }
    if (bytes_read >= 2 && header[0] == 'B' && header[1] == 'M') {
        return IMAGE_TYPE_BMP;
    }
    return IMAGE_TYPE_UNKNOWN;
}

static void software_jpeg_error_exit(j_common_ptr cinfo)
{
    software_jpeg_error_mgr_t *err = (software_jpeg_error_mgr_t *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->setjmp_buffer, 1);
}

static bool probe_jpeg_software_header(const char *path, jpeg_software_header_info_t *out_info)
{
    if (!path || !out_info) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));

    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    struct jpeg_decompress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    software_jpeg_error_mgr_t jerr;
    memset(&jerr, 0, sizeof(jerr));
    volatile bool jpeg_created = false;
    bool ok = false;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = software_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        ESP_LOGW(TAG, "probe jpeg header failed for %s: %s", path, jerr.message[0] ? jerr.message : "unknown");
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_created = true;
    jpeg_stdio_src(&cinfo, file);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        goto cleanup;
    }

    out_info->valid = true;
    out_info->progressive_mode = cinfo.progressive_mode;
    out_info->arith_code = cinfo.arith_code;
    out_info->num_components = cinfo.num_components;
    out_info->jpeg_color_space = cinfo.jpeg_color_space;
    out_info->width = cinfo.image_width;
    out_info->height = cinfo.image_height;
    ok = true;

cleanup:
    if (jpeg_created) {
        jpeg_destroy_decompress(&cinfo);
    }
    fclose(file);
    return ok;
}

static const char *choose_software_jpeg_reason(const jpeg_software_header_info_t *info)
{
    if (!info || !info->valid) {
        return NULL;
    }
    if (info->arith_code) {
        return "arithmetic-coded jpeg";
    }
    if (info->progressive_mode) {
        return "progressive jpeg";
    }
    if (info->num_components != 1 && info->num_components != 3) {
        return "unsupported jpeg component count";
    }

    switch (info->jpeg_color_space) {
    case JCS_GRAYSCALE:
    case JCS_RGB:
    case JCS_YCbCr:
        return NULL;
    default:
        return "unsupported jpeg color space";
    }
}

static uint16_t normalize_source_rotation_deg(uint16_t rotation_deg)
{
    switch (rotation_deg % 360U) {
    case 90:
    case 180:
    case 270:
        return (uint16_t)(rotation_deg % 360U);
    default:
        return 0;
    }
}

static void get_oriented_source_dimensions(uint32_t source_width,
                                           uint32_t source_height,
                                           uint16_t source_rotation_deg,
                                           uint32_t *out_width,
                                           uint32_t *out_height)
{
    if (!out_width || !out_height) {
        return;
    }
    uint16_t normalized = normalize_source_rotation_deg(source_rotation_deg);
    if (normalized == 90 || normalized == 270) {
        *out_width = source_height;
        *out_height = source_width;
    } else {
        *out_width = source_width;
        *out_height = source_height;
    }
}

static void map_oriented_source_to_raw(uint32_t raw_width,
                                       uint32_t raw_height,
                                       uint16_t source_rotation_deg,
                                       uint32_t oriented_x,
                                       uint32_t oriented_y,
                                       uint32_t *out_raw_x,
                                       uint32_t *out_raw_y)
{
    uint16_t normalized = normalize_source_rotation_deg(source_rotation_deg);
    uint32_t raw_x = oriented_x;
    uint32_t raw_y = oriented_y;

    switch (normalized) {
    case 90:
        raw_x = oriented_y;
        raw_y = raw_height > 0 ? (raw_height - 1U - oriented_x) : 0;
        break;
    case 180:
        raw_x = raw_width > 0 ? (raw_width - 1U - oriented_x) : 0;
        raw_y = raw_height > 0 ? (raw_height - 1U - oriented_y) : 0;
        break;
    case 270:
        raw_x = raw_width > 0 ? (raw_width - 1U - oriented_y) : 0;
        raw_y = oriented_x;
        break;
    default:
        break;
    }

    if (out_raw_x) {
        *out_raw_x = raw_x;
    }
    if (out_raw_y) {
        *out_raw_y = raw_y;
    }
}

static uint16_t lookup_photo_source_rotation_deg(const char *source_path)
{
    if (!source_path || !source_path[0]) {
        return 0;
    }

    int index = gallery_service_find_index_by_path(source_path);
    if (index < 0) {
        return 0;
    }

    ephoto_photo_t photo = {0};
    if (gallery_service_get_item(index, &photo) != ESP_OK) {
        return 0;
    }

    return normalize_source_rotation_deg(photo.effective_rotation_deg);
}

static esp_err_t build_render_plan(uint32_t source_width,
                                   uint32_t source_height,
                                   ephoto_fit_mode_t fit_mode,
                                   uint16_t rotation_deg,
                                   image_render_plan_t *out_plan)
{
    if (!out_plan || source_width == 0 || source_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_plan, 0, sizeof(*out_plan));
    get_logical_dimensions(rotation_deg, &out_plan->logical_width, &out_plan->logical_height);
    out_plan->source_width = source_width;
    out_plan->source_height = source_height;
    out_plan->render_width = out_plan->logical_width;
    out_plan->render_height = out_plan->logical_height;

    bool source_wider = (uint64_t)source_width * out_plan->logical_height >
                        (uint64_t)out_plan->logical_width * source_height;
    if (fit_mode == EPHOTO_FIT_COVER) {
        if (source_wider) {
            out_plan->render_width = (int)(((uint64_t)source_width * out_plan->logical_height) / source_height);
        } else {
            out_plan->render_height = (int)(((uint64_t)source_height * out_plan->logical_width) / source_width);
        }
    } else {
        if (source_wider) {
            out_plan->render_height = (int)(((uint64_t)source_height * out_plan->logical_width) / source_width);
        } else {
            out_plan->render_width = (int)(((uint64_t)source_width * out_plan->logical_height) / source_height);
        }
    }

    if (out_plan->render_width <= 0) {
        out_plan->render_width = 1;
    }
    if (out_plan->render_height <= 0) {
        out_plan->render_height = 1;
    }

    out_plan->offset_x = (out_plan->logical_width - out_plan->render_width) / 2;
    out_plan->offset_y = (out_plan->logical_height - out_plan->render_height) / 2;
    size_t map_len = (size_t)out_plan->render_width;
    out_plan->source_x0_map = heap_caps_malloc(sizeof(uint32_t) * map_len,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_plan->source_x1_map = heap_caps_malloc(sizeof(uint32_t) * map_len,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_plan->source_x_weight_map = heap_caps_malloc(sizeof(uint16_t) * map_len,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t map_len_y = (size_t)out_plan->render_height;
    out_plan->source_y0_map = heap_caps_malloc(sizeof(uint32_t) * map_len_y,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_plan->source_y1_map = heap_caps_malloc(sizeof(uint32_t) * map_len_y,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_plan->source_y_weight_map = heap_caps_malloc(sizeof(uint16_t) * map_len_y,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_plan->source_x0_map || !out_plan->source_x1_map || !out_plan->source_x_weight_map ||
        !out_plan->source_y0_map || !out_plan->source_y1_map || !out_plan->source_y_weight_map) {
        return ESP_ERR_NO_MEM;
    }

    for (int x = 0; x < out_plan->render_width; ++x) {
        uint32_t src_fp = (uint32_t)(((uint64_t)x * source_width * 256ULL) / out_plan->render_width);
        uint32_t src_x0 = src_fp >> 8;
        uint32_t src_x1 = src_x0 + 1U < source_width ? src_x0 + 1U : src_x0;
        out_plan->source_x0_map[x] = src_x0;
        out_plan->source_x1_map[x] = src_x1;
        out_plan->source_x_weight_map[x] = (uint16_t)(src_fp & 0xFFU);
    }

    for (int y = 0; y < out_plan->render_height; ++y) {
        uint32_t src_fp = (uint32_t)(((uint64_t)y * source_height * 256ULL) / out_plan->render_height);
        uint32_t src_y0 = src_fp >> 8;
        uint32_t src_y1 = src_y0 + 1U < source_height ? src_y0 + 1U : src_y0;
        out_plan->source_y0_map[y] = src_y0;
        out_plan->source_y1_map[y] = src_y1;
        out_plan->source_y_weight_map[y] = (uint16_t)(src_fp & 0xFFU);
    }

    return ESP_OK;
}

static void free_render_plan(image_render_plan_t *plan)
{
    if (!plan) {
        return;
    }
    free(plan->source_x0_map);
    free(plan->source_x1_map);
    free(plan->source_x_weight_map);
    free(plan->source_y0_map);
    free(plan->source_y1_map);
    free(plan->source_y_weight_map);
    memset(plan, 0, sizeof(*plan));
}

static void blit_rgb888_row_with_plan_to(const image_render_plan_t *plan,
                                         const uint8_t *source_row0,
                                         const uint8_t *source_row1,
                                         int channels,
                                         uint16_t rotation_deg,
                                         int render_y,
                                         color_pixel_rgb565_data_t *target)
{
    if (!plan || !source_row0 || !source_row1 || !plan->source_x0_map || !plan->source_x1_map ||
        !plan->source_x_weight_map || channels < 3 || render_y < 0 || render_y >= plan->render_height || !target) {
        return;
    }

    int logical_y = plan->offset_y + render_y;
    uint16_t wy = plan->source_y_weight_map ? plan->source_y_weight_map[render_y] : 0;
    uint16_t iwy = 256U - wy;
    for (int render_x = 0; render_x < plan->render_width; ++render_x) {
        const uint8_t *pixel00 = source_row0 + (size_t)plan->source_x0_map[render_x] * channels;
        const uint8_t *pixel01 = source_row0 + (size_t)plan->source_x1_map[render_x] * channels;
        const uint8_t *pixel10 = source_row1 + (size_t)plan->source_x0_map[render_x] * channels;
        const uint8_t *pixel11 = source_row1 + (size_t)plan->source_x1_map[render_x] * channels;
        uint16_t wx = plan->source_x_weight_map[render_x];
        uint16_t iw = 256U - wx;
        rgb888_t top = {
            .r = (uint8_t)((pixel00[0] * iw + pixel01[0] * wx + 128U) >> 8),
            .g = (uint8_t)((pixel00[1] * iw + pixel01[1] * wx + 128U) >> 8),
            .b = (uint8_t)((pixel00[2] * iw + pixel01[2] * wx + 128U) >> 8),
        };
        rgb888_t bottom = {
            .r = (uint8_t)((pixel10[0] * iw + pixel11[0] * wx + 128U) >> 8),
            .g = (uint8_t)((pixel10[1] * iw + pixel11[1] * wx + 128U) >> 8),
            .b = (uint8_t)((pixel10[2] * iw + pixel11[2] * wx + 128U) >> 8),
        };
        rgb888_t color = {
            .r = (uint8_t)((top.r * iwy + bottom.r * wy + 128U) >> 8),
            .g = (uint8_t)((top.g * iwy + bottom.g * wy + 128U) >> 8),
            .b = (uint8_t)((top.b * iwy + bottom.b * wy + 128U) >> 8),
        };
        if (channels >= 4) {
            uint16_t alpha0 = (uint16_t)((pixel00[3] * iw + pixel01[3] * wx + 128U) >> 8);
            uint16_t alpha1 = (uint16_t)((pixel10[3] * iw + pixel11[3] * wx + 128U) >> 8);
            uint16_t alpha = (uint16_t)((alpha0 * iwy + alpha1 * wy + 128U) >> 8);
            color.r = (uint8_t)(((uint16_t)color.r * alpha + 127U) / 255U);
            color.g = (uint8_t)(((uint16_t)color.g * alpha + 127U) / 255U);
            color.b = (uint8_t)(((uint16_t)color.b * alpha + 127U) / 255U);
        }
        put_pixel_logical_to(target,
                             plan->offset_x + render_x,
                             logical_y,
                             rgb888_to_rgb565(color),
                             rotation_deg);
    }
}

static void render_rgb888_pair_rows_to(const image_render_plan_t *plan,
                                       const uint8_t *source_row0,
                                       const uint8_t *source_row1,
                                       int channels,
                                       uint16_t rotation_deg,
                                       uint32_t source_y0,
                                       uint32_t source_y1,
                                       int *render_y_cursor,
                                       color_pixel_rgb565_data_t *target)
{
    if (!plan || !source_row0 || !source_row1 || !render_y_cursor || !target) {
        return;
    }

    while (*render_y_cursor < plan->render_height) {
        int render_y = *render_y_cursor;
        uint32_t plan_y0 = plan->source_y0_map[render_y];
        uint32_t plan_y1 = plan->source_y1_map[render_y];
        if (plan_y0 != source_y0 || plan_y1 != source_y1) {
            break;
        }
        blit_rgb888_row_with_plan_to(plan,
                                     source_row0,
                                     source_row1,
                                     channels,
                                     rotation_deg,
                                     render_y,
                                     target);
        ++(*render_y_cursor);
    }
}

static unsigned select_jpeg_scale_denom(uint32_t image_width,
                                        uint32_t image_height,
                                        uint32_t target_width,
                                        uint32_t target_height)
{
    if (image_width == 0 || image_height == 0 || target_width == 0 || target_height == 0) {
        return 1;
    }

    bool relax_target = ((uint64_t)image_width * image_height) >= EPHOTO_LARGE_JPEG_RELAX_AREA_PIXELS;
    uint32_t min_target_width = target_width;
    uint32_t min_target_height = target_height;
    if (relax_target) {
        min_target_width = (target_width * 3U + 3U) / 4U;
        min_target_height = (target_height * 3U + 3U) / 4U;
    }

    const unsigned candidates[] = {8, 4, 2, 1};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        unsigned denom = candidates[i];
        uint32_t scaled_w = image_width / denom;
        uint32_t scaled_h = image_height / denom;
        if (scaled_w == 0) {
            scaled_w = 1;
        }
        if (scaled_h == 0) {
            scaled_h = 1;
        }

        // Prefer the largest native JPEG downscale that still preserves at least
        // the final render resolution, so giant photos don't get fully decoded in software.
        if (scaled_w >= min_target_width && scaled_h >= min_target_height) {
            return denom;
        }
    }

    return 1;
}

static unsigned probe_jpeg_scale_denom(const char *path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    unsigned scale_denom = 1;
    uint32_t image_width = 0;
    uint32_t image_height = 0;

    jpeg_software_header_info_t header_info = {0};
    if (probe_jpeg_software_header(path, &header_info) &&
        header_info.width > 0 && header_info.height > 0) {
        image_width = header_info.width;
        image_height = header_info.height;
    }

    if (image_width > 0 && image_height > 0) {
        uint16_t source_rotation_deg = normalize_source_rotation_deg(lookup_photo_source_rotation_deg(path));
        uint32_t oriented_width = image_width;
        uint32_t oriented_height = image_height;
        get_oriented_source_dimensions(image_width,
                                       image_height,
                                       source_rotation_deg,
                                       &oriented_width,
                                       &oriented_height);
        image_render_plan_t plan;
        memset(&plan, 0, sizeof(plan));
        if (build_render_plan(oriented_width, oriented_height, fit_mode, rotation_deg, &plan) == ESP_OK) {
            scale_denom = select_jpeg_scale_denom(oriented_width,
                                                  oriented_height,
                                                  (uint32_t)plan.render_width,
                                                  (uint32_t)plan.render_height);
            ESP_LOGI(TAG,
                     "jpeg scale probe %s: src=%ux%u oriented=%ux%u render=%dx%d chosen=1/%u",
                     path,
                     image_width,
                     image_height,
                     oriented_width,
                     oriented_height,
                     plan.render_width,
                     plan.render_height,
                     scale_denom);
            free_render_plan(&plan);
        }
    }
    return scale_denom;
}

static void blit_rgb888_row_to_render_target(const image_render_plan_t *plan,
                                             const uint8_t *source_row,
                                             int channels,
                                             uint32_t source_y,
                                             uint16_t rotation_deg,
                                             color_pixel_rgb565_data_t *target)
{
    if (!plan || !source_row || !plan->source_x0_map || !plan->source_x1_map ||
        !plan->source_x_weight_map || channels < 3 || source_y >= plan->source_height || !target) {
        return;
    }

    int render_y_start = (int)(((uint64_t)source_y * plan->render_height + plan->source_height - 1U) /
                               plan->source_height);
    int render_y_end = (int)((((uint64_t)source_y + 1U) * plan->render_height + plan->source_height - 1U) /
                             plan->source_height);
    if (render_y_start < 0) {
        render_y_start = 0;
    }
    if (render_y_end > plan->render_height) {
        render_y_end = plan->render_height;
    }

    for (int render_y = render_y_start; render_y < render_y_end; ++render_y) {
        int logical_y = plan->offset_y + render_y;
        for (int render_x = 0; render_x < plan->render_width; ++render_x) {
            const uint8_t *pixel0 = source_row + (size_t)plan->source_x0_map[render_x] * channels;
            const uint8_t *pixel1 = source_row + (size_t)plan->source_x1_map[render_x] * channels;
            uint16_t wx = plan->source_x_weight_map[render_x];
            uint16_t iw = 256U - wx;
            rgb888_t color = {
                .r = (uint8_t)((pixel0[0] * iw + pixel1[0] * wx + 128U) >> 8),
                .g = (uint8_t)((pixel0[1] * iw + pixel1[1] * wx + 128U) >> 8),
                .b = (uint8_t)((pixel0[2] * iw + pixel1[2] * wx + 128U) >> 8),
            };
            put_pixel_logical_to(target,
                                 plan->offset_x + render_x,
                                 logical_y,
                                 rgb888_to_rgb565(color),
                                 rotation_deg);
        }
    }
}

static unsigned clamp_software_stream_scale_denom(uint32_t image_width,
                                                  uint32_t image_height,
                                                  unsigned scale_denom)
{
    unsigned denom = scale_denom == 0 ? 1U : scale_denom;
    const size_t max_pixels = s_framebuffer_pixels > 0 ? (s_framebuffer_pixels * 2U) : (size_t)(800U * 1280U * 2U);

    while (denom < 8U) {
        uint32_t scaled_w = image_width / denom;
        uint32_t scaled_h = image_height / denom;
        if (scaled_w == 0) {
            scaled_w = 1;
        }
        if (scaled_h == 0) {
            scaled_h = 1;
        }
        if ((size_t)scaled_w * (size_t)scaled_h <= max_pixels) {
            break;
        }
        denom *= 2U;
    }

    if (denom > 8U) {
        denom = 8U;
    }
    return denom;
}

static bool should_force_full_scale_software_jpeg(const jpeg_software_header_info_t *header_info)
{
    return header_info &&
           header_info->valid &&
           header_info->progressive_mode;
}

static bool should_render_photo(const ephoto_app_state_t *state)
{
    return state->settings.screen_on &&
           state->storage.mounted &&
           state->storage.photo_count > 0 &&
           state->current_photo_path[0] != '\0';
}

static void apply_rotation(uint16_t rotation_deg)
{
    s_rotation_deg = rotation_deg == 90 ? 90 : 0;
}

static ephoto_brightness_t normalize_brightness(ephoto_brightness_t brightness)
{
    if (brightness > EPHOTO_BRIGHTNESS_MAX) {
        return EPHOTO_BRIGHTNESS_MAX;
    }
    return brightness;
}

static esp_err_t init_backlight_pwm(const board_profile_t *profile)
{
    if (!profile || profile->backlight == GPIO_NUM_NC || !profile->backlight_pwm) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = EPHOTO_BACKLIGHT_PWM_MODE,
        .duty_resolution = EPHOTO_BACKLIGHT_PWM_RES,
        .timer_num = EPHOTO_BACKLIGHT_PWM_TIMER,
        .freq_hz = EPHOTO_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight pwm timer config failed");

    ledc_channel_config_t channel_cfg = {
        .gpio_num = profile->backlight,
        .speed_mode = EPHOTO_BACKLIGHT_PWM_MODE,
        .channel = EPHOTO_BACKLIGHT_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = EPHOTO_BACKLIGHT_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "backlight pwm channel config failed");
    s_backlight_pwm_ready = true;
    return ESP_OK;
}

static void apply_backlight_level(ephoto_brightness_t brightness, bool screen_on)
{
    if (!s_profile || s_profile->backlight == GPIO_NUM_NC) {
        return;
    }

    ephoto_brightness_t effective = screen_on ? normalize_brightness(brightness) : EPHOTO_BRIGHTNESS_MIN;
    if (s_backlight_pwm_ready) {
        uint32_t duty = ((uint32_t)effective * EPHOTO_BACKLIGHT_PWM_DUTY_MAX + 50U) / 100U;
        esp_err_t err = ledc_set_duty(EPHOTO_BACKLIGHT_PWM_MODE, EPHOTO_BACKLIGHT_PWM_CHANNEL, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(EPHOTO_BACKLIGHT_PWM_MODE, EPHOTO_BACKLIGHT_PWM_CHANNEL);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "backlight pwm update failed: %s", esp_err_to_name(err));
        }
        return;
    }

    gpio_set_level(s_profile->backlight, effective > 0 ? 1 : 0);
}

static void apply_display_power(bool screen_on)
{
    if (!s_panel) {
        return;
    }
    if (s_display_on != screen_on) {
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, screen_on);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel disp_on_off failed: %s", esp_err_to_name(err));
        } else {
            s_display_on = screen_on;
        }
    }

}

static esp_err_t enable_dsi_phy_power(const board_profile_t *profile)
{
    if (profile->dsi_phy_ldo_chan < 0 || profile->dsi_phy_ldo_voltage_mv <= 0) {
        return ESP_OK;
    }
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = profile->dsi_phy_ldo_chan,
        .voltage_mv = profile->dsi_phy_ldo_voltage_mv,
    };
    return esp_ldo_acquire_channel(&ldo_cfg, &s_dsi_phy_ldo);
}

static esp_err_t ensure_jpeg_decoder(void)
{
    if (s_jpeg_decoder) {
        return ESP_OK;
    }

    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .timeout_ms = 200,
    };
    return jpeg_new_decoder_engine(&decode_eng_cfg, &s_jpeg_decoder);
}

static esp_err_t ensure_input_buffer(size_t required_size)
{
    if (s_jpeg_input_capacity >= required_size && s_jpeg_input_buffer) {
        return ESP_OK;
    }

    free(s_jpeg_input_buffer);
    s_jpeg_input_buffer = NULL;
    s_jpeg_input_capacity = 0;

    jpeg_decode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    s_jpeg_input_buffer = jpeg_alloc_decoder_mem(required_size, &input_cfg, &s_jpeg_input_capacity);
    if (!s_jpeg_input_buffer) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ensure_output_buffer(size_t required_size)
{
    if (s_jpeg_output_capacity >= required_size && s_jpeg_output_buffer) {
        return ESP_OK;
    }

    free(s_jpeg_output_buffer);
    s_jpeg_output_buffer = NULL;
    s_jpeg_output_capacity = 0;

    jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_jpeg_output_buffer = jpeg_alloc_decoder_mem(required_size, &output_cfg, &s_jpeg_output_capacity);
    if (!s_jpeg_output_buffer) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void release_hardware_jpeg_buffers(void)
{
    free(s_jpeg_input_buffer);
    s_jpeg_input_buffer = NULL;
    s_jpeg_input_capacity = 0;

    free(s_jpeg_output_buffer);
    s_jpeg_output_buffer = NULL;
    s_jpeg_output_capacity = 0;
}

static esp_err_t ensure_boot_image_mapped(void)
{
    if (s_bootimg_pixels) {
        return ESP_OK;
    }

    const char *label = boot_image_service_is_custom_selected()
                            ? EPHOTO_BOOT_IMAGE_PARTITION_LABEL
                            : EPHOTO_BOOT_IMAGE_DEFAULT_PARTITION_LABEL;
    if (boot_image_service_is_custom_selected()) {
        if (!s_bootimg_partition) {
            s_bootimg_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           label);
        }
    } else if (!s_bootimg_default_partition) {
        s_bootimg_default_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               ESP_PARTITION_SUBTYPE_ANY,
                                                               label);
    }

    const esp_partition_t *partition = boot_image_service_is_custom_selected()
                                           ? s_bootimg_partition
                                           : s_bootimg_default_partition;
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG, "boot image partition not found");
    ESP_RETURN_ON_FALSE(partition->size >= EPHOTO_BOOT_IMAGE_SIZE_BYTES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "boot image partition too small");

    void *buffer = heap_caps_malloc(EPHOTO_BOOT_IMAGE_SIZE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = malloc(EPHOTO_BOOT_IMAGE_SIZE_BYTES);
    }
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "boot image buffer alloc failed");

    esp_err_t err = esp_partition_read(partition, 0, buffer, EPHOTO_BOOT_IMAGE_SIZE_BYTES);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }
    s_bootimg_pixels = buffer;
    return ESP_OK;
}

static esp_err_t ensure_owned_jpeg_file_rgb565(const char *source_path,
                                               ephoto_fit_mode_t fit_mode,
                                               uint16_t rotation_deg,
                                               color_pixel_rgb565_data_t **out_target)
{
    if (!source_path || !source_path[0] || !out_target) {
        return ESP_ERR_INVALID_ARG;
    }

    image_type_t sniffed_type = sniff_image_type_from_file(source_path);
    image_type_t image_type = sniffed_type != IMAGE_TYPE_UNKNOWN ? sniffed_type : detect_image_type(source_path);

    if (image_type == IMAGE_TYPE_JPEG) {
        jpeg_software_header_info_t header_info = {0};
        probe_jpeg_software_header(source_path, &header_info);
        if (header_info.valid) {
            return render_jpeg_file_sw_scanline_to_owned_buffer(source_path,
                                                                fit_mode,
                                                                rotation_deg,
                                                                1U,
                                                                out_target);
        }
    }

    if (image_type == IMAGE_TYPE_PNG) {
        color_pixel_rgb565_data_t *target = heap_caps_malloc(s_framebuffer_bytes,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!target) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = render_png_file_sw_to_buffer(source_path, fit_mode, rotation_deg, target);
        if (err != ESP_OK) {
            free(target);
            return err;
        }
        *out_target = target;
        return ESP_OK;
    }

    if (image_type == IMAGE_TYPE_BMP) {
        color_pixel_rgb565_data_t *target = heap_caps_malloc(s_framebuffer_bytes,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!target) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = render_bmp_file_sw_to_buffer(source_path, fit_mode, rotation_deg, target);
        if (err != ESP_OK) {
            free(target);
            return err;
        }
        *out_target = target;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t decode_screen_jpeg_cache_to_buffer(const uint8_t *jpeg_data,
                                                    size_t jpeg_size,
                                                    uint16_t rotation_deg,
                                                    color_pixel_rgb565_data_t *target)
{
    if (!jpeg_data || jpeg_size == 0 || !target) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_jpeg_decoder(), TAG, "jpeg decoder unavailable");

    jpeg_decode_picture_info_t info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(jpeg_data, (uint32_t)jpeg_size, &info),
                        TAG,
                        "screen cache JPEG header parse failed");

    uint16_t expected_width = 0;
    uint16_t expected_height = 0;
    get_cache_frame_dimensions(rotation_deg, &expected_width, &expected_height);

    uint32_t aligned_width = align_up_u32(info.width, 16);
    uint32_t aligned_height = align_up_u32(info.height, 16);
    if (info.width != expected_width ||
        info.height != expected_height ||
        aligned_width != expected_width ||
        aligned_height != expected_height) {
        ESP_LOGW(TAG,
                 "screen cache JPEG size mismatch: got=%" PRIu32 "x%" PRIu32 " aligned=%" PRIu32 "x%" PRIu32
                 " expected=%ux%u",
                 info.width,
                 info.height,
                 aligned_width,
                 aligned_height,
                 expected_width,
                 expected_height);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t required_output_size = (size_t)aligned_width * aligned_height * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(ensure_output_buffer(required_output_size), TAG, "screen cache JPEG output alloc failed");

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    ESP_RETURN_ON_ERROR(jpeg_decoder_process(s_jpeg_decoder,
                                             &decode_cfg,
                                             jpeg_data,
                                             (uint32_t)jpeg_size,
                                             s_jpeg_output_buffer,
                                             s_jpeg_output_capacity,
                                             &out_size),
                        TAG,
                        "screen cache JPEG decode failed");
    if (out_size < s_framebuffer_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(target, s_jpeg_output_buffer, s_framebuffer_bytes);
    return ESP_OK;
}

static esp_err_t decode_jpeg_file_sw_to_decoded_rgb565(const char *path,
                                                       const jpeg_software_header_info_t *header_info,
                                                       ephoto_fit_mode_t fit_mode,
                                                       uint16_t rotation_deg,
                                                       decoded_jpeg_t *out_image)
{
    if (!path || !out_image) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_image, 0, sizeof(*out_image));

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_FAIL;
    struct jpeg_decompress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    software_jpeg_error_mgr_t jerr;
    memset(&jerr, 0, sizeof(jerr));
    volatile bool jpeg_created = false;
    uint16_t *pixel_buffer = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = software_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        ESP_LOGW(TAG,
                 "software jpeg decode-to-raw failed for %s: %s",
                 path,
                 jerr.message[0] ? jerr.message : "unknown");
        ret = ESP_FAIL;
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_created = true;
    jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);

    unsigned requested_scale_denom = probe_jpeg_scale_denom(path, fit_mode, rotation_deg);
    unsigned actual_scale_denom = requested_scale_denom;
    if (!should_force_full_scale_software_jpeg(header_info ? header_info : &(jpeg_software_header_info_t){0})) {
        actual_scale_denom = clamp_software_stream_scale_denom(cinfo.image_width,
                                                               cinfo.image_height,
                                                               requested_scale_denom);
    }
    if (actual_scale_denom != requested_scale_denom) {
        ESP_LOGI(TAG,
                 "raw software jpeg scale adjusted for %s: requested=1/%u adjusted=1/%u",
                 path,
                 requested_scale_denom,
                 actual_scale_denom);
    }

    cinfo.scale_num = 1;
    cinfo.scale_denom = actual_scale_denom;
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.do_block_smoothing = FALSE;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&cinfo);

    size_t pixel_count = (size_t)cinfo.output_width * (size_t)cinfo.output_height;
    if (cinfo.output_width == 0 || cinfo.output_height == 0 || pixel_count == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    pixel_buffer = heap_caps_malloc(pixel_count * sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixel_buffer) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t row_stride = (size_t)cinfo.output_width * (size_t)cinfo.output_components;
    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo,
                                                   JPOOL_IMAGE,
                                                   (JDIMENSION)row_stride,
                                                   1);

    while (cinfo.output_scanline < cinfo.output_height) {
        if (jpeg_read_scanlines(&cinfo, buffer, 1) != 1) {
            ret = ESP_FAIL;
            goto cleanup;
        }

        uint32_t row = cinfo.output_scanline - 1U;
        uint16_t *dst_row = pixel_buffer + ((size_t)row * cinfo.output_width);
        const uint8_t *src_row = buffer[0];
        for (uint32_t x = 0; x < cinfo.output_width; ++x) {
            const uint8_t *pixel = src_row + ((size_t)x * cinfo.output_components);
            dst_row[x] = rgb888_to_rgb565((rgb888_t){
                .r = pixel[0],
                .g = pixel[1],
                .b = pixel[2],
            });
        }
        cooperative_decode_pause(cinfo.output_scanline);
    }

    jpeg_finish_decompress(&cinfo);

    out_image->width = (uint16_t)cinfo.output_width;
    out_image->height = (uint16_t)cinfo.output_height;
    out_image->aligned_width = (uint16_t)cinfo.output_width;
    out_image->aligned_height = (uint16_t)cinfo.output_height;
    out_image->pixels = pixel_buffer;
    pixel_buffer = NULL;
    ret = ESP_OK;

cleanup:
    free(pixel_buffer);
    if (jpeg_created) {
        jpeg_destroy_decompress(&cinfo);
    }
    fclose(file);
    return ret;
}

static esp_err_t render_jpeg_file_sw_scanline_to_owned_buffer(const char *path,
                                                              ephoto_fit_mode_t fit_mode,
                                                              uint16_t rotation_deg,
                                                              unsigned scale_denom,
                                                              color_pixel_rgb565_data_t **out_target)
{
    if (!path || !out_target) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_target = NULL;

    esp_err_t ret = ESP_FAIL;
    int64_t render_started_ms = clock_service_now_ms();
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    struct jpeg_decompress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    software_jpeg_error_mgr_t jerr;
    memset(&jerr, 0, sizeof(jerr));
    image_render_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    volatile bool jpeg_created = false;
    color_pixel_rgb565_data_t *target = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = software_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        ESP_LOGW(TAG, "scanline software jpeg decode failed for %s: %s", path, jerr.message[0] ? jerr.message : "unknown");
        ret = ESP_FAIL;
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_created = true;
    jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);
    ESP_LOGI(TAG,
             "scanline software jpeg header %s: %ux%u components=%d progressive=%d arith=%d color_space=%d",
             path,
             cinfo.image_width,
             cinfo.image_height,
             cinfo.num_components,
             cinfo.progressive_mode,
             cinfo.arith_code,
             cinfo.jpeg_color_space);
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale_denom > 0 ? scale_denom : 1U;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    ret = build_render_plan(cinfo.output_width, cinfo.output_height, fit_mode, rotation_deg, &plan);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    target = heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!target) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    paint_buffer(target, 0x0000);
    int row_stride = (int)cinfo.output_width * cinfo.output_components;
    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, (JDIMENSION)row_stride, 1);

    while (cinfo.output_scanline < cinfo.output_height) {
        if (jpeg_read_scanlines(&cinfo, buffer, 1) != 1) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        blit_rgb888_row_to_render_target(&plan,
                                         buffer[0],
                                         cinfo.output_components,
                                         cinfo.output_scanline - 1U,
                                         rotation_deg,
                                         target);
        cooperative_decode_pause(cinfo.output_scanline);
    }

    jpeg_finish_decompress(&cinfo);
    ESP_LOGI(TAG,
             "scanline software jpeg finished %s: scale=1/%u output=%ux%u total_elapsed=%lldms",
             path,
             cinfo.scale_denom,
             cinfo.output_width,
             cinfo.output_height,
             (long long)(clock_service_now_ms() - render_started_ms));
    *out_target = target;
    target = NULL;
    ret = ESP_OK;

cleanup:
    free(target);
    free_render_plan(&plan);
    if (jpeg_created) {
        jpeg_destroy_decompress(&cinfo);
    }
    fclose(file);
    return ret;
}


static esp_err_t generate_jpeg_display_cache(const char *path,
                                             const jpeg_software_header_info_t *header_info,
                                             ephoto_fit_mode_t fit_mode,
                                             uint16_t rotation_deg)
{
    decoded_jpeg_t image = {0};
    color_pixel_rgb565_data_t *cache_buffer = NULL;
    esp_err_t err = ESP_FAIL;
    int64_t started_ms = clock_service_now_ms();
    const char *software_reason = header_info ? choose_software_jpeg_reason(header_info) : NULL;
    uint16_t source_rotation_deg = normalize_source_rotation_deg(lookup_photo_source_rotation_deg(path));

    if (header_info && header_info->valid) {
        if (software_reason) {
            ESP_LOGI(TAG, "jpeg cache for %s may need software stream path: %s", path, software_reason);
        } else {
            ESP_LOGI(TAG,
                     "jpeg cache for %s forces software decode: src=%" PRIu32 "x%" PRIu32,
                     path,
                     header_info->width,
                     header_info->height);
        }
        release_runtime_framebuffers_for_cache_build();
        log_spiram_state("after_release_runtime_fb");
    }

    log_spiram_state("before_raw_sw_decode");
    err = decode_jpeg_file_sw_to_decoded_rgb565(path,
                                                header_info,
                                                fit_mode,
                                                rotation_deg,
                                                &image);
    log_spiram_state(err == ESP_OK ? "after_raw_sw_decode_ok" : "after_raw_sw_decode_fail");
    if (err != ESP_OK) {
        goto cleanup;
    }

    cache_buffer = heap_caps_malloc(s_framebuffer_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cache_buffer) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = save_decoded_jpeg_cache(path,
                                  &image,
                                  fit_mode,
                                  rotation_deg,
                                  source_rotation_deg,
                                  cache_buffer);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "jpeg cache applied source rotation for %s: source_rot=%u display_rot=%u",
                 path,
                 (unsigned)source_rotation_deg,
                 (unsigned)rotation_deg);
    }

cleanup:
    free(cache_buffer);
    free(image.pixels);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "prepared display cache for %s in %lldms", path, (long long)(clock_service_now_ms() - started_ms));
    }
    return err;
}

static bool should_defer_foreground_render(const char *path,
                                           size_t size_bytes,
                                           uint32_t width,
                                           uint32_t height,
                                           ephoto_fit_mode_t fit_mode,
                                           uint16_t rotation_deg)
{
    if (!path || !path[0]) {
        return false;
    }
    if (display_service_has_cache(path, fit_mode, rotation_deg)) {
        return false;
    }

    image_type_t sniffed_type = sniff_image_type_from_file(path);
    image_type_t image_type = sniffed_type != IMAGE_TYPE_UNKNOWN ? sniffed_type : detect_image_type(path);
    if (image_type == IMAGE_TYPE_JPEG) {
        return true;
    }

    return should_defer_uncached_photo_path(path, size_bytes, width, height);
}

static esp_err_t load_bmp_rgb_row(FILE *file,
                                  uint32_t pixel_offset,
                                  uint32_t row_stride,
                                  uint32_t width,
                                  uint32_t source_y,
                                  uint32_t height,
                                  bool top_down,
                                  uint16_t bits_per_pixel,
                                  uint8_t *raw_row,
                                  uint8_t *rgb_row)
{
    if (!file || !raw_row || !rgb_row || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t file_row = top_down ? source_y : (height - 1U - source_y);
    long offset = (long)(pixel_offset + (uint64_t)file_row * row_stride);
    if (fseek(file, offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(raw_row, 1, row_stride, file) != row_stride) {
        return ESP_FAIL;
    }

    if (bits_per_pixel == 24) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t src = (size_t)x * 3U;
            size_t dst = src;
            rgb_row[dst] = raw_row[src + 2];
            rgb_row[dst + 1] = raw_row[src + 1];
            rgb_row[dst + 2] = raw_row[src];
        }
        return ESP_OK;
    }

    if (bits_per_pixel == 32) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t src = (size_t)x * 4U;
            size_t dst = src;
            rgb_row[dst] = raw_row[src + 2];
            rgb_row[dst + 1] = raw_row[src + 1];
            rgb_row[dst + 2] = raw_row[src];
            rgb_row[dst + 3] = raw_row[src + 3];
        }
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t render_bmp_file_sw_to_buffer(const char *path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              color_pixel_rgb565_data_t *target)
{
    release_hardware_jpeg_buffers();

    if (!target) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t file_header[14] = {0};
    uint8_t dib_header[40] = {0};
    image_render_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    uint8_t *raw_row0 = NULL;
    uint8_t *raw_row1 = NULL;
    uint8_t *rgb_row0 = NULL;
    uint8_t *rgb_row1 = NULL;
    esp_err_t ret = ESP_FAIL;

    if (fread(file_header, 1, sizeof(file_header), file) != sizeof(file_header) ||
        fread(dib_header, 1, sizeof(dib_header), file) != sizeof(dib_header)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    if (file_header[0] != 'B' || file_header[1] != 'M') {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    uint32_t pixel_offset = read_le32_mem(&file_header[10]);
    uint32_t dib_size = read_le32_mem(&dib_header[0]);
    int32_t width_signed = read_le32s_mem(&dib_header[4]);
    int32_t height_signed = read_le32s_mem(&dib_header[8]);
    uint16_t planes = read_le16_mem(&dib_header[12]);
    uint16_t bits_per_pixel = read_le16_mem(&dib_header[14]);
    uint32_t compression = read_le32_mem(&dib_header[16]);

    if (dib_size < 40 || width_signed <= 0 || height_signed == 0 || planes != 1) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    if (bits_per_pixel != 24 && bits_per_pixel != 32) {
        ESP_LOGW(TAG, "bmp unsupported bits-per-pixel for %s: %u", path, bits_per_pixel);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    if (compression != 0) {
        ESP_LOGW(TAG, "bmp unsupported compression for %s: %u", path, compression);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    uint32_t width = (uint32_t)width_signed;
    uint32_t height = (height_signed < 0) ? (uint32_t)(-height_signed) : (uint32_t)height_signed;
    bool top_down = height_signed < 0;
    int channels = bits_per_pixel == 32 ? 4 : 3;
    uint32_t src_row_stride = ((width * bits_per_pixel + 31U) / 32U) * 4U;
    size_t rgb_row_bytes = (size_t)width * channels;

    ret = build_render_plan(width, height, fit_mode, rotation_deg, &plan);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    raw_row0 = heap_caps_malloc(src_row_stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    raw_row1 = heap_caps_malloc(src_row_stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rgb_row0 = heap_caps_malloc(rgb_row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rgb_row1 = heap_caps_malloc(rgb_row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_row0 || !raw_row1 || !rgb_row0 || !rgb_row1) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    paint_buffer(target, 0x0000);

    int render_y_cursor = 0;
    for (uint32_t source_y = 0; source_y + 1U < height; ++source_y) {
        ret = load_bmp_rgb_row(file,
                               pixel_offset,
                               src_row_stride,
                               width,
                               source_y,
                               height,
                               top_down,
                               bits_per_pixel,
                               raw_row0,
                               rgb_row0);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = load_bmp_rgb_row(file,
                               pixel_offset,
                               src_row_stride,
                               width,
                               source_y + 1U,
                               height,
                               top_down,
                               bits_per_pixel,
                               raw_row1,
                               rgb_row1);
        if (ret != ESP_OK) {
            goto cleanup;
        }

        render_rgb888_pair_rows_to(&plan,
                                   rgb_row0,
                                   rgb_row1,
                                   channels,
                                   rotation_deg,
                                   source_y,
                                   source_y + 1U,
                                   &render_y_cursor,
                                   target);
        cooperative_decode_pause(source_y);
    }

    if (render_y_cursor < plan.render_height && height > 0) {
        ret = load_bmp_rgb_row(file,
                               pixel_offset,
                               src_row_stride,
                               width,
                               height - 1U,
                               height,
                               top_down,
                               bits_per_pixel,
                               raw_row0,
                               rgb_row0);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        render_rgb888_pair_rows_to(&plan,
                                   rgb_row0,
                                   rgb_row0,
                                   channels,
                                   rotation_deg,
                                   height - 1U,
                                   height - 1U,
                                   &render_y_cursor,
                                   target);
    }

    ret = ESP_OK;

cleanup:
    free(raw_row0);
    free(raw_row1);
    free(rgb_row0);
    free(rgb_row1);
    free_render_plan(&plan);
    fclose(file);
    return ret;
}

static esp_err_t render_png_file_sw_to_buffer(const char *path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              color_pixel_rgb565_data_t *target)
{
    release_hardware_jpeg_buffers();

    if (!target) {
        return ESP_ERR_INVALID_ARG;
    }

    volatile esp_err_t ret = ESP_FAIL;
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    image_render_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    volatile uint8_t *row_prev = NULL;
    volatile uint8_t *row_curr = NULL;

    if (setjmp(png_jmpbuf(png)) != 0) {
        ESP_LOGW(TAG, "png decode failed for %s", path);
        goto cleanup;
    }

    png_init_io(png, file);
    png_read_info(png, info);

    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    int bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);
    int channels = png_get_channels(png, info);
    png_size_t rowbytes = png_get_rowbytes(png, info);

    ret = build_render_plan(width, height, fit_mode, rotation_deg, &plan);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    paint_buffer(target, 0x0000);
    row_prev = heap_caps_malloc(rowbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    row_curr = heap_caps_malloc(rowbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row_prev || !row_curr) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    int render_y_cursor = 0;
    png_read_row(png, (png_bytep)row_prev, NULL);
    for (png_uint_32 source_y = 0; source_y + 1U < height; ++source_y) {
        png_read_row(png, (png_bytep)row_curr, NULL);
        render_rgb888_pair_rows_to(&plan,
                                   (const uint8_t *)row_prev,
                                   (const uint8_t *)row_curr,
                                   channels,
                                   rotation_deg,
                                   source_y,
                                   source_y + 1U,
                                   &render_y_cursor,
                                   target);
        volatile uint8_t *swap = row_prev;
        row_prev = row_curr;
        row_curr = swap;
        cooperative_decode_pause(source_y);
    }

    if (render_y_cursor < plan.render_height && height > 0) {
        render_rgb888_pair_rows_to(&plan,
                                   (const uint8_t *)row_prev,
                                   (const uint8_t *)row_prev,
                                   channels,
                                   rotation_deg,
                                   height - 1U,
                                   height - 1U,
                                   &render_y_cursor,
                                   target);
    }
    png_read_end(png, NULL);

    ret = ESP_OK;

cleanup:
    free((void *)row_prev);
    free((void *)row_curr);
    free_render_plan(&plan);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(file);
    return (esp_err_t)ret;
}

static void map_logical_to_physical(int logical_x, int logical_y, uint16_t rotation_deg, int *physical_x, int *physical_y)
{
    (void)rotation_deg;
    *physical_x = logical_x;
    *physical_y = logical_y;
}

static esp_err_t blit_decoded_image_to_buffer(const decoded_jpeg_t *image,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              uint16_t source_rotation_deg,
                                              color_pixel_rgb565_data_t *target)
{
    if (!image || !image->pixels || !target) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t oriented_width = 0;
    uint32_t oriented_height = 0;
    get_oriented_source_dimensions(image->width,
                                   image->height,
                                   source_rotation_deg,
                                   &oriented_width,
                                   &oriented_height);

    image_render_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    if (build_render_plan(oriented_width, oriented_height, fit_mode, rotation_deg, &plan) != ESP_OK) {
        return ESP_FAIL;
    }

    paint_buffer(target, 0x0000);

    for (int render_y = 0; render_y < plan.render_height; ++render_y) {
        uint32_t src_y0 = plan.source_y0_map[render_y];
        uint32_t src_y1 = plan.source_y1_map[render_y];
        for (int render_x = 0; render_x < plan.render_width; ++render_x) {
            uint32_t src_x0 = plan.source_x0_map[render_x];
            uint32_t src_x1 = plan.source_x1_map[render_x];
            uint16_t wx = plan.source_x_weight_map[render_x];
            uint16_t iwx = 256U - wx;
            uint32_t raw_x00 = 0;
            uint32_t raw_y00 = 0;
            uint32_t raw_x01 = 0;
            uint32_t raw_y01 = 0;
            uint32_t raw_x10 = 0;
            uint32_t raw_y10 = 0;
            uint32_t raw_x11 = 0;
            uint32_t raw_y11 = 0;

            map_oriented_source_to_raw(image->width, image->height, source_rotation_deg, src_x0, src_y0, &raw_x00, &raw_y00);
            map_oriented_source_to_raw(image->width, image->height, source_rotation_deg, src_x1, src_y0, &raw_x01, &raw_y01);
            map_oriented_source_to_raw(image->width, image->height, source_rotation_deg, src_x0, src_y1, &raw_x10, &raw_y10);
            map_oriented_source_to_raw(image->width, image->height, source_rotation_deg, src_x1, src_y1, &raw_x11, &raw_y11);

            rgb888_t c00 = rgb565_to_rgb888(image->pixels[(size_t)raw_y00 * image->aligned_width + raw_x00]);
            rgb888_t c01 = rgb565_to_rgb888(image->pixels[(size_t)raw_y01 * image->aligned_width + raw_x01]);
            rgb888_t c10 = rgb565_to_rgb888(image->pixels[(size_t)raw_y10 * image->aligned_width + raw_x10]);
            rgb888_t c11 = rgb565_to_rgb888(image->pixels[(size_t)raw_y11 * image->aligned_width + raw_x11]);

            uint32_t top_r = c00.r * iwx + c01.r * wx;
            uint32_t top_g = c00.g * iwx + c01.g * wx;
            uint32_t top_b = c00.b * iwx + c01.b * wx;
            uint32_t bot_r = c10.r * iwx + c11.r * wx;
            uint32_t bot_g = c10.g * iwx + c11.g * wx;
            uint32_t bot_b = c10.b * iwx + c11.b * wx;

            rgb888_t color = {
                .r = (uint8_t)(((top_r + bot_r + 256U) >> 9) & 0xFF),
                .g = (uint8_t)(((top_g + bot_g + 256U) >> 9) & 0xFF),
                .b = (uint8_t)(((top_b + bot_b + 256U) >> 9) & 0xFF),
            };

            put_pixel_logical_to(target,
                                 plan.offset_x + render_x,
                                 plan.offset_y + render_y,
                                 rgb888_to_rgb565(color),
                                 rotation_deg);
        }
        cooperative_decode_pause((uint32_t)render_y);
    }

    free_render_plan(&plan);
    return ESP_OK;
}

static esp_err_t save_decoded_jpeg_cache(const char *path,
                                         const decoded_jpeg_t *image,
                                         ephoto_fit_mode_t fit_mode,
                                         uint16_t rotation_deg,
                                         uint16_t source_rotation_deg,
                                         color_pixel_rgb565_data_t *cache_buffer)
{
    if (!path || !image || !image->pixels || !cache_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = blit_decoded_image_to_buffer(image,
                                                 fit_mode,
                                                 rotation_deg,
                                                 source_rotation_deg,
                                                 cache_buffer);
    if (err != ESP_OK) {
        return err;
    }
    return save_cached_buffer(path, rotation_deg, fit_mode, cache_buffer);
}

bool display_service_has_cache(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return false;
    }
    return load_cache_header(source_path, rotation_deg, fit_mode, NULL, NULL, 0) == ESP_OK;
}

bool display_service_has_known_cache_failure(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return false;
    }
    return load_cache_failure_header(source_path, rotation_deg, fit_mode, NULL, NULL, 0) == ESP_OK;
}

esp_err_t display_service_get_cache_failure(const char *source_path,
                                            ephoto_fit_mode_t fit_mode,
                                            uint16_t rotation_deg,
                                            esp_err_t *out_error)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0] || !out_error) {
        return ESP_ERR_INVALID_ARG;
    }

    ephoto_cache_failure_header_t header = {0};
    esp_err_t err = load_cache_failure_header(source_path, rotation_deg, fit_mode, &header, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    *out_error = (esp_err_t)header.error_code;
    return ESP_OK;
}

bool display_service_has_web_thumbnail(const char *source_path, ephoto_fit_mode_t fit_mode)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return false;
    }
    return load_web_thumbnail_header(source_path, fit_mode, NULL, NULL, 0) == ESP_OK;
}

bool display_service_has_known_web_thumbnail_failure(const char *source_path, ephoto_fit_mode_t fit_mode)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return false;
    }
    return load_web_thumbnail_failure_header(source_path, fit_mode, NULL, NULL, 0) == ESP_OK;
}

esp_err_t display_service_get_web_thumbnail_failure(const char *source_path,
                                                    ephoto_fit_mode_t fit_mode,
                                                    esp_err_t *out_error)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0] || !out_error) {
        return ESP_ERR_INVALID_ARG;
    }

    ephoto_cache_failure_header_t header = {0};
    esp_err_t err = load_web_thumbnail_failure_header(source_path, fit_mode, &header, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    *out_error = (esp_err_t)header.error_code;
    return ESP_OK;
}

bool display_service_is_photo_ready(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!source_path || !source_path[0] || !s_display_mutex) {
        return false;
    }

    bool ready = false;
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    ready = base_photo_matches_locked(source_path, fit_mode, rotation_deg);
    xSemaphoreGive(s_display_mutex);
    return ready;
}

esp_err_t display_service_get_cache_jpeg_info(const char *source_path,
                                              ephoto_fit_mode_t fit_mode,
                                              uint16_t rotation_deg,
                                              char *out_cache_path,
                                              size_t out_cache_path_len,
                                              size_t *out_payload_offset,
                                              size_t *out_payload_size)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ephoto_cache_header_t header = {0};
    esp_err_t err = load_cache_header(source_path,
                                      rotation_deg,
                                      fit_mode,
                                      &header,
                                      out_cache_path,
                                      out_cache_path_len);
    if (err != ESP_OK) {
        return err;
    }

    if (out_payload_offset) {
        *out_payload_offset = sizeof(ephoto_cache_header_t);
    }
    if (out_payload_size) {
        *out_payload_size = header.payload_size;
    }
    return ESP_OK;
}

esp_err_t display_service_get_web_thumbnail_jpeg_info(const char *source_path,
                                                      ephoto_fit_mode_t fit_mode,
                                                      char *out_cache_path,
                                                      size_t out_cache_path_len,
                                                      size_t *out_payload_offset,
                                                      size_t *out_payload_size)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ephoto_cache_header_t header = {0};
    esp_err_t err = load_web_thumbnail_header(source_path,
                                              fit_mode,
                                              &header,
                                              out_cache_path,
                                              out_cache_path_len);
    if (err != ESP_OK) {
        return err;
    }

    if (out_payload_offset) {
        *out_payload_offset = sizeof(ephoto_cache_header_t);
    }
    if (out_payload_size) {
        *out_payload_size = header.payload_size;
    }
    return ESP_OK;
}

static esp_err_t decode_web_thumbnail_jpeg_to_allocated_rgb565(const uint8_t *jpeg_data,
                                                               size_t jpeg_size,
                                                               uint16_t max_width,
                                                               uint16_t max_height,
                                                               void **out_pixels,
                                                               uint16_t *out_width,
                                                               uint16_t *out_height)
{
    if (!jpeg_data || jpeg_size == 0 || !out_pixels || !out_width || !out_height) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    ESP_RETURN_ON_ERROR(ensure_jpeg_decoder(), TAG, "jpeg decoder unavailable");

    jpeg_decode_picture_info_t info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(jpeg_data, (uint32_t)jpeg_size, &info),
                        TAG,
                        "thumbnail JPEG header parse failed");

    uint32_t aligned_width = align_up_u32(info.width, 16);
    uint32_t aligned_height = align_up_u32(info.height, 16);
    size_t raw_output_size = (size_t)aligned_width * aligned_height * sizeof(uint16_t);
    size_t raw_output_capacity = 0;
    jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    color_pixel_rgb565_data_t *raw_output =
        jpeg_alloc_decoder_mem(raw_output_size, &output_cfg, &raw_output_capacity);
    if (!raw_output) {
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg_decoder,
                                         &decode_cfg,
                                         jpeg_data,
                                         (uint32_t)jpeg_size,
                                         (uint8_t *)raw_output,
                                         raw_output_capacity,
                                         &decoded_size);
    if (err != ESP_OK) {
        free(raw_output);
        return err;
    }

    uint16_t image_width = (uint16_t)info.width;
    uint16_t image_height = (uint16_t)info.height;
    color_pixel_rgb565_data_t *decoded_pixels = raw_output;

    if (aligned_width != info.width) {
        size_t compact_size = (size_t)info.width * info.height * sizeof(uint16_t);
        color_pixel_rgb565_data_t *compact = heap_caps_malloc(compact_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!compact) {
            free(raw_output);
            return ESP_ERR_NO_MEM;
        }
        for (uint32_t y = 0; y < info.height; ++y) {
            memcpy(compact + (size_t)y * info.width,
                   raw_output + (size_t)y * aligned_width,
                   (size_t)info.width * sizeof(uint16_t));
        }
        free(raw_output);
        decoded_pixels = compact;
    }

    uint16_t target_width = image_width;
    uint16_t target_height = image_height;
    if (max_width > 0 && max_height > 0 &&
        (image_width > max_width || image_height > max_height)) {
        uint32_t fit_w = max_width;
        uint32_t fit_h = (uint32_t)image_height * fit_w / image_width;
        if (fit_h == 0) {
            fit_h = 1;
        }
        if (fit_h > max_height) {
            fit_h = max_height;
            fit_w = (uint32_t)image_width * fit_h / image_height;
            if (fit_w == 0) {
                fit_w = 1;
            }
        }

        target_width = (uint16_t)fit_w;
        target_height = (uint16_t)fit_h;
        size_t scaled_size = (size_t)target_width * target_height * sizeof(uint16_t);
        color_pixel_rgb565_data_t *scaled = heap_caps_malloc(scaled_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!scaled) {
            free(decoded_pixels);
            return ESP_ERR_NO_MEM;
        }

        err = scale_rgb565_rect(decoded_pixels,
                                image_width,
                                image_height,
                                0,
                                0,
                                image_width,
                                image_height,
                                target_width,
                                target_height,
                                scaled);
        free(decoded_pixels);
        if (err != ESP_OK) {
            free(scaled);
            return err;
        }
        decoded_pixels = scaled;
    }

    *out_pixels = decoded_pixels;
    *out_width = target_width;
    *out_height = target_height;
    (void)decoded_size;
    return ESP_OK;
}

static esp_err_t decode_jpeg_memory_software_to_allocated_rgb565(const uint8_t *jpeg_data,
                                                                 size_t jpeg_size,
                                                                 uint16_t max_width,
                                                                 uint16_t max_height,
                                                                 void **out_pixels,
                                                                 uint16_t *out_width,
                                                                 uint16_t *out_height)
{
    if (!jpeg_data || jpeg_size == 0 || !out_pixels || !out_width || !out_height) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    struct jpeg_decompress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    software_jpeg_error_mgr_t jerr;
    memset(&jerr, 0, sizeof(jerr));
    volatile bool jpeg_created = false;
    color_pixel_rgb565_data_t *decoded_pixels = NULL;
    esp_err_t result = ESP_FAIL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = software_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer) != 0) {
        ESP_LOGW(TAG,
                 "software jpeg memory decode failed: %s",
                 jerr.message[0] ? jerr.message : "unknown");
        result = ESP_FAIL;
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_created = true;
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        result = ESP_FAIL;
        goto cleanup;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_components < 3) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    size_t decoded_size = (size_t)cinfo.output_width * cinfo.output_height * sizeof(color_pixel_rgb565_data_t);
    decoded_pixels = heap_caps_malloc(decoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded_pixels) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    JSAMPARRAY row_buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo,
                                                       JPOOL_IMAGE,
                                                       cinfo.output_width * cinfo.output_components,
                                                       1);
    if (!row_buffer) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        if (jpeg_read_scanlines(&cinfo, row_buffer, 1) != 1) {
            result = ESP_FAIL;
            goto cleanup;
        }
        uint32_t y = cinfo.output_scanline - 1U;
        const uint8_t *src = row_buffer[0];
        color_pixel_rgb565_data_t *dst = decoded_pixels + (size_t)y * cinfo.output_width;
        for (uint32_t x = 0; x < cinfo.output_width; ++x) {
            rgb888_t color = {
                .r = src[x * cinfo.output_components + 0],
                .g = src[x * cinfo.output_components + 1],
                .b = src[x * cinfo.output_components + 2],
            };
            dst[x].val = rgb888_to_rgb565(color);
        }
    }

    jpeg_finish_decompress(&cinfo);

    uint16_t target_width = (uint16_t)cinfo.output_width;
    uint16_t target_height = (uint16_t)cinfo.output_height;
    if (max_width > 0 && max_height > 0 &&
        (target_width > max_width || target_height > max_height)) {
        uint32_t fit_w = max_width;
        uint32_t fit_h = (uint32_t)target_height * fit_w / target_width;
        if (fit_h == 0) {
            fit_h = 1;
        }
        if (fit_h > max_height) {
            fit_h = max_height;
            fit_w = (uint32_t)target_width * fit_h / target_height;
            if (fit_w == 0) {
                fit_w = 1;
            }
        }

        size_t scaled_size = (size_t)fit_w * fit_h * sizeof(color_pixel_rgb565_data_t);
        color_pixel_rgb565_data_t *scaled = heap_caps_malloc(scaled_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!scaled) {
            result = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        result = scale_rgb565_rect(decoded_pixels,
                                   target_width,
                                   target_height,
                                   0,
                                   0,
                                   target_width,
                                   target_height,
                                   (uint16_t)fit_w,
                                   (uint16_t)fit_h,
                                   scaled);
        free(decoded_pixels);
        if (result != ESP_OK) {
            free(scaled);
            decoded_pixels = NULL;
            goto cleanup;
        }
        decoded_pixels = scaled;
        target_width = (uint16_t)fit_w;
        target_height = (uint16_t)fit_h;
    }

    *out_pixels = decoded_pixels;
    *out_width = target_width;
    *out_height = target_height;
    decoded_pixels = NULL;
    result = ESP_OK;

cleanup:
    free(decoded_pixels);
    if (jpeg_created) {
        jpeg_destroy_decompress(&cinfo);
    }
    return result;
}

esp_err_t display_service_decode_jpeg_rgb565(const uint8_t *jpeg_data,
                                             size_t jpeg_size,
                                             uint16_t max_width,
                                             uint16_t max_height,
                                             void **out_pixels,
                                             uint16_t *out_width,
                                             uint16_t *out_height)
{
    esp_err_t err = decode_web_thumbnail_jpeg_to_allocated_rgb565(jpeg_data,
                                                                  jpeg_size,
                                                                  max_width,
                                                                  max_height,
                                                                  out_pixels,
                                                                  out_width,
                                                                  out_height);
    if (err == ESP_OK && out_pixels && *out_pixels && out_width && *out_width > 0 && out_height && *out_height > 0) {
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "hardware jpeg memory decode failed, fallback to software: %s",
             esp_err_to_name(err));
    return decode_jpeg_memory_software_to_allocated_rgb565(jpeg_data,
                                                           jpeg_size,
                                                           max_width,
                                                           max_height,
                                                           out_pixels,
                                                           out_width,
                                                           out_height);
}

esp_err_t display_service_decode_jpeg_file_to_screen_rgb565(const char *source_path,
                                                            ephoto_fit_mode_t fit_mode,
                                                            uint16_t rotation_deg,
                                                            void **out_pixels,
                                                            uint16_t *out_width,
                                                            uint16_t *out_height)
{
    if (!out_pixels || !out_width || !out_height) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    color_pixel_rgb565_data_t *target = NULL;
    esp_err_t err = ensure_owned_jpeg_file_rgb565(source_path, fit_mode, rotation_deg, &target);
    if (err != ESP_OK) {
        return err;
    }

    *out_pixels = target;
    *out_width = EPHOTO_BOOT_IMAGE_WIDTH;
    *out_height = EPHOTO_BOOT_IMAGE_HEIGHT;
    return ESP_OK;
}

esp_err_t display_service_get_boot_image_rgb565(const void **out_pixels,
                                                uint16_t *out_width,
                                                uint16_t *out_height)
{
    if (!out_pixels || !out_width || !out_height) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    esp_err_t err = ensure_boot_image_mapped();
    if (err != ESP_OK) {
        return err;
    }

    *out_pixels = s_bootimg_pixels;
    *out_width = EPHOTO_BOOT_IMAGE_WIDTH;
    *out_height = EPHOTO_BOOT_IMAGE_HEIGHT;
    return ESP_OK;
}

esp_err_t display_service_load_web_thumbnail_rgb565(const char *source_path,
                                                    ephoto_fit_mode_t fit_mode,
                                                    uint16_t max_width,
                                                    uint16_t max_height,
                                                    void **out_pixels,
                                                    uint16_t *out_width,
                                                    uint16_t *out_height)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !s_profile || !source_path || !source_path[0] ||
        !out_pixels || !out_width || !out_height || !s_display_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    ephoto_cache_header_t header = {0};
    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    esp_err_t err = load_web_thumbnail_header(source_path,
                                              fit_mode,
                                              &header,
                                              cache_path,
                                              sizeof(cache_path));
    if (err != ESP_OK) {
        xSemaphoreGive(s_display_mutex);
        return err;
    }

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        xSemaphoreGive(s_display_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (lseek(fd, (off_t)sizeof(ephoto_cache_header_t), SEEK_SET) < 0) {
        close(fd);
        xSemaphoreGive(s_display_mutex);
        return ESP_FAIL;
    }
    err = ensure_input_buffer(header.payload_size);
    if (err != ESP_OK) {
        close(fd);
        xSemaphoreGive(s_display_mutex);
        return err;
    }

    size_t total_read = 0;
    while (total_read < header.payload_size) {
        ssize_t chunk = read(fd,
                             s_jpeg_input_buffer + total_read,
                             (size_t)header.payload_size - total_read);
        if (chunk <= 0) {
            break;
        }
        total_read += (size_t)chunk;
    }
    close(fd);
    if (total_read != header.payload_size) {
        xSemaphoreGive(s_display_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    err = decode_web_thumbnail_jpeg_to_allocated_rgb565(s_jpeg_input_buffer,
                                                        header.payload_size,
                                                        max_width,
                                                        max_height,
                                                        out_pixels,
                                                        out_width,
                                                        out_height);
    xSemaphoreGive(s_display_mutex);
    return err;
}

static esp_err_t load_cached_photo_into_buffer(const char *source_path,
                                               ephoto_fit_mode_t fit_mode,
                                               uint16_t rotation_deg,
                                               color_pixel_rgb565_data_t *target,
                                               uint32_t *out_payload_size,
                                               int64_t *out_read_time_ms,
                                               int64_t *out_decode_time_ms)
{
    if (!source_path || !source_path[0] || !target) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat src_stat;
    if (stat(source_path, &src_stat) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    ESP_RETURN_ON_ERROR(build_cache_path(source_path, rotation_deg, fit_mode, cache_path, sizeof(cache_path)),
                        TAG,
                        "cache path build failed");

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    struct stat cache_stat;
    if (fstat(fd, &cache_stat) != 0) {
        close(fd);
        return ESP_ERR_INVALID_STATE;
    }

    ephoto_cache_header_t header = {0};
    ssize_t header_bytes = read(fd, &header, sizeof(header));
    uint16_t expected_frame_width = 0;
    uint16_t expected_frame_height = 0;
    get_cache_frame_dimensions(rotation_deg, &expected_frame_width, &expected_frame_height);

    bool header_ok = header_bytes == sizeof(header) &&
                     header.magic == EPHOTO_CACHE_MAGIC &&
                     header.version == EPHOTO_CACHE_VERSION &&
                     header.format == EPHOTO_CACHE_FORMAT_SCREEN_JPEG &&
                     header.rotation_deg == rotation_deg &&
                     header.frame_width == expected_frame_width &&
                     header.frame_height == expected_frame_height &&
                     header.source_size == (uint64_t)src_stat.st_size &&
                     header.source_mtime == (int64_t)src_stat.st_mtime &&
                     header.payload_size > 0 &&
                     (size_t)cache_stat.st_size == sizeof(ephoto_cache_header_t) + (size_t)header.payload_size &&
                     header.source_hash == hash_source_key(source_path, rotation_deg, fit_mode);
    if (!header_ok) {
        close(fd);
        unlink(cache_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ensure_input_buffer(header.payload_size);
    if (err != ESP_OK) {
        close(fd);
        return err;
    }

    int64_t read_started_ms = clock_service_now_ms();
    size_t total_read = 0;
    while (total_read < header.payload_size) {
        ssize_t chunk = read(fd,
                             s_jpeg_input_buffer + total_read,
                             (size_t)header.payload_size - total_read);
        if (chunk <= 0) {
            break;
        }
        total_read += (size_t)chunk;
    }
    close(fd);
    if (total_read != header.payload_size) {
        unlink(cache_path);
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t decode_started_ms = clock_service_now_ms();
    err = decode_screen_jpeg_cache_to_buffer(s_jpeg_input_buffer, header.payload_size, rotation_deg, target);
    if (err != ESP_OK) {
        unlink(cache_path);
        return err;
    }

    if (out_payload_size) {
        *out_payload_size = header.payload_size;
    }
    if (out_read_time_ms) {
        *out_read_time_ms = decode_started_ms - read_started_ms;
    }
    if (out_decode_time_ms) {
        *out_decode_time_ms = clock_service_now_ms() - decode_started_ms;
    }
    return ESP_OK;
}

esp_err_t display_service_stage_photo(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!s_initialized || !source_path || !source_path[0] || !s_display_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(ensure_shadow_framebuffers(), TAG, "runtime framebuffer unavailable");
    ESP_RETURN_ON_ERROR(ensure_stage_framebuffer(), TAG, "stage framebuffer unavailable");

    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    bool already_ready = base_photo_matches_locked(source_path, fit_mode, rotation_deg);
    xSemaphoreGive(s_display_mutex);
    if (already_ready) {
        return ESP_OK;
    }

    int64_t started_ms = clock_service_now_ms();
    uint32_t payload_size = 0;
    int64_t read_time_ms = 0;
    int64_t decode_time_ms = 0;
    esp_err_t err = load_cached_photo_into_buffer(source_path,
                                                  fit_mode,
                                                  rotation_deg,
                                                  s_stage_buffer,
                                                  &payload_size,
                                                  &read_time_ms,
                                                  &decode_time_ms);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    color_pixel_rgb565_data_t *old_base = s_base_buffer;
    s_base_buffer = s_stage_buffer;
    s_stage_buffer = NULL;
    strlcpy(s_base_photo_path, source_path, sizeof(s_base_photo_path));
    s_base_photo_rotation_deg = rotation_deg;
    s_base_photo_fit_mode = fit_mode;
    ++s_buffer_generation;
    retire_buffer_locked(old_base);
    xSemaphoreGive(s_display_mutex);

    ESP_LOGI(TAG,
             "staged JPEG display cache for %s in %lldms (read=%lldms decode=%lldms payload=%" PRIu32 " bytes, %.1f%% raw)",
             source_path,
             (long long)(clock_service_now_ms() - started_ms),
             (long long)read_time_ms,
             (long long)decode_time_ms,
             payload_size,
             (double)payload_size * 100.0 / (double)s_framebuffer_bytes);
    return ESP_OK;
}

bool display_service_should_defer_photo(const ephoto_photo_t *photo, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!photo || !photo->path[0]) {
        return false;
    }
    return should_defer_foreground_render(photo->path,
                                          photo->size_bytes,
                                          photo->width,
                                          photo->height,
                                          fit_mode,
                                          rotation_deg);
}

esp_err_t display_service_prepare_photo_cache(const char *source_path, ephoto_fit_mode_t fit_mode, uint16_t rotation_deg)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!s_initialized || !source_path || !source_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display_service_has_cache(source_path, fit_mode, rotation_deg)) {
        return ESP_OK;
    }

    clear_cache_failure_marker(source_path, rotation_deg, fit_mode);

    image_type_t sniffed_type = sniff_image_type_from_file(source_path);
    image_type_t image_type = sniffed_type != IMAGE_TYPE_UNKNOWN ? sniffed_type : detect_image_type(source_path);
    bool long_decode_wdt_guard = begin_long_decode_wdt_guard();
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    if (image_type == IMAGE_TYPE_JPEG) {
        jpeg_software_header_info_t header_info = {0};
        probe_jpeg_software_header(source_path, &header_info);
        err = generate_jpeg_display_cache(source_path, &header_info, fit_mode, rotation_deg);
        end_long_decode_wdt_guard(long_decode_wdt_guard);
        if (err != ESP_OK) {
            save_cache_failure_marker(source_path, rotation_deg, fit_mode, err);
        }
        return err;
    }

    if (image_type == IMAGE_TYPE_PNG || image_type == IMAGE_TYPE_BMP) {
        color_pixel_rgb565_data_t *cache_buffer = heap_caps_malloc(s_framebuffer_bytes,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cache_buffer) {
            return ESP_ERR_NO_MEM;
        }

        err = (image_type == IMAGE_TYPE_PNG)
                  ? render_png_file_sw_to_buffer(source_path, fit_mode, rotation_deg, cache_buffer)
                  : render_bmp_file_sw_to_buffer(source_path, fit_mode, rotation_deg, cache_buffer);
        if (err == ESP_OK) {
            err = save_cached_buffer(source_path, rotation_deg, fit_mode, cache_buffer);
        }
        free(cache_buffer);
        end_long_decode_wdt_guard(long_decode_wdt_guard);
        if (err != ESP_OK) {
            save_cache_failure_marker(source_path, rotation_deg, fit_mode, err);
        }
        return err;
    }

    end_long_decode_wdt_guard(long_decode_wdt_guard);
    save_cache_failure_marker(source_path, rotation_deg, fit_mode, err);
    return err;
}

esp_err_t display_service_prepare_web_thumbnail(const char *source_path, ephoto_fit_mode_t fit_mode)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!s_initialized || !source_path || !source_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (display_service_has_web_thumbnail(source_path, fit_mode)) {
        return ESP_OK;
    }

    clear_web_thumbnail_failure_marker(source_path, fit_mode);

    esp_err_t err = ensure_matching_screen_cache_ready(source_path, fit_mode);
    if (err != ESP_OK) {
        save_web_thumbnail_failure_marker(source_path, fit_mode, err);
        return err;
    }

    color_pixel_rgb565_data_t *screen_buffer =
        heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!screen_buffer) {
        save_web_thumbnail_failure_marker(source_path, fit_mode, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    err = load_cached_photo_into_buffer(source_path, fit_mode, 0, screen_buffer, NULL, NULL, NULL);
    if (err != ESP_OK) {
        free(screen_buffer);
        save_web_thumbnail_failure_marker(source_path, fit_mode, err);
        return err;
    }
    err = build_web_thumbnail_from_screen_buffer(source_path, fit_mode, screen_buffer);
    free(screen_buffer);
    if (err != ESP_OK) {
        save_web_thumbnail_failure_marker(source_path, fit_mode, err);
    }
    return err;
}

esp_err_t display_service_prepare_missing_photo_caches(const char *source_path,
                                                       uint32_t *out_completed,
                                                       uint32_t *out_failed,
                                                       display_service_cache_progress_cb_t progress_cb,
                                                       void *progress_ctx)
{
    /* One full cache pass now covers both display-side caches and web-side caches.
     * Screen: contain/cover x rotation 0/90
     * Web: contain preview + cover thumbnail
     */
    static const ephoto_fit_mode_t all_fit_modes[] = {EPHOTO_FIT_CONTAIN, EPHOTO_FIT_COVER};
    static const uint16_t all_rotations[] = {0, 90};

    if (out_completed) {
        *out_completed = 0;
    }
    if (out_failed) {
        *out_failed = 0;
    }

    if (!EPHOTO_DISPLAY_CACHE_ENABLED) {
        return ESP_OK;
    }
    if (!s_initialized || !source_path || !source_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    image_type_t sniffed_type = sniff_image_type_from_file(source_path);
    image_type_t image_type = sniffed_type != IMAGE_TYPE_UNKNOWN ? sniffed_type : detect_image_type(source_path);
    bool long_decode_wdt_guard = begin_long_decode_wdt_guard();
    esp_err_t first_err = ESP_OK;
    bool handled_screen_caches = false;

    if (image_type == IMAGE_TYPE_JPEG) {
        jpeg_software_header_info_t header_info = {0};
        probe_jpeg_software_header(source_path, &header_info);
        ephoto_fit_mode_t pending_fit_modes[4];
        uint16_t pending_rotations[4];
        size_t pending_count = 0;

        for (size_t rot_i = 0; rot_i < sizeof(all_rotations) / sizeof(all_rotations[0]); ++rot_i) {
            uint16_t rotation_deg = all_rotations[rot_i];
            for (size_t fit_i = 0; fit_i < sizeof(all_fit_modes) / sizeof(all_fit_modes[0]); ++fit_i) {
                ephoto_fit_mode_t fit_mode = all_fit_modes[fit_i];
                if (display_service_has_cache(source_path, fit_mode, rotation_deg) ||
                    display_service_has_known_cache_failure(source_path, fit_mode, rotation_deg)) {
                    continue;
                }
                pending_fit_modes[pending_count] = fit_mode;
                pending_rotations[pending_count] = rotation_deg;
                pending_count += 1;
            }
        }

        if (pending_count > 0) {
            esp_err_t err = ESP_OK;
            for (size_t i = 0; i < pending_count; ++i) {
                clear_cache_failure_marker(source_path, pending_rotations[i], pending_fit_modes[i]);
                err = generate_jpeg_display_cache(source_path,
                                                  &header_info,
                                                  pending_fit_modes[i],
                                                  pending_rotations[i]);
                if (out_completed) {
                    *out_completed += 1U;
                }
                if (err != ESP_OK) {
                    save_cache_failure_marker(source_path, pending_rotations[i], pending_fit_modes[i], err);
                    if (out_failed) {
                        *out_failed += 1U;
                    }
                }
                if (progress_cb) {
                    progress_cb(progress_ctx,
                                out_completed ? *out_completed : 0U,
                                out_failed ? *out_failed : (err != ESP_OK ? 1U : 0U));
                }
            }

            if (err != ESP_OK && first_err == ESP_OK) {
                first_err = err;
            }
        }
        handled_screen_caches = true;
    }

    if (!handled_screen_caches) {
        for (size_t rot_i = 0; rot_i < sizeof(all_rotations) / sizeof(all_rotations[0]); ++rot_i) {
            for (size_t fit_i = 0; fit_i < sizeof(all_fit_modes) / sizeof(all_fit_modes[0]); ++fit_i) {
                uint16_t rotation_deg = all_rotations[rot_i];
                ephoto_fit_mode_t fit_mode = all_fit_modes[fit_i];
                if (display_service_has_cache(source_path, fit_mode, rotation_deg) ||
                    display_service_has_known_cache_failure(source_path, fit_mode, rotation_deg)) {
                    continue;
                }

                clear_cache_failure_marker(source_path, rotation_deg, fit_mode);
                esp_err_t err = display_service_prepare_photo_cache(source_path, fit_mode, rotation_deg);
                if (out_completed) {
                    *out_completed += 1U;
                }
                if (err != ESP_OK) {
                    if (out_failed) {
                        *out_failed += 1U;
                    }
                    if (first_err == ESP_OK) {
                        first_err = err;
                    }
                }
                if (progress_cb) {
                    progress_cb(progress_ctx,
                                out_completed ? *out_completed : 0U,
                                out_failed ? *out_failed : (err != ESP_OK ? 1U : 0U));
                }
            }
        }
    }

    for (size_t fit_i = 0; fit_i < sizeof(all_fit_modes) / sizeof(all_fit_modes[0]); ++fit_i) {
        ephoto_fit_mode_t fit_mode = all_fit_modes[fit_i];
        if (display_service_has_web_thumbnail(source_path, fit_mode)) {
            continue;
        }
        if (display_service_has_known_web_thumbnail_failure(source_path, fit_mode)) {
            continue;
        }
        esp_err_t web_err = display_service_prepare_web_thumbnail(source_path, fit_mode);
        if (out_completed) {
            *out_completed += 1U;
        }
        if (web_err != ESP_OK && out_failed) {
            *out_failed += 1U;
        }
        if (web_err != ESP_OK && first_err == ESP_OK) {
            first_err = web_err;
        }
        if (progress_cb) {
            progress_cb(progress_ctx,
                        out_completed ? *out_completed : 0U,
                        out_failed ? *out_failed : (web_err != ESP_OK ? 1U : 0U));
        }
    }

    end_long_decode_wdt_guard(long_decode_wdt_guard);
    return first_err;
}

esp_err_t display_service_invalidate_photo_cache(const char *source_path)
{
    if (!EPHOTO_DISPLAY_CACHE_ENABLED || !source_path || !source_path[0] || !s_profile) {
        return ESP_ERR_INVALID_ARG;
    }

    ephoto_fit_mode_t fit_modes[] = {EPHOTO_FIT_CONTAIN, EPHOTO_FIT_COVER};
    uint16_t rotations[] = {0, 90};
    esp_err_t result = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++i) {
        for (size_t j = 0; j < sizeof(rotations) / sizeof(rotations[0]); ++j) {
            char cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
            if (build_cache_path_with_ext(source_path,
                                          rotations[j],
                                          fit_modes[i],
                                          "epf",
                                          cache_path,
                                          sizeof(cache_path)) != ESP_OK) {
                continue;
            }
            if (unlink(cache_path) == 0) {
                result = ESP_OK;
            }
            clear_cache_failure_marker(source_path, rotations[j], fit_modes[i]);
        }
    }

    for (size_t i = 0; i < sizeof(fit_modes) / sizeof(fit_modes[0]); ++i) {
        char web_cache_path[EPHOTO_MAX_PHOTO_PATH_LEN];
        if (build_web_thumbnail_cache_path(source_path, fit_modes[i], web_cache_path, sizeof(web_cache_path)) == ESP_OK) {
            if (unlink(web_cache_path) == 0) {
                result = ESP_OK;
            }
        }
        clear_web_thumbnail_failure_marker(source_path, fit_modes[i]);
    }

    if (s_display_mutex) {
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        if (strcmp(s_base_photo_path, source_path) == 0) {
            reset_photo_cache();
        }
        xSemaphoreGive(s_display_mutex);
    }

    return result;
}

esp_err_t display_service_purge_all_caches(void)
{
    if (!s_profile) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *mount_path = storage_service_get_mount_path();
    if (!mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    char cache_dir[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (snprintf(cache_dir, sizeof(cache_dir), "%s/.ephoto_cache", mount_path) <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    DIR *dir = opendir(cache_dir);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry = NULL;
    int removed = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char file_path[EPHOTO_MAX_PHOTO_PATH_LEN];
        if (snprintf(file_path, sizeof(file_path), "%s/%s", cache_dir, entry->d_name) <= 0) {
            continue;
        }
        if (unlink(file_path) == 0) {
            ++removed;
        }
    }
    closedir(dir);

    if (s_display_mutex) {
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        reset_photo_cache();
        xSemaphoreGive(s_display_mutex);
    }

    ESP_LOGI(TAG, "purged %d display cache files", removed);
    return ESP_OK;
}

esp_err_t display_service_init(const board_profile_t *profile)
{
    ephoto_settings_t startup_settings = {0};
    bool have_startup_settings = false;
    s_profile = profile;
    reset_photo_cache();
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_display_mutex, ESP_ERR_NO_MEM, TAG, "display mutex alloc failed");
    }

    ESP_RETURN_ON_ERROR(enable_dsi_phy_power(profile), TAG, "enable DSI PHY power failed");
    vTaskDelay(pdMS_TO_TICKS(30));

    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    bus_config.bus_id = profile->dsi_bus_id;
    bus_config.num_data_lanes = profile->dsi_num_data_lanes;
    bus_config.lane_bit_rate_mbps = profile->dsi_lane_bit_rate_mbps;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus), TAG, "create DSI bus failed");

    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_panel_io), TAG, "create DSI DBI IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.video_timing.h_size = profile->lcd_h_res;
    dpi_config.video_timing.v_size = profile->lcd_v_res;
    jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = profile->dsi_num_data_lanes,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = profile->display_reset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(s_panel_io, &panel_config, &s_panel), TAG, "create JD9365 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "panel display off failed");

    if (settings_service_load(&startup_settings) == ESP_OK) {
        have_startup_settings = true;
    }
    uint16_t startup_rotation_deg = resolve_startup_rotation_deg(profile,
                                                                 &startup_settings,
                                                                 have_startup_settings);
    ephoto_brightness_t startup_brightness = have_startup_settings ? startup_settings.brightness
                                                                   : profile->default_brightness;

    s_framebuffer_pixels = (size_t)profile->lcd_h_res * profile->lcd_v_res;
    s_framebuffer_bytes = s_framebuffer_pixels * sizeof(color_pixel_rgb565_data_t);
    s_base_buffer = heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_base_buffer, ESP_ERR_NO_MEM, TAG, "allocate base framebuffer failed");
    memset(s_base_buffer, 0, s_framebuffer_bytes);

    s_stage_buffer = heap_caps_malloc(s_framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_stage_buffer, ESP_ERR_NO_MEM, TAG, "allocate stage framebuffer failed");
    memset(s_stage_buffer, 0, s_framebuffer_bytes);

    s_file_io_buffer = heap_caps_malloc(64U * 1024U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file_io_buffer) {
        ESP_LOGW(TAG, "file io buffer allocation failed, using stdio default buffering");
    }

    s_frame_buffer = s_base_buffer;
    s_display_on = false;
    s_brightness = startup_brightness;
    s_backlight_pwm_ready = false;
    s_applied_screen_on_valid = false;
    s_applied_screen_on = false;
    s_buffer_generation = 1;
    s_last_rendered_generation = 1;
    apply_rotation(startup_rotation_deg);
    memset(s_base_buffer, 0, s_framebuffer_bytes);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(display_lvgl_backend_init(profile,
                                                  s_panel_io,
                                                  s_panel,
                                                  startup_rotation_deg),
                        TAG,
                        "lvgl backend init failed");
    display_lvgl_backend_set_software_brightness_enabled(false);
    ESP_RETURN_ON_ERROR(init_backlight_pwm(profile), TAG, "hardware backlight pwm init failed");
    if (profile->backlight != GPIO_NUM_NC) {
        apply_backlight_level(profile->default_brightness, false);
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel display on failed");
    s_display_on = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (profile->backlight != GPIO_NUM_NC) {
        apply_backlight_level(startup_brightness, true);
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "display service ready for %ux%u panel (lvgl backend)",
             profile->lcd_h_res,
             profile->lcd_v_res);
    return ESP_OK;
}

void display_service_apply_settings(const ephoto_settings_t *settings)
{
    if (!s_profile || !settings) {
        return;
    }
    if (s_rotation_deg != settings->rotation_deg) {
        apply_rotation(settings->rotation_deg);
    }
    bool screen_state_changed = !s_applied_screen_on_valid || s_applied_screen_on != settings->screen_on;
    if (screen_state_changed && !settings->screen_on) {
        apply_backlight_level(settings->brightness, false);
    }
    if (screen_state_changed) {
        apply_display_power(settings->screen_on);
        s_applied_screen_on = settings->screen_on;
        s_applied_screen_on_valid = true;
    }
    if (screen_state_changed || s_brightness != settings->brightness) {
        apply_backlight_level(settings->brightness, settings->screen_on);
    }
    s_brightness = settings->brightness;
    display_lvgl_backend_apply_settings(settings);
}

void display_service_set_startup_scan_progress(size_t completed, size_t total)
{
    if (!s_initialized) {
        return;
    }
    display_lvgl_backend_set_startup_scan_progress(completed, total);
}

bool display_service_is_busy(void)
{
    return s_render_busy;
}

void display_service_release_album_resources(void)
{
    if (!s_initialized) {
        return;
    }
    display_lvgl_backend_release_album_resources();
}

bool display_service_has_pending_album_thumbnail_work(void)
{
    if (!s_initialized) {
        return false;
    }
    return display_lvgl_backend_has_pending_album_thumbnail_work();
}

void display_service_mark_frame_rendered(void)
{
    if (!s_display_mutex) {
        return;
    }

    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    s_last_rendered_generation = s_buffer_generation;
    xSemaphoreGive(s_display_mutex);
}

void display_service_render_state(const ephoto_app_state_t *state)
{
    if (!s_initialized || !state) {
        return;
    }

    bool photo_ready_now = false;
    const void *photo_pixels = NULL;
    ephoto_settings_t effective_settings = state->settings;
    if (state->settings.screen_on && should_render_photo(state)) {
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        photo_ready_now = base_photo_matches_locked(state->current_photo_path,
                                                    state->settings.fit_mode,
                                                    state->settings.rotation_deg);
        if (photo_ready_now) {
            photo_pixels = (const void *)s_base_buffer;
        }
        xSemaphoreGive(s_display_mutex);
    }
    bool has_current_photo = state->current_photo_path[0] != '\0';
    bool photo_loading = should_render_photo(state) && !photo_ready_now;
    bool has_visible_photo_base = photo_ready_now || s_base_photo_path[0] != '\0';
    char clock_text[16] = {0};
    if (!photo_loading &&
        !state->cache_build_active &&
        state->settings.clock_visible &&
        state->settings.screen_on &&
        (has_visible_photo_base || !has_current_photo)) {
        (void)get_clock_compact(clock_text, sizeof(clock_text));
    } else if (photo_loading &&
               !state->cache_build_active &&
               state->settings.clock_visible &&
               state->settings.screen_on &&
               (has_visible_photo_base || !has_current_photo)) {
        (void)get_clock_compact(clock_text, sizeof(clock_text));
    }
    display_service_apply_settings(&effective_settings);
    s_render_busy = photo_loading;
    display_lvgl_backend_render_state(state,
                                      &effective_settings,
                                      photo_pixels,
                                      photo_ready_now,
                                      photo_loading,
                                      clock_text);
}
