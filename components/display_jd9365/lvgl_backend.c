#include "display_lvgl_backend.h"

#include <stdio.h>
#include <string.h>

#include "clock_service.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "gallery_service.h"
#include "lvgl.h"
#include "wifi_admin.h"

static const char *TAG = "display_lvgl";
LV_FONT_DECLARE(ephoto_ui_font_18);
LV_FONT_DECLARE(ephoto_ui_font_27);
LV_FONT_DECLARE(ephoto_ui_font_36);
LV_FONT_DECLARE(ephoto_clock_num_81);


#define EPHOTO_MENU_STATUS_COUNT 8
#define EPHOTO_BOOT_QR_SIZE 128
static const board_profile_t *s_profile;
static lv_display_t *s_display;
static esp_lcd_panel_handle_t s_panel;

static lv_obj_t *s_photo;
static lv_obj_t *s_dim_overlay;
static lv_obj_t *s_clock_card;
static lv_obj_t *s_clock_primary;
static lv_obj_t *s_clock_meridiem;
static lv_obj_t *s_clock_date;
static lv_obj_t *s_clock_weekday;
static lv_obj_t *s_notification_card;
static lv_obj_t *s_notification_label;
static lv_obj_t *s_boot_overlay;
static lv_obj_t *s_boot_image;
static lv_obj_t *s_boot_card;
static lv_obj_t *s_boot_brand;
static lv_obj_t *s_boot_title;
static lv_obj_t *s_boot_hint;
static lv_obj_t *s_boot_line1;
static lv_obj_t *s_boot_line2;
static lv_obj_t *s_boot_bar;
static lv_obj_t *s_boot_qr;
static size_t s_boot_scan_last_total;
static int s_boot_scan_last_percent = -1;
static lv_obj_t *s_cache_overlay;
static lv_obj_t *s_cache_card;
static lv_obj_t *s_cache_title;
static lv_obj_t *s_cache_line1;
static lv_obj_t *s_cache_line2;
static lv_obj_t *s_cache_bar;
static lv_obj_t *s_ota_overlay;
static lv_obj_t *s_ota_card;
static lv_obj_t *s_ota_title;
static lv_obj_t *s_ota_line1;
static lv_obj_t *s_ota_line2;
static lv_obj_t *s_ota_bar;
static lv_obj_t *s_menu_panel;
static lv_obj_t *s_menu_title;
static lv_obj_t *s_menu_hint;
static lv_obj_t *s_menu_status[EPHOTO_MENU_STATUS_COUNT];
static lv_obj_t *s_menu_rows[EPHOTO_MENU_MAX_ITEMS];
static lv_obj_t *s_menu_row_keys[EPHOTO_MENU_MAX_ITEMS];
static lv_obj_t *s_menu_row_vals[EPHOTO_MENU_MAX_ITEMS];
static lv_obj_t *s_menu_dropdown;
static lv_obj_t *s_menu_dropdown_title;
static lv_obj_t *s_menu_dropdown_rows[EPHOTO_MENU_MAX_ITEMS];
static lv_obj_t *s_menu_dropdown_labels[EPHOTO_MENU_MAX_ITEMS];
static lv_obj_t *s_album_panel;
static lv_obj_t *s_album_title;
static lv_obj_t *s_album_summary;
static lv_obj_t *s_album_hint;
static lv_obj_t *s_album_tiles[EPHOTO_ALBUM_MAX_SLOTS];
static lv_obj_t *s_album_tile_images[EPHOTO_ALBUM_MAX_SLOTS];
static lv_obj_t *s_album_tile_badges[EPHOTO_ALBUM_MAX_SLOTS];
static lv_obj_t *s_album_tile_meta[EPHOTO_ALBUM_MAX_SLOTS];
static lv_obj_t *s_album_tile_current[EPHOTO_ALBUM_MAX_SLOTS];
static lv_obj_t *s_album_action_popup;
static lv_obj_t *s_album_action_title;
static lv_obj_t *s_album_action_rows[EPHOTO_ALBUM_MAX_ACTIONS];
static lv_obj_t *s_album_action_labels[EPHOTO_ALBUM_MAX_ACTIONS];

static lv_image_dsc_t s_photo_dsc;
static lv_image_dsc_t s_boot_image_dsc;
static lv_image_dsc_t s_boot_qr_dsc;
static lv_image_dsc_t s_album_tile_dsc[EPHOTO_ALBUM_MAX_SLOTS];
static char s_last_photo_path[EPHOTO_MAX_PHOTO_PATH_LEN];
static uint16_t s_last_photo_rotation;
static ephoto_fit_mode_t s_last_photo_fit_mode = (ephoto_fit_mode_t)-1;

static lv_style_t s_style_screen;
static lv_style_t s_style_glass;
static lv_style_t s_style_panel;
static lv_style_t s_style_selected;
static lv_style_t s_style_editing;
static lv_style_t s_style_clock_text;
static lv_style_t s_style_text;
static lv_style_t s_style_muted;
static lv_style_t s_style_menu_text;
static lv_style_t s_style_menu_muted;
static lv_style_t s_style_menu_status;
static bool s_styles_ready;
static lv_display_rotation_t s_display_rotation = LV_DISPLAY_ROTATION_0;
static uint16_t s_last_layout_rotation = UINT16_MAX;
static ephoto_brightness_t s_last_overlay_brightness = (ephoto_brightness_t)-1;
static bool s_last_overlay_screen_on;
static bool s_last_overlay_screen_on_valid;
static bool s_last_menu_photo_dim_valid;
static bool s_last_menu_photo_dimmed;
static ephoto_clock_format_t s_last_clock_format = (ephoto_clock_format_t)-1;
static ephoto_clock_position_t s_last_clock_position = (ephoto_clock_position_t)-1;
static ephoto_clock_color_t s_last_clock_color = (ephoto_clock_color_t)-1;
static uint16_t s_startup_rotation_deg;
static const void *s_boot_image_pixels;
static void *s_album_tile_pixels[EPHOTO_ALBUM_MAX_SLOTS];
static int32_t s_album_tile_photo_indices[EPHOTO_ALBUM_MAX_SLOTS];
static bool s_album_pending_thumbnail_work;
// The boot overlay is only for the first usable photo, never for slideshow transitions.
static bool s_boot_initial_photo_pending;

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void load_boot_image_asset(const board_profile_t *profile)
{
    (void)profile;
    if (s_boot_image_pixels) {
        return;
    }

    const void *pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t err = display_service_get_boot_image_rgb565(&pixels, &width, &height);
    if (err != ESP_OK || !pixels || width == 0 || height == 0) {
        ESP_LOGW(TAG, "boot image load failed: %s", esp_err_to_name(err));
        return;
    }

    memset(&s_boot_image_dsc, 0, sizeof(s_boot_image_dsc));
    s_boot_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_boot_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_boot_image_dsc.header.flags = 0;
    s_boot_image_dsc.header.w = width;
    s_boot_image_dsc.header.h = height;
    s_boot_image_dsc.header.stride = (uint32_t)width * 2U;
    s_boot_image_dsc.data_size = (uint32_t)width * height * 2U;
    s_boot_image_dsc.data = pixels;
    s_boot_image_pixels = pixels;
}

static void init_boot_qr_asset(void)
{
    // The public source distribution intentionally omits the private mini-app QR asset.
    memset(&s_boot_qr_dsc, 0, sizeof(s_boot_qr_dsc));
}

static void set_row_style(lv_obj_t *row, bool selected, bool editing)
{
    if (!row) {
        return;
    }

    if (selected && editing) {
        lv_obj_set_style_bg_color(row, lv_color_hex(0x337EB7), 0);
        lv_obj_set_style_bg_opa(row, (lv_opa_t)236, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
    } else if (selected) {
        lv_obj_set_style_bg_color(row, lv_color_hex(0x5B9FD2), 0);
        lv_obj_set_style_bg_opa(row, (lv_opa_t)220, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_TRANSP, 0);
    }
}

static void set_action_popup_row_style(lv_obj_t *row, lv_obj_t *label, bool selected, bool destructive)
{
    if (!row) {
        return;
    }

    if (selected) {
        lv_obj_set_style_bg_color(row, destructive ? lv_color_hex(0xC85A6A) : lv_color_hex(0x4A97D1), 0);
        lv_obj_set_style_bg_opa(row, (lv_opa_t)244, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_70, 0);
        lv_obj_set_style_shadow_color(row,
                                      destructive ? lv_color_hex(0xE4A3AD) : lv_color_hex(0x7DB9E5),
                                      0);
        lv_obj_set_style_shadow_opa(row, (lv_opa_t)90, 0);
        lv_obj_set_style_shadow_width(row, 14, 0);
        lv_obj_set_style_shadow_spread(row, 0, 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        lv_obj_set_style_bg_color(row, lv_color_hex(0xF5FAFF), 0);
        lv_obj_set_style_bg_opa(row, (lv_opa_t)180, 0);
        lv_obj_set_style_border_color(row,
                                      destructive ? lv_color_hex(0xE6B7BE) : lv_color_hex(0xBCDDF3),
                                      0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
        lv_obj_set_style_shadow_opa(row, LV_OPA_TRANSP, 0);
        if (label) {
            lv_obj_set_style_text_color(label,
                                        destructive ? lv_color_hex(0xB24B5A) : lv_color_hex(0x13324B),
                                        0);
        }
    }
}

static const char *network_mode_label_cn(ephoto_network_mode_t mode)
{
    switch (mode) {
    case EPHOTO_NETWORK_STA:
        return "自动";
    case EPHOTO_NETWORK_AP:
        return "热点";
    case EPHOTO_NETWORK_DISCONNECTED:
        return "离线";
    case EPHOTO_NETWORK_UNAVAILABLE:
    default:
        return "启动中";
    }
}

static const char *weekday_cn(int weekday)
{
    static const char *days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (weekday < 0 || weekday > 6) {
        return "--";
    }
    return days[weekday];
}

static const char *clock_format_label_cn(ephoto_clock_format_t format)
{
    return format == EPHOTO_CLOCK_FORMAT_12H ? "12小时" : "24小时";
}

static const char *firmware_version_string(void)
{
#ifdef EPHOTO_PROJECT_VER
    return EPHOTO_PROJECT_VER;
#else
    return "unknown";
#endif
}

static void style_init_once(void)
{
    if (s_styles_ready) {
        return;
    }

    lv_style_init(&s_style_screen);
    lv_style_set_bg_color(&s_style_screen, lv_color_hex(0x0B1522));
    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_screen, 0);
    lv_style_set_pad_all(&s_style_screen, 0);
    lv_style_set_radius(&s_style_screen, 0);

    lv_style_init(&s_style_glass);
    lv_style_set_bg_color(&s_style_glass, lv_color_hex(0xD8ECFA));
    lv_style_set_bg_opa(&s_style_glass, (lv_opa_t)148);
    lv_style_set_border_color(&s_style_glass, lv_color_hex(0xFFFFFF));
    lv_style_set_border_opa(&s_style_glass, LV_OPA_30);
    lv_style_set_border_width(&s_style_glass, 1);
    lv_style_set_radius(&s_style_glass, 20);
    lv_style_set_shadow_color(&s_style_glass, lv_color_hex(0x6FA8D6));
    lv_style_set_shadow_opa(&s_style_glass, LV_OPA_10);
    lv_style_set_shadow_width(&s_style_glass, 12);
    lv_style_set_shadow_spread(&s_style_glass, 0);
    lv_style_set_pad_all(&s_style_glass, 12);

    lv_style_init(&s_style_panel);
    lv_style_set_bg_color(&s_style_panel, lv_color_hex(0xF5FAFF));
    lv_style_set_bg_opa(&s_style_panel, (lv_opa_t)217);
    lv_style_set_border_width(&s_style_panel, 0);
    lv_style_set_radius(&s_style_panel, 16);
    lv_style_set_pad_all(&s_style_panel, 10);

    lv_style_init(&s_style_selected);
    lv_style_set_bg_color(&s_style_selected, lv_color_hex(0x9ED4F5));
    lv_style_set_bg_opa(&s_style_selected, (lv_opa_t)217);
    lv_style_set_radius(&s_style_selected, 14);
    lv_style_set_border_width(&s_style_selected, 0);

    lv_style_init(&s_style_editing);
    lv_style_set_bg_color(&s_style_editing, lv_color_hex(0x6EB8E8));
    lv_style_set_bg_opa(&s_style_editing, (lv_opa_t)242);
    lv_style_set_radius(&s_style_editing, 14);
    lv_style_set_border_color(&s_style_editing, lv_color_hex(0xFFFFFF));
    lv_style_set_border_width(&s_style_editing, 1);
    lv_style_set_border_opa(&s_style_editing, LV_OPA_60);

    lv_style_init(&s_style_clock_text);
    lv_style_set_text_color(&s_style_clock_text, lv_color_hex(0x13324B));

    lv_style_init(&s_style_text);
    lv_style_set_text_color(&s_style_text, lv_color_hex(0x10283D));
    lv_style_set_text_font(&s_style_text, &ephoto_ui_font_18);

    lv_style_init(&s_style_muted);
    lv_style_set_text_color(&s_style_muted, lv_color_hex(0x425D76));
    lv_style_set_text_font(&s_style_muted, &ephoto_ui_font_18);

    lv_style_init(&s_style_menu_text);
    lv_style_set_text_color(&s_style_menu_text, lv_color_hex(0x10283D));
    lv_style_set_text_font(&s_style_menu_text, &ephoto_ui_font_18);

    lv_style_init(&s_style_menu_muted);
    lv_style_set_text_color(&s_style_menu_muted, lv_color_hex(0x425D76));
    lv_style_set_text_font(&s_style_menu_muted, &ephoto_ui_font_18);

    lv_style_init(&s_style_menu_status);
    lv_style_set_text_color(&s_style_menu_status, lv_color_hex(0x29455E));
    lv_style_set_text_font(&s_style_menu_status, &ephoto_ui_font_18);

    s_styles_ready = true;
}

static void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void release_album_tile_image(size_t index)
{
    if (index >= EPHOTO_ALBUM_MAX_SLOTS) {
        return;
    }
    free(s_album_tile_pixels[index]);
    s_album_tile_pixels[index] = NULL;
    memset(&s_album_tile_dsc[index], 0, sizeof(s_album_tile_dsc[index]));
    s_album_tile_photo_indices[index] = -1;
    if (s_album_tile_images[index]) {
        lv_image_set_src(s_album_tile_images[index], NULL);
        lv_obj_invalidate(s_album_tile_images[index]);
    }
    if (s_album_tiles[index]) {
        lv_obj_invalidate(s_album_tiles[index]);
    }
}

static void release_all_album_tile_images(void)
{
    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        release_album_tile_image(i);
    }
    s_album_pending_thumbnail_work = false;
}

static bool resolve_album_photo_path(int32_t photo_index, char *out_path, size_t out_path_len)
{
    if (!out_path || out_path_len == 0 || photo_index < 0) {
        return false;
    }

    ephoto_photo_t photo = {0};
    if (gallery_service_get_item(photo_index, &photo) != ESP_OK || !photo.path[0]) {
        out_path[0] = '\0';
        return false;
    }

    strlcpy(out_path, photo.path, out_path_len);
    return true;
}

static void load_album_tile_image(size_t index, int32_t photo_index)
{
    if (index >= EPHOTO_ALBUM_MAX_SLOTS || photo_index < 0) {
        release_album_tile_image(index);
        return;
    }
    if (s_album_tile_photo_indices[index] == photo_index) {
        return;
    }

    release_album_tile_image(index);
    char path[EPHOTO_MAX_PHOTO_PATH_LEN];
    if (!resolve_album_photo_path(photo_index, path, sizeof(path))) {
        return;
    }

    void *pixels = NULL;
    uint16_t width = 0;
    uint16_t height = 0;
    lv_coord_t tile_w = s_album_tiles[index] ? lv_obj_get_width(s_album_tiles[index]) : 0;
    lv_coord_t tile_h = s_album_tiles[index] ? lv_obj_get_height(s_album_tiles[index]) : 0;
    uint16_t target_w = (tile_w > 20) ? (uint16_t)(tile_w - 20) : 96;
    uint16_t target_h = (tile_h > 56) ? (uint16_t)(tile_h - 56) : 96;
    if (target_w < 72) {
        target_w = 72;
    }
    if (target_h < 72) {
        target_h = 72;
    }
    if (display_service_load_web_thumbnail_rgb565(path,
                                                  EPHOTO_FIT_COVER,
                                                  target_w,
                                                  target_h,
                                                  &pixels,
                                                  &width,
                                                  &height) != ESP_OK ||
        !pixels || width == 0 || height == 0) {
        s_album_tile_photo_indices[index] = photo_index;
        return;
    }

    s_album_tile_pixels[index] = pixels;
    s_album_tile_dsc[index].header.magic = LV_IMAGE_HEADER_MAGIC;
    s_album_tile_dsc[index].header.cf = LV_COLOR_FORMAT_RGB565;
    s_album_tile_dsc[index].header.flags = 0;
    s_album_tile_dsc[index].header.w = width;
    s_album_tile_dsc[index].header.h = height;
    s_album_tile_dsc[index].header.stride = (uint32_t)width * 2U;
    s_album_tile_dsc[index].data_size = (uint32_t)width * height * 2U;
    s_album_tile_dsc[index].data = pixels;
    s_album_tile_photo_indices[index] = photo_index;
    lv_image_set_src(s_album_tile_images[index], &s_album_tile_dsc[index]);
    lv_obj_set_size(s_album_tile_images[index], width, height);
    lv_obj_align(s_album_tile_images[index], LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_invalidate(s_album_tile_images[index]);
    lv_obj_invalidate(s_album_tiles[index]);
}

static void apply_brightness(ephoto_brightness_t brightness, bool screen_on)
{
    (void)brightness;
    (void)screen_on;
    if (!s_dim_overlay) {
        return;
    }
    set_obj_hidden(s_dim_overlay, true);
}

static ephoto_clock_position_t normalize_clock_position_local(ephoto_clock_position_t position)
{
    switch (position) {
    case EPHOTO_CLOCK_POSITION_TOP_LEFT:
    case EPHOTO_CLOCK_POSITION_BOTTOM_LEFT:
    case EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT:
    case EPHOTO_CLOCK_POSITION_TOP_RIGHT:
        return position;
    default:
        return EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    }
}

static ephoto_clock_color_t normalize_clock_color_local(ephoto_clock_color_t color)
{
    return color == EPHOTO_CLOCK_COLOR_DARK_GRAY
               ? EPHOTO_CLOCK_COLOR_DARK_GRAY
               : EPHOTO_CLOCK_COLOR_WHITE;
}

static void apply_clock_colors(ephoto_clock_color_t color)
{
    lv_color_t primary = lv_color_hex(0xF7FBFF);
    lv_color_t secondary = lv_color_hex(0xDCEBFA);
    lv_color_t tertiary = lv_color_hex(0xCFE3F7);

    if (normalize_clock_color_local(color) == EPHOTO_CLOCK_COLOR_DARK_GRAY) {
        primary = lv_color_hex(0x495057);
        secondary = lv_color_hex(0x5C6670);
        tertiary = lv_color_hex(0x5C6670);
    }

    if (s_clock_primary) {
        lv_obj_set_style_text_color(s_clock_primary, primary, 0);
    }
    if (s_clock_meridiem) {
        lv_obj_set_style_text_color(s_clock_meridiem, secondary, 0);
    }
    if (s_clock_date) {
        lv_obj_set_style_text_color(s_clock_date, tertiary, 0);
    }
    if (s_clock_weekday) {
        lv_obj_set_style_text_color(s_clock_weekday, tertiary, 0);
    }
}

static void update_clock_geometry(uint16_t rotation_deg,
                                  ephoto_clock_format_t format,
                                  ephoto_clock_position_t position)
{
    if (!s_profile || !s_clock_card) {
        return;
    }

    bool landscape = rotation_deg == 90;
    lv_coord_t screen_w = s_display ? (lv_coord_t)lv_display_get_horizontal_resolution(s_display) : s_profile->lcd_h_res;
    lv_coord_t screen_h = s_display ? (lv_coord_t)lv_display_get_vertical_resolution(s_display) : s_profile->lcd_v_res;
    lv_coord_t aux_size = 37;
    lv_coord_t card_w = landscape ? 378 : 332;
    lv_coord_t card_h = landscape ? 112 : 118;
    lv_coord_t base_x = screen_w - card_w - aux_size;
    lv_coord_t base_y = aux_size;

    (void)format;
    position = normalize_clock_position_local(position);
    switch (position) {
    case EPHOTO_CLOCK_POSITION_TOP_LEFT:
        base_x = aux_size;
        base_y = aux_size;
        break;
    case EPHOTO_CLOCK_POSITION_BOTTOM_LEFT:
        base_x = aux_size;
        base_y = screen_h - card_h - aux_size;
        break;
    case EPHOTO_CLOCK_POSITION_BOTTOM_RIGHT:
        base_x = screen_w - card_w - aux_size;
        base_y = screen_h - card_h - aux_size;
        break;
    case EPHOTO_CLOCK_POSITION_TOP_RIGHT:
    default:
        base_x = screen_w - card_w - aux_size;
        base_y = aux_size;
        break;
    }

    lv_obj_set_pos(s_clock_card, base_x, base_y);
    lv_obj_set_size(s_clock_card, card_w, card_h);

    if (s_clock_primary) {
        lv_obj_set_size(s_clock_primary, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(s_clock_primary, card_w - 82, 0);
    }
    if (s_clock_meridiem) {
        lv_obj_set_size(s_clock_meridiem, 72, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(s_clock_meridiem, 72, 0);
    }
    if (s_clock_date) {
        lv_obj_set_size(s_clock_date, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(s_clock_date, card_w - 82, 0);
    }
    if (s_clock_weekday) {
        lv_obj_set_size(s_clock_weekday, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(s_clock_weekday, 78, 0);
    }
}

static void layout_clock_content(uint16_t rotation_deg, bool show_meridiem)
{
    if (!s_clock_card || !s_clock_primary || !s_clock_date || !s_clock_weekday) {
        return;
    }

    bool landscape = rotation_deg == 90;
    bool anchor_left = normalize_clock_position_local(s_last_clock_position) == EPHOTO_CLOCK_POSITION_TOP_LEFT ||
                       normalize_clock_position_local(s_last_clock_position) == EPHOTO_CLOCK_POSITION_BOTTOM_LEFT;
    lv_coord_t card_w = lv_obj_get_width(s_clock_card);
    lv_coord_t card_h = lv_obj_get_height(s_clock_card);
    lv_coord_t pad_right = 0;
    lv_coord_t pad_left = 0;
    lv_coord_t pad_top = 0;
    lv_coord_t gap_between = landscape ? 6 : 4;
    lv_coord_t date_gap = landscape ? 8 : 10;
    lv_coord_t meridiem_slot_w = 72;
    lv_coord_t weekday_gap = landscape ? 10 : 8;

    lv_obj_update_layout(s_clock_card);

    lv_coord_t primary_w = lv_obj_get_width(s_clock_primary);
    lv_coord_t primary_h = lv_obj_get_height(s_clock_primary);
    lv_coord_t second_row_y = pad_top + primary_h + date_gap;
    if (second_row_y < 0) {
        second_row_y = 0;
    }
    if (second_row_y > card_h) {
        second_row_y = card_h;
    }

    lv_obj_set_style_text_align(s_clock_primary, anchor_left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(s_clock_meridiem, anchor_left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(s_clock_date, anchor_left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(s_clock_weekday, anchor_left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);

    lv_coord_t primary_x = anchor_left
                               ? pad_left
                               : (card_w - meridiem_slot_w - gap_between - primary_w - pad_right);

    lv_obj_set_pos(s_clock_primary, primary_x, pad_top);

    if (s_clock_meridiem) {
        lv_coord_t meridiem_x = anchor_left
                                    ? (primary_x + primary_w + gap_between)
                                    : (card_w - meridiem_slot_w - pad_right);
        lv_obj_set_pos(s_clock_meridiem,
                       meridiem_x,
                       pad_top + (primary_h / 2) - (lv_obj_get_height(s_clock_meridiem) / 2) + 1);
        if (show_meridiem) {
            lv_obj_set_style_text_opa(s_clock_meridiem, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_opa(s_clock_meridiem, LV_OPA_TRANSP, 0);
        }
    }

    lv_coord_t weekday_w = lv_obj_get_width(s_clock_weekday);
    lv_coord_t date_w = lv_obj_get_width(s_clock_date);
    lv_coord_t date_x = 0;
    lv_coord_t weekday_x = 0;

    if (anchor_left) {
        date_x = pad_left;
        weekday_x = date_x + date_w + weekday_gap;
    } else {
        weekday_x = card_w - weekday_w - pad_right;
        date_x = weekday_x - weekday_gap - date_w;
    }

    lv_obj_set_pos(s_clock_date, date_x, second_row_y);
    lv_obj_set_pos(s_clock_weekday, weekday_x, second_row_y);
}

static void update_startup_overlay(const ephoto_app_state_t *state)
{
    bool show_cache_startup = false;
    bool no_photos = false;
    bool current_photo_loading = false;
    bool initial_photo_loading = false;
    bool show = false;

    if (state) {
        show_cache_startup = state->interaction_locked && state->cache_build_active;
        no_photos = state->storage.photo_count <= 0;
        if (no_photos) {
            // If the device starts empty, treat the first later upload as its first photo.
            s_boot_initial_photo_pending = true;
        }
        current_photo_loading = state->storage.photo_count > 0 && state->current_photo_path[0] &&
                                !display_service_is_photo_ready(state->current_photo_path,
                                                                state->settings.fit_mode,
                                                                state->settings.rotation_deg);
        if (state->storage.photo_count > 0 && state->current_photo_path[0] && !current_photo_loading) {
            s_boot_initial_photo_pending = false;
        }
        initial_photo_loading = s_boot_initial_photo_pending && current_photo_loading;
        show = no_photos || show_cache_startup || initial_photo_loading ||
               (s_boot_initial_photo_pending && state->storage.photo_count > 0 && !state->current_photo_path[0]);
    }

    set_obj_hidden(s_boot_overlay, !show);
    if (!show) {
        return;
    }

    uint32_t total = state->cache_build_total;
    uint32_t done = state->cache_build_done;
    uint32_t failed = state->cache_build_failed;
    uint32_t photo_total = state->cache_build_photo_total;
    uint32_t photo_index = state->cache_build_photo_index;
    uint32_t percent = (total > 0) ? ((done > total ? total : done) * 100U) / total : 6U;
    char line1[64];
    char line2[96];

    bool startup_mode = show_cache_startup && (total == 0 || total > 4);
    set_label_text_if_changed(s_boot_brand, "E-Photo 忆风电子相框");
    if (no_photos) {
        set_label_text_if_changed(s_boot_title, "设备存储内无任何照片");
        set_label_text_if_changed(s_boot_hint, "暂无照片，请上传");
        strlcpy(line1, "可使用右侧小程序码快速进入上传页面", sizeof(line1));
        strlcpy(line2, "上传完成后，设备会自动开始显示照片", sizeof(line2));
        percent = 12U;
    } else if (!show_cache_startup) {
        set_label_text_if_changed(s_boot_title, "正在启动你的电子相框");
        set_label_text_if_changed(s_boot_hint, "正在载入首张照片，请稍候片刻");
        strlcpy(line1, "正在生成当前显示所需缓存", sizeof(line1));
        strlcpy(line2, "首张照片准备完成后将立即显示", sizeof(line2));
        percent = 95U;
    } else {
        set_label_text_if_changed(s_boot_title, startup_mode ? "正在启动你的电子相框" : "正在准备当前照片");
        set_label_text_if_changed(s_boot_hint,
                                  startup_mode ? "首次启动会检查图片缓存，请稍候片刻"
                                               : "正在生成显示缓存，完成后会立即恢复播放");

        if (total > 0) {
            uint32_t display_photo_total = photo_total > 0 ? photo_total : total;
            uint32_t display_photo_index = photo_index > 0 ? photo_index : done;
            if (display_photo_index > display_photo_total) {
                display_photo_index = display_photo_total;
            }
            snprintf(line1,
                     sizeof(line1),
                     "第%lu张/共%lu张",
                     (unsigned long)display_photo_index,
                     (unsigned long)display_photo_total);
            if (failed > 0) {
                snprintf(line2,
                         sizeof(line2),
                         "缓存处理中 · 失败%lu",
                         (unsigned long)failed);
            } else {
                snprintf(line2, sizeof(line2), "缓存处理中 · %lu%%", (unsigned long)percent);
            }
        } else {
            strlcpy(line1, "正在扫描照片与缓存文件", sizeof(line1));
            strlcpy(line2, "请保持供电稳定，不要拔出 TF 卡", sizeof(line2));
        }
    }

    set_label_text_if_changed(s_boot_line1, line1);
    set_label_text_if_changed(s_boot_line2, line2);
    set_obj_hidden(s_boot_bar, !(show_cache_startup || initial_photo_loading));
    if (show_cache_startup || initial_photo_loading) {
        lv_bar_set_value(s_boot_bar, (int32_t)percent, LV_ANIM_OFF);
    }
}

static void apply_layout(uint16_t rotation_deg)
{
    if (!s_profile || !s_menu_panel || !s_cache_card) {
        return;
    }

    bool landscape = rotation_deg == 90;
    lv_coord_t screen_w = s_display ? (lv_coord_t)lv_display_get_horizontal_resolution(s_display) : s_profile->lcd_h_res;
    lv_coord_t screen_h = s_display ? (lv_coord_t)lv_display_get_vertical_resolution(s_display) : s_profile->lcd_v_res;

    lv_coord_t menu_w = landscape ? 760 : (screen_w - 24);
    lv_coord_t menu_h = landscape ? 424 : (screen_h - 24);
    if (menu_w > screen_w - (landscape ? 48 : 16)) {
        menu_w = screen_w - (landscape ? 48 : 16);
    }
    if (menu_h > screen_h - (landscape ? 56 : 16)) {
        menu_h = screen_h - (landscape ? 56 : 16);
    }

    lv_obj_set_style_transform_rotation(s_menu_panel, 0, 0);
    lv_obj_set_style_transform_rotation(s_clock_card, 0, 0);
    lv_obj_set_style_transform_rotation(s_notification_card, 0, 0);
    lv_obj_set_style_transform_rotation(s_cache_card, 0, 0);
    lv_obj_set_style_transform_rotation(s_ota_card, 0, 0);

    lv_obj_set_size(s_menu_panel, menu_w, menu_h);
    lv_obj_center(s_menu_panel);
    lv_obj_set_pos(s_menu_title, 18, 12);
    set_obj_hidden(s_menu_hint, true);

    lv_coord_t content_x = landscape ? 18 : 16;
    lv_coord_t content_w = menu_w - content_x * 2;
    lv_coord_t status_y0 = landscape ? 42 : 50;
    lv_coord_t row_y0 = landscape ? 124 : 164;
    lv_coord_t row_h = landscape ? 26 : 34;
    lv_coord_t row_gap = landscape ? 28 : 40;

    for (size_t i = 0; i < EPHOTO_MENU_STATUS_COUNT; ++i) {
        lv_label_set_long_mode(s_menu_status[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_menu_status[i], 22);
    }

    if (landscape) {
        lv_coord_t col_gap = 12;
        lv_coord_t col_w = (content_w - col_gap * 2) / 3;
        lv_coord_t row_gap_status = 28;
        lv_coord_t col0_x = content_x;
        lv_coord_t col1_x = content_x + col_w + col_gap;
        lv_coord_t col2_x = content_x + (col_w + col_gap) * 2;

        lv_obj_set_pos(s_menu_status[0], col0_x, status_y0);
        lv_obj_set_width(s_menu_status[0], col_w);
        lv_obj_set_style_text_align(s_menu_status[0], LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_set_pos(s_menu_status[1], col1_x, status_y0);
        lv_obj_set_width(s_menu_status[1], col_w);
        lv_obj_set_style_text_align(s_menu_status[1], LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_set_pos(s_menu_status[2], col2_x, status_y0);
        lv_obj_set_width(s_menu_status[2], col_w);
        lv_obj_set_style_text_align(s_menu_status[2], LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_set_pos(s_menu_status[3], col0_x, status_y0 + row_gap_status);
        lv_obj_set_width(s_menu_status[3], col_w);
        lv_obj_set_style_text_align(s_menu_status[3], LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_set_pos(s_menu_status[4], col1_x, status_y0 + row_gap_status);
        lv_obj_set_width(s_menu_status[4], col_w);
        lv_obj_set_style_text_align(s_menu_status[4], LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_set_pos(s_menu_status[5], col2_x, status_y0 + row_gap_status);
        lv_obj_set_width(s_menu_status[5], col_w);
        lv_obj_set_style_text_align(s_menu_status[5], LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_set_pos(s_menu_status[6], col0_x, status_y0 + row_gap_status * 2);
        lv_obj_set_width(s_menu_status[6], col_w * 2 + col_gap);
        lv_obj_set_style_text_align(s_menu_status[6], LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_set_pos(s_menu_status[7], col2_x, status_y0 + row_gap_status * 2);
        lv_obj_set_width(s_menu_status[7], col_w);
        lv_obj_set_style_text_align(s_menu_status[7], LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        lv_coord_t col_gap = 14;
        lv_coord_t col_w = (content_w - col_gap) / 2;
        lv_coord_t status_row_gap = 26;
        for (size_t i = 0; i < EPHOTO_MENU_STATUS_COUNT; ++i) {
            lv_coord_t status_x = content_x + (lv_coord_t)(i % 2) * (col_w + col_gap);
            lv_coord_t status_y = status_y0 + (lv_coord_t)(i / 2) * status_row_gap;
            lv_obj_set_pos(s_menu_status[i], status_x, status_y);
            lv_obj_set_width(s_menu_status[i], col_w);
            lv_obj_set_style_text_align(s_menu_status[i], LV_TEXT_ALIGN_LEFT, 0);
        }
    }

    for (size_t i = 0; i < EPHOTO_MENU_STATUS_COUNT; ++i) {
        set_obj_hidden(s_menu_status[i], false);
    }

    for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
        lv_obj_set_pos(s_menu_rows[i], content_x, row_y0 + (lv_coord_t)i * row_gap);
        lv_obj_set_size(s_menu_rows[i], content_w, row_h);
        // Keep both columns inside equal row margins.  Aligning after setting
        // the widths also replaces the old fixed 8 px offsets from init.
        lv_coord_t row_pad = landscape ? 16 : 14;
        lv_coord_t col_gap = landscape ? 24 : 18;
        lv_coord_t inner_w = content_w - row_pad * 2 - col_gap;
        lv_coord_t key_w = inner_w / 2;
        lv_coord_t val_w = inner_w - key_w;
        lv_obj_set_width(s_menu_row_keys[i], key_w);
        lv_obj_set_width(s_menu_row_vals[i], val_w);
        lv_obj_align(s_menu_row_keys[i], LV_ALIGN_LEFT_MID, row_pad, 0);
        lv_obj_align(s_menu_row_vals[i], LV_ALIGN_RIGHT_MID, -row_pad, 0);
    }

    if (s_menu_dropdown) {
        bool portrait_dropdown = !landscape;
        lv_coord_t dropdown_w = portrait_dropdown
                                    ? (menu_w - 28)
                                    : (menu_w > 420 ? 420 : menu_w - 40);
        lv_coord_t dropdown_h = portrait_dropdown ? 340 : 320;
        lv_coord_t dropdown_row_h = portrait_dropdown ? 28 : 24;
        lv_coord_t dropdown_step = portrait_dropdown ? 32 : 28;
        if (dropdown_w < 280) {
            dropdown_w = 280;
        }
        lv_obj_set_size(s_menu_dropdown, dropdown_w, dropdown_h);
        lv_obj_center(s_menu_dropdown);
        lv_obj_set_pos(s_menu_dropdown_title, 14, 10);
        for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
            lv_obj_set_pos(s_menu_dropdown_rows[i], 14, 40 + (lv_coord_t)i * dropdown_step);
            lv_obj_set_size(s_menu_dropdown_rows[i], dropdown_w - 28, dropdown_row_h);
            lv_obj_set_width(s_menu_dropdown_labels[i], dropdown_w - 60);
        }
    }

    update_clock_geometry(rotation_deg,
                          s_last_clock_format == (ephoto_clock_format_t)-1
                              ? EPHOTO_CLOCK_FORMAT_24H
                              : s_last_clock_format,
                          s_last_clock_position == (ephoto_clock_position_t)-1
                              ? EPHOTO_CLOCK_POSITION_TOP_RIGHT
                              : s_last_clock_position);

    lv_obj_set_pos(s_notification_card, 12, screen_h - 58);
    lv_obj_set_size(s_notification_card, landscape ? 280 : 260, 44);
    lv_obj_set_width(s_notification_label, lv_pct(100));

    if (s_ota_card) {
        lv_coord_t ota_w = landscape ? 560 : (screen_w - 40);
        lv_coord_t ota_h = landscape ? 270 : 300;
        if (ota_w > screen_w - 24) {
            ota_w = screen_w - 24;
        }
        lv_obj_set_size(s_ota_card, ota_w, ota_h);
        lv_obj_center(s_ota_card);
        lv_obj_set_width(s_ota_line1, ota_w - 40);
        lv_obj_set_width(s_ota_line2, ota_w - 40);
        lv_obj_set_width(s_ota_bar, ota_w - 40);
    }

    if (s_album_panel) {
        lv_coord_t panel_w = screen_w - (landscape ? 20 : 16);
        lv_coord_t panel_h = screen_h - (landscape ? 20 : 16);
        if (panel_w < 420) {
            panel_w = 420;
        }
        if (panel_h < 320) {
            panel_h = 320;
        }
        lv_obj_set_size(s_album_panel, panel_w, panel_h);
        lv_obj_center(s_album_panel);
        lv_obj_set_pos(s_album_title, 16, 10);
        lv_obj_set_pos(s_album_summary, panel_w - (landscape ? 176 : 144), 10);
        lv_obj_set_width(s_album_summary, landscape ? 160 : 128);
        lv_obj_set_style_text_align(s_album_summary, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_album_hint, 16, panel_h - 28);
        lv_obj_set_width(s_album_hint, panel_w - 32);

        uint8_t cols = landscape ? 5U : 3U;
        uint8_t rows = landscape ? 3U : 5U;
        lv_coord_t content_x = 16;
        lv_coord_t content_y = landscape ? 38 : 44;
        lv_coord_t content_w = panel_w - 32;
        lv_coord_t content_h = panel_h - (landscape ? 78 : 84);
        lv_coord_t gap_x = 8;
        lv_coord_t gap_y = 8;
        lv_coord_t tile_w = (content_w - gap_x * (cols - 1)) / cols;
        lv_coord_t tile_h = (content_h - gap_y * (rows - 1)) / rows;

        for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
            lv_coord_t x = content_x + (lv_coord_t)(i % cols) * (tile_w + gap_x);
            lv_coord_t y = content_y + (lv_coord_t)(i / cols) * (tile_h + gap_y);
            lv_obj_set_pos(s_album_tiles[i], x, y);
            lv_obj_set_size(s_album_tiles[i], tile_w, tile_h);
            lv_obj_align(s_album_tile_images[i], LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_set_pos(s_album_tile_current[i], 8, 6);
            lv_coord_t bottom_y = tile_h - 28;
            lv_coord_t badge_w = landscape ? (tile_w / 3) : 46;
            lv_coord_t meta_x = badge_w + 8;
            lv_obj_set_pos(s_album_tile_badges[i], 8, bottom_y);
            lv_obj_set_pos(s_album_tile_meta[i], meta_x, bottom_y);
            lv_obj_set_style_text_font(s_album_tile_badges[i], &ephoto_ui_font_18, 0);
            lv_obj_set_style_text_font(s_album_tile_meta[i], &ephoto_ui_font_18, 0);
            lv_obj_set_width(s_album_tile_badges[i], badge_w);
            lv_obj_set_width(s_album_tile_meta[i], tile_w - meta_x - 8);
            lv_label_set_long_mode(s_album_tile_badges[i], LV_LABEL_LONG_DOT);
            lv_label_set_long_mode(s_album_tile_meta[i], LV_LABEL_LONG_DOT);
        }

        if (s_album_action_popup) {
            lv_coord_t popup_w = landscape ? 360 : (panel_w - 36);
            lv_coord_t popup_h = landscape ? (52 + (lv_coord_t)EPHOTO_ALBUM_MAX_ACTIONS * 38)
                                           : (58 + (lv_coord_t)EPHOTO_ALBUM_MAX_ACTIONS * 42);
            if (popup_h > panel_h - 36) {
                popup_h = panel_h - 36;
            }
            if (popup_w > panel_w - 60) {
                popup_w = panel_w - 60;
            }
            lv_obj_set_size(s_album_action_popup, popup_w, popup_h);
            lv_obj_center(s_album_action_popup);
            lv_obj_set_width(s_album_action_title, popup_w - 40);
            lv_obj_set_style_text_align(s_album_action_title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(s_album_action_title, LV_ALIGN_TOP_MID, 0, 10);
            for (size_t i = 0; i < EPHOTO_ALBUM_MAX_ACTIONS; ++i) {
                lv_coord_t action_row_h = landscape ? 34 : 38;
                lv_coord_t action_step = landscape ? 38 : 42;
                lv_obj_set_size(s_album_action_rows[i], popup_w - 36, action_row_h);
                lv_obj_align(s_album_action_rows[i], LV_ALIGN_TOP_MID, 0, 38 + (lv_coord_t)i * action_step);
                lv_obj_set_width(s_album_action_labels[i], popup_w - 68);
                lv_obj_align(s_album_action_labels[i], LV_ALIGN_CENTER, 0, 0);
            }
        }

    }

    lv_coord_t cache_card_w = landscape ? 500 : 420;
    lv_coord_t cache_card_h = landscape ? 220 : 220;
    lv_obj_set_size(s_cache_card, cache_card_w, cache_card_h);
    lv_obj_center(s_cache_card);

    lv_obj_set_size(s_photo, screen_w, screen_h);
    lv_obj_set_pos(s_photo, 0, 0);

    lv_obj_set_size(s_cache_overlay, screen_w, screen_h);
    lv_obj_set_pos(s_cache_overlay, 0, 0);
    if (s_ota_overlay) {
        lv_obj_set_size(s_ota_overlay, screen_w, screen_h);
        lv_obj_set_pos(s_ota_overlay, 0, 0);
    }
    if (s_boot_overlay) {
        lv_obj_set_size(s_boot_overlay, screen_w, screen_h);
        lv_obj_set_pos(s_boot_overlay, 0, 0);
    }
    if (s_boot_image) {
        lv_obj_set_size(s_boot_image, screen_w, screen_h);
        lv_obj_set_pos(s_boot_image, 0, 0);
    }
    if (s_dim_overlay) {
        lv_obj_set_size(s_dim_overlay, screen_w, screen_h);
        lv_obj_set_pos(s_dim_overlay, 0, 0);
    }

    if (s_boot_card) {
        lv_coord_t boot_w = screen_w - 32;
        lv_coord_t boot_h = landscape ? 204 : 226;
        if (boot_w < 300) {
            boot_w = screen_w - 16;
        }
        if (boot_h > screen_h - 24) {
            boot_h = screen_h - 24;
        }
        lv_obj_set_size(s_boot_card, boot_w, boot_h);
        lv_obj_align(s_boot_card, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_coord_t qr_margin = landscape ? 18 : 16;
        lv_coord_t qr_size = boot_h - qr_margin * 2;
        if (qr_size > EPHOTO_BOOT_QR_SIZE) {
            qr_size = EPHOTO_BOOT_QR_SIZE;
        }
        if (qr_size < 72) {
            qr_size = 72;
        }
        lv_coord_t text_w = boot_w - 18 - qr_size - qr_margin * 2;
        if (text_w < 180) {
            text_w = 180;
        }
        lv_obj_set_pos(s_boot_brand, 18, 12);
        lv_obj_set_pos(s_boot_title, 18, 38);
        lv_obj_set_pos(s_boot_hint, 18, 66);
        lv_obj_set_width(s_boot_brand, text_w);
        lv_obj_set_width(s_boot_title, text_w);
        lv_obj_set_width(s_boot_hint, text_w);
        lv_obj_set_width(s_boot_line1, text_w);
        lv_obj_set_width(s_boot_line2, text_w);
        lv_obj_set_pos(s_boot_line1, 18, landscape ? 102 : 118);
        lv_obj_set_pos(s_boot_line2, 18, landscape ? 130 : 150);
        lv_obj_set_size(s_boot_bar, text_w, 10);
        lv_obj_set_pos(s_boot_bar, 18, boot_h - 22);
        if (s_boot_qr) {
            lv_obj_set_size(s_boot_qr, qr_size, qr_size);
            lv_obj_align(s_boot_qr, LV_ALIGN_TOP_RIGHT, -qr_margin, qr_margin);
        }
    }
}

static void init_menu_panel(lv_obj_t *parent)
{
    s_menu_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu_panel);
    lv_obj_add_style(s_menu_panel, &s_style_glass, 0);
    lv_obj_set_style_bg_opa(s_menu_panel, (lv_opa_t)172, 0);
    lv_obj_set_style_border_opa(s_menu_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_menu_panel, 2, 0);
    set_obj_hidden(s_menu_panel, true);

    s_menu_title = lv_label_create(s_menu_panel);
    lv_obj_add_style(s_menu_title, &s_style_menu_text, 0);
    lv_label_set_text(s_menu_title, "设置");

    s_menu_hint = lv_label_create(s_menu_panel);
    lv_obj_add_style(s_menu_hint, &s_style_menu_muted, 0);
    lv_label_set_text(s_menu_hint, "上下选择  确认进入");
    set_obj_hidden(s_menu_hint, true);

    for (size_t i = 0; i < EPHOTO_MENU_STATUS_COUNT; ++i) {
        s_menu_status[i] = lv_label_create(s_menu_panel);
        lv_obj_add_style(s_menu_status[i], &s_style_menu_status, 0);
        lv_label_set_text(s_menu_status[i], "-");
    }

    for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
        s_menu_rows[i] = lv_obj_create(s_menu_panel);
        lv_obj_remove_style_all(s_menu_rows[i]);
        lv_obj_add_style(s_menu_rows[i], &s_style_panel, 0);
        lv_obj_set_style_bg_opa(s_menu_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_menu_rows[i], 0, 0);
        lv_obj_set_style_pad_hor(s_menu_rows[i], 12, 0);
        lv_obj_set_style_pad_ver(s_menu_rows[i], 4, 0);
        lv_obj_set_style_radius(s_menu_rows[i], 12, 0);
        lv_obj_clear_flag(s_menu_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        set_obj_hidden(s_menu_rows[i], true);

        s_menu_row_keys[i] = lv_label_create(s_menu_rows[i]);
        lv_obj_add_style(s_menu_row_keys[i], &s_style_menu_text, 0);
        lv_obj_align(s_menu_row_keys[i], LV_ALIGN_LEFT_MID, 8, 0);
        lv_label_set_long_mode(s_menu_row_keys[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(s_menu_row_keys[i], "");

        s_menu_row_vals[i] = lv_label_create(s_menu_rows[i]);
        lv_obj_add_style(s_menu_row_vals[i], &s_style_menu_muted, 0);
        lv_obj_align(s_menu_row_vals[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_label_set_long_mode(s_menu_row_vals[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_menu_row_vals[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(s_menu_row_vals[i], "");
    }

    s_menu_dropdown = lv_obj_create(parent);
    lv_obj_remove_style_all(s_menu_dropdown);
    lv_obj_add_style(s_menu_dropdown, &s_style_glass, 0);
    lv_obj_set_style_bg_opa(s_menu_dropdown, (lv_opa_t)184, 0);
    lv_obj_set_style_border_opa(s_menu_dropdown, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_menu_dropdown, 2, 0);
    set_obj_hidden(s_menu_dropdown, true);

    s_menu_dropdown_title = lv_label_create(s_menu_dropdown);
    lv_obj_add_style(s_menu_dropdown_title, &s_style_menu_text, 0);
    lv_obj_set_pos(s_menu_dropdown_title, 14, 10);
    lv_label_set_text(s_menu_dropdown_title, "选择项目");

    for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
        s_menu_dropdown_rows[i] = lv_obj_create(s_menu_dropdown);
        lv_obj_remove_style_all(s_menu_dropdown_rows[i]);
        lv_obj_add_style(s_menu_dropdown_rows[i], &s_style_panel, 0);
        lv_obj_set_style_bg_opa(s_menu_dropdown_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_menu_dropdown_rows[i], 0, 0);
        lv_obj_set_style_pad_hor(s_menu_dropdown_rows[i], 10, 0);
        lv_obj_set_style_pad_ver(s_menu_dropdown_rows[i], 4, 0);
        lv_obj_set_style_radius(s_menu_dropdown_rows[i], 10, 0);
        lv_obj_set_pos(s_menu_dropdown_rows[i], 14, 40 + (lv_coord_t)i * 28);
        lv_obj_set_size(s_menu_dropdown_rows[i], 360, 24);
        lv_obj_clear_flag(s_menu_dropdown_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        set_obj_hidden(s_menu_dropdown_rows[i], true);

        s_menu_dropdown_labels[i] = lv_label_create(s_menu_dropdown_rows[i]);
        lv_obj_add_style(s_menu_dropdown_labels[i], &s_style_menu_text, 0);
        lv_obj_align(s_menu_dropdown_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_long_mode(s_menu_dropdown_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_menu_dropdown_labels[i], 320);
        lv_label_set_text(s_menu_dropdown_labels[i], "");
    }
}

static void init_album_panel(lv_obj_t *parent)
{
    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        s_album_tile_photo_indices[i] = -1;
    }

    s_album_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_album_panel);
    lv_obj_add_style(s_album_panel, &s_style_glass, 0);
    lv_obj_set_style_bg_opa(s_album_panel, (lv_opa_t)180, 0);
    lv_obj_set_style_border_opa(s_album_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_album_panel, 2, 0);
    set_obj_hidden(s_album_panel, true);

    s_album_title = lv_label_create(s_album_panel);
    lv_obj_add_style(s_album_title, &s_style_menu_text, 0);
    lv_label_set_text(s_album_title, "相册");

    s_album_summary = lv_label_create(s_album_panel);
    lv_obj_add_style(s_album_summary, &s_style_menu_muted, 0);
    lv_label_set_text(s_album_summary, "-");

    s_album_hint = lv_label_create(s_album_panel);
    lv_obj_add_style(s_album_hint, &s_style_menu_muted, 0);
    lv_label_set_long_mode(s_album_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_album_hint, "");

    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        s_album_tiles[i] = lv_obj_create(s_album_panel);
        lv_obj_remove_style_all(s_album_tiles[i]);
        lv_obj_add_style(s_album_tiles[i], &s_style_panel, 0);
        lv_obj_set_style_radius(s_album_tiles[i], 18, 0);
        lv_obj_set_style_bg_opa(s_album_tiles[i], (lv_opa_t)226, 0);
        lv_obj_set_style_border_width(s_album_tiles[i], 0, 0);
        lv_obj_clear_flag(s_album_tiles[i], LV_OBJ_FLAG_SCROLLABLE);
        set_obj_hidden(s_album_tiles[i], true);

        s_album_tile_images[i] = lv_image_create(s_album_tiles[i]);
        lv_obj_align(s_album_tile_images[i], LV_ALIGN_TOP_MID, 0, 10);
        set_obj_hidden(s_album_tile_images[i], true);

        s_album_tile_current[i] = lv_label_create(s_album_tiles[i]);
        lv_obj_add_style(s_album_tile_current[i], &s_style_menu_muted, 0);
        lv_obj_set_style_text_color(s_album_tile_current[i], lv_color_hex(0x2D78AF), 0);
        lv_label_set_text(s_album_tile_current[i], "当前");
        set_obj_hidden(s_album_tile_current[i], true);

        s_album_tile_badges[i] = lv_label_create(s_album_tiles[i]);
        lv_obj_add_style(s_album_tile_badges[i], &s_style_menu_text, 0);
        lv_label_set_text(s_album_tile_badges[i], "");

        s_album_tile_meta[i] = lv_label_create(s_album_tiles[i]);
        lv_obj_add_style(s_album_tile_meta[i], &s_style_menu_muted, 0);
        lv_obj_set_style_text_align(s_album_tile_meta[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(s_album_tile_meta[i], "");
    }

    s_album_action_popup = lv_obj_create(s_album_panel);
    lv_obj_remove_style_all(s_album_action_popup);
    lv_obj_add_style(s_album_action_popup, &s_style_glass, 0);
    lv_obj_set_style_bg_opa(s_album_action_popup, (lv_opa_t)226, 0);
    lv_obj_set_style_border_opa(s_album_action_popup, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_album_action_popup, 2, 0);
    lv_obj_set_style_radius(s_album_action_popup, 22, 0);
    lv_obj_set_style_shadow_color(s_album_action_popup, lv_color_hex(0x6FA8D6), 0);
    lv_obj_set_style_shadow_opa(s_album_action_popup, (lv_opa_t)64, 0);
    lv_obj_set_style_shadow_width(s_album_action_popup, 20, 0);
    lv_obj_set_style_shadow_spread(s_album_action_popup, 0, 0);
    lv_obj_clear_flag(s_album_action_popup, LV_OBJ_FLAG_SCROLLABLE);
    set_obj_hidden(s_album_action_popup, true);

    s_album_action_title = lv_label_create(s_album_action_popup);
    lv_obj_add_style(s_album_action_title, &s_style_menu_text, 0);
    lv_obj_set_style_text_color(s_album_action_title, lv_color_hex(0x2B5878), 0);
    lv_label_set_text(s_album_action_title, "选择操作");

    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_ACTIONS; ++i) {
        s_album_action_rows[i] = lv_obj_create(s_album_action_popup);
        lv_obj_remove_style_all(s_album_action_rows[i]);
        lv_obj_add_style(s_album_action_rows[i], &s_style_panel, 0);
        lv_obj_set_style_bg_opa(s_album_action_rows[i], (lv_opa_t)180, 0);
        lv_obj_set_style_radius(s_album_action_rows[i], 14, 0);
        lv_obj_set_style_pad_hor(s_album_action_rows[i], 14, 0);
        lv_obj_set_style_pad_ver(s_album_action_rows[i], 8, 0);
        lv_obj_clear_flag(s_album_action_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        set_obj_hidden(s_album_action_rows[i], true);

        s_album_action_labels[i] = lv_label_create(s_album_action_rows[i]);
        lv_obj_add_style(s_album_action_labels[i], &s_style_menu_text, 0);
        lv_obj_set_style_text_align(s_album_action_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_album_action_labels[i], LV_ALIGN_CENTER, 0, 0);
        lv_label_set_long_mode(s_album_action_labels[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(s_album_action_labels[i], "");
    }
}

static void init_ui_tree(void)
{
    s_boot_initial_photo_pending = true;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &s_style_screen, 0);

    s_photo = lv_image_create(screen);
    lv_obj_set_pos(s_photo, 0, 0);
    lv_obj_set_size(s_photo, s_profile->lcd_h_res, s_profile->lcd_v_res);

    s_dim_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_dim_overlay);
    lv_obj_set_size(s_dim_overlay, s_profile->lcd_h_res, s_profile->lcd_v_res);
    lv_obj_set_pos(s_dim_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dim_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dim_overlay, 0, 0);
    lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    set_obj_hidden(s_dim_overlay, true);

    s_clock_card = lv_obj_create(screen);
    lv_obj_remove_style_all(s_clock_card);
    lv_obj_set_style_bg_opa(s_clock_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_clock_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_opa(s_clock_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(s_clock_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_clock_card, 0, 0);
    lv_obj_set_style_radius(s_clock_card, 0, 0);
    lv_obj_add_flag(s_clock_card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s_clock_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    set_obj_hidden(s_clock_card, true);

    s_clock_primary = lv_label_create(s_clock_card);
    lv_obj_add_style(s_clock_primary, &s_style_clock_text, 0);
    lv_obj_set_style_text_font(s_clock_primary, &ephoto_clock_num_81, 0);
    lv_obj_set_style_text_align(s_clock_primary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_clock_primary, -3, 0);
    lv_label_set_text(s_clock_primary, "");

    s_clock_meridiem = lv_label_create(s_clock_card);
    lv_obj_add_style(s_clock_meridiem, &s_style_clock_text, 0);
    lv_obj_set_style_text_font(s_clock_meridiem, &ephoto_ui_font_27, 0);
    lv_obj_set_style_text_align(s_clock_meridiem, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_clock_meridiem, 0, 0);
    lv_label_set_text(s_clock_meridiem, "");

    s_clock_date = lv_label_create(s_clock_card);
    lv_obj_add_style(s_clock_date, &s_style_clock_text, 0);
    lv_obj_set_style_text_font(s_clock_date, &ephoto_ui_font_27, 0);
    lv_obj_set_style_text_align(s_clock_date, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_clock_date, 0, 0);
    lv_label_set_text(s_clock_date, "");

    s_clock_weekday = lv_label_create(s_clock_card);
    lv_obj_add_style(s_clock_weekday, &s_style_clock_text, 0);
    lv_obj_set_style_text_font(s_clock_weekday, &ephoto_ui_font_27, 0);
    lv_obj_set_style_text_align(s_clock_weekday, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_letter_space(s_clock_weekday, 0, 0);
    lv_label_set_text(s_clock_weekday, "");
    apply_clock_colors(EPHOTO_CLOCK_COLOR_WHITE);

    s_notification_card = lv_obj_create(screen);
    lv_obj_remove_style_all(s_notification_card);
    lv_obj_add_style(s_notification_card, &s_style_glass, 0);
    lv_obj_set_style_bg_opa(s_notification_card, (lv_opa_t)214, 0);
    lv_obj_set_style_border_opa(s_notification_card, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_notification_card, 2, 0);
    s_notification_label = lv_label_create(s_notification_card);
    lv_obj_add_style(s_notification_label, &s_style_text, 0);
    lv_obj_set_style_text_color(s_notification_label, lv_color_hex(0x0F2538), 0);
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_notification_label, "");
    lv_obj_center(s_notification_label);
    set_obj_hidden(s_notification_card, true);

    s_boot_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_boot_overlay);
    lv_obj_add_style(s_boot_overlay, &s_style_screen, 0);
    lv_obj_set_style_bg_opa(s_boot_overlay, LV_OPA_COVER, 0);
    set_obj_hidden(s_boot_overlay, false);

    s_boot_image = lv_image_create(s_boot_overlay);
    if (s_boot_image_pixels) {
        lv_image_set_src(s_boot_image, &s_boot_image_dsc);
    }
    lv_image_set_inner_align(s_boot_image, LV_IMAGE_ALIGN_COVER);
    lv_image_set_antialias(s_boot_image, false);
    lv_obj_set_pos(s_boot_image, 0, 0);

    s_boot_card = lv_obj_create(s_boot_overlay);
    lv_obj_remove_style_all(s_boot_card);
    lv_obj_add_style(s_boot_card, &s_style_glass, 0);
    lv_obj_set_style_bg_color(s_boot_card, lv_color_hex(0xE7F4FF), 0);
    lv_obj_set_style_bg_opa(s_boot_card, (lv_opa_t)214, 0);
    lv_obj_set_style_border_opa(s_boot_card, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_boot_card, 1, 0);
    lv_obj_set_style_shadow_color(s_boot_card, lv_color_hex(0x06111B), 0);
    lv_obj_set_style_shadow_opa(s_boot_card, (lv_opa_t)24, 0);
    lv_obj_set_style_shadow_width(s_boot_card, 18, 0);
    lv_obj_set_style_radius(s_boot_card, 28, 0);

    s_boot_qr = lv_image_create(s_boot_card);
    if (s_boot_qr_dsc.data) {
        lv_image_set_src(s_boot_qr, &s_boot_qr_dsc);
    }
    lv_image_set_inner_align(s_boot_qr, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_radius(s_boot_qr, 14, 0);
    lv_obj_set_style_clip_corner(s_boot_qr, true, 0);
    lv_obj_set_style_bg_color(s_boot_qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_boot_qr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_boot_qr, lv_color_hex(0xA6C9E2), 0);
    lv_obj_set_style_border_width(s_boot_qr, 1, 0);
    lv_obj_set_style_border_opa(s_boot_qr, LV_OPA_70, 0);

    s_boot_brand = lv_label_create(s_boot_card);
    lv_obj_add_style(s_boot_brand, &s_style_muted, 0);
    lv_obj_set_style_text_color(s_boot_brand, lv_color_hex(0x5A89AD), 0);
    lv_label_set_text(s_boot_brand, "E-Photo 忆风电子相框");

    s_boot_title = lv_label_create(s_boot_card);
    lv_obj_add_style(s_boot_title, &s_style_text, 0);
    lv_obj_set_style_text_font(s_boot_title, &ephoto_ui_font_18, 0);
    lv_obj_set_style_text_color(s_boot_title, lv_color_hex(0x12314B), 0);
    lv_label_set_text(s_boot_title, "正在启动你的电子相框");

    s_boot_hint = lv_label_create(s_boot_card);
    lv_obj_add_style(s_boot_hint, &s_style_muted, 0);
    lv_obj_set_width(s_boot_hint, 300);
    lv_label_set_long_mode(s_boot_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_boot_hint, "首次启动会检查图片缓存，请稍候片刻");

    s_boot_line1 = lv_label_create(s_boot_card);
    lv_obj_add_style(s_boot_line1, &s_style_text, 0);
    lv_label_set_long_mode(s_boot_line1, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_boot_line1, "正在扫描照片与缓存文件");

    s_boot_line2 = lv_label_create(s_boot_card);
    lv_obj_add_style(s_boot_line2, &s_style_muted, 0);
    lv_label_set_long_mode(s_boot_line2, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_boot_line2, "请保持供电稳定，不要拔出 TF 卡");

    s_boot_bar = lv_bar_create(s_boot_card);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(0xB7D4E8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_boot_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(0x287FB4), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_boot_bar, 5, LV_PART_INDICATOR);
    lv_bar_set_range(s_boot_bar, 0, 100);
    lv_bar_set_value(s_boot_bar, 0, LV_ANIM_OFF);
    set_obj_hidden(s_boot_bar, true);

    s_cache_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_cache_overlay);
    lv_obj_add_style(s_cache_overlay, &s_style_screen, 0);
    lv_obj_set_style_bg_opa(s_cache_overlay, (lv_opa_t)191, 0);
    set_obj_hidden(s_cache_overlay, true);

    s_cache_card = lv_obj_create(s_cache_overlay);
    lv_obj_remove_style_all(s_cache_card);
    lv_obj_add_style(s_cache_card, &s_style_glass, 0);
    lv_obj_set_size(s_cache_card, 420, 220);
    lv_obj_center(s_cache_card);

    s_cache_title = lv_label_create(s_cache_card);
    lv_obj_add_style(s_cache_title, &s_style_text, 0);
    lv_label_set_text(s_cache_title, "正在建立图片缓存");
    lv_obj_set_pos(s_cache_title, 20, 20);

    s_cache_line1 = lv_label_create(s_cache_card);
    lv_obj_add_style(s_cache_line1, &s_style_muted, 0);
    lv_obj_set_pos(s_cache_line1, 20, 70);
    lv_label_set_text(s_cache_line1, "第0张/共0张");

    s_cache_line2 = lv_label_create(s_cache_card);
    lv_obj_add_style(s_cache_line2, &s_style_muted, 0);
    lv_obj_set_pos(s_cache_line2, 20, 100);
    lv_label_set_text(s_cache_line2, "进度 0%");

    s_cache_bar = lv_bar_create(s_cache_card);
    lv_obj_set_size(s_cache_bar, 360, 20);
    lv_obj_set_pos(s_cache_bar, 20, 150);
    lv_bar_set_range(s_cache_bar, 0, 100);
    lv_bar_set_value(s_cache_bar, 0, LV_ANIM_OFF);

    s_ota_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_ota_overlay);
    lv_obj_add_style(s_ota_overlay, &s_style_screen, 0);
    lv_obj_set_style_bg_opa(s_ota_overlay, (lv_opa_t)235, 0);
    set_obj_hidden(s_ota_overlay, true);

    s_ota_card = lv_obj_create(s_ota_overlay);
    lv_obj_remove_style_all(s_ota_card);
    lv_obj_add_style(s_ota_card, &s_style_glass, 0);
    lv_obj_set_style_bg_color(s_ota_card, lv_color_hex(0xE7F4FF), 0);
    lv_obj_set_style_bg_opa(s_ota_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(s_ota_card, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ota_card, 2, 0);
    lv_obj_set_style_radius(s_ota_card, 28, 0);

    s_ota_title = lv_label_create(s_ota_card);
    lv_obj_add_style(s_ota_title, &s_style_text, 0);
    lv_obj_set_style_text_font(s_ota_title, &ephoto_ui_font_27, 0);
    lv_obj_set_style_text_color(s_ota_title, lv_color_hex(0x12314B), 0);
    lv_obj_set_pos(s_ota_title, 20, 24);
    lv_label_set_text(s_ota_title, "正在下载固件");

    s_ota_line1 = lv_label_create(s_ota_card);
    lv_obj_add_style(s_ota_line1, &s_style_text, 0);
    lv_obj_set_pos(s_ota_line1, 20, 82);
    lv_label_set_text(s_ota_line1, "更新进度 · 0%");

    s_ota_line2 = lv_label_create(s_ota_card);
    lv_obj_add_style(s_ota_line2, &s_style_muted, 0);
    lv_obj_set_pos(s_ota_line2, 20, 122);
    lv_label_set_long_mode(s_ota_line2, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ota_line2, "请保持设备联网，不要断开电源");

    s_ota_bar = lv_bar_create(s_ota_card);
    lv_obj_set_pos(s_ota_bar, 20, 190);
    lv_obj_set_height(s_ota_bar, 22);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0xB7D4E8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ota_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x287FB4), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ota_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_bar, 7, LV_PART_INDICATOR);

    init_menu_panel(screen);
    init_album_panel(screen);
}

static void update_photo(const ephoto_app_state_t *state,
                         const ephoto_settings_t *effective_settings,
                         const void *photo_pixels,
                         bool photo_ready)
{
    if (!state || !effective_settings) {
        return;
    }

    const void *display_pixels = photo_pixels;
    const char *photo_key = state->current_photo_path;
    ephoto_fit_mode_t fit_mode = effective_settings->fit_mode;
    bool use_boot_fallback = (!state->current_photo_path[0]) && s_boot_image_pixels != NULL;

    if ((!photo_ready || !display_pixels) && use_boot_fallback) {
        display_pixels = s_boot_image_dsc.data;
        photo_ready = display_pixels != NULL;
        photo_key = "__boot_fallback__";
        fit_mode = EPHOTO_FIT_COVER;
    }

    if (!photo_ready || !display_pixels) {
        return;
    }

    bool changed = strcmp(s_last_photo_path, photo_key) != 0 ||
                   s_last_photo_rotation != effective_settings->rotation_deg ||
                   s_last_photo_fit_mode != fit_mode ||
                   s_photo_dsc.data != display_pixels;

    if (!changed) {
        return;
    }

    memset(&s_photo_dsc, 0, sizeof(s_photo_dsc));
    lv_coord_t logical_w = s_display ? (lv_coord_t)lv_display_get_horizontal_resolution(s_display) : s_profile->lcd_h_res;
    lv_coord_t logical_h = s_display ? (lv_coord_t)lv_display_get_vertical_resolution(s_display) : s_profile->lcd_v_res;
    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.flags = 0;
    s_photo_dsc.header.w = logical_w;
    s_photo_dsc.header.h = logical_h;
    s_photo_dsc.header.stride = (uint32_t)logical_w * 2U;
    s_photo_dsc.data_size = (uint32_t)(logical_w * logical_h * 2);
    s_photo_dsc.data = display_pixels;

    lv_image_set_src(s_photo, &s_photo_dsc);
    lv_obj_set_size(s_photo, logical_w, logical_h);
    lv_obj_invalidate(s_photo);

    strlcpy(s_last_photo_path, photo_key, sizeof(s_last_photo_path));
    s_last_photo_rotation = effective_settings->rotation_deg;
    s_last_photo_fit_mode = fit_mode;
}

static void update_clock(const ephoto_settings_t *settings, const char *clock_text, bool visible)
{
    ephoto_clock_format_t format = settings ? settings->clock_format : EPHOTO_CLOCK_FORMAT_24H;
    ephoto_clock_position_t position =
        settings ? normalize_clock_position_local(settings->clock_position) : EPHOTO_CLOCK_POSITION_TOP_RIGHT;
    ephoto_clock_color_t color =
        settings ? normalize_clock_color_local(settings->clock_color) : EPHOTO_CLOCK_COLOR_WHITE;
    set_obj_hidden(s_clock_card, !visible || !clock_text || !clock_text[0]);
    if (!visible || !clock_text || !clock_text[0]) {
        return;
    }

    if (format != s_last_clock_format || position != s_last_clock_position) {
        s_last_clock_format = format;
        s_last_clock_position = position;
        update_clock_geometry(settings ? settings->rotation_deg : 0, format, position);
    }
    if (color != s_last_clock_color) {
        s_last_clock_color = color;
        apply_clock_colors(color);
    }

    struct tm tm_now = {0};
    bool tm_valid = clock_service_get_local_tm(&tm_now);
    char primary[32];
    char meridiem[12];
    char date_text[40];
    char weekday_text[16];

    if (tm_valid) {
        int hour = tm_now.tm_hour;
        if (format == EPHOTO_CLOCK_FORMAT_12H) {
            int display_hour = hour % 12;
            if (display_hour == 0) {
                display_hour = 12;
            }
            snprintf(primary, sizeof(primary), "%02d:%02d", display_hour, tm_now.tm_min);
            strlcpy(meridiem, hour < 12 ? "上午" : "下午", sizeof(meridiem));
        } else {
            snprintf(primary, sizeof(primary), "%02d:%02d", hour, tm_now.tm_min);
            meridiem[0] = '\0';
        }
        snprintf(date_text,
                 sizeof(date_text),
                 "%04d年%02d月%02d日",
                 tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1,
                 tm_now.tm_mday);
        strlcpy(weekday_text, weekday_cn(tm_now.tm_wday), sizeof(weekday_text));
    } else {
        primary[0] = '\0';
        meridiem[0] = '\0';
        date_text[0] = '\0';
        weekday_text[0] = '\0';
    }

    set_label_text_if_changed(s_clock_primary, primary);
    set_label_text_if_changed(s_clock_meridiem, meridiem);
    set_label_text_if_changed(s_clock_date, date_text);
    set_label_text_if_changed(s_clock_weekday, weekday_text);
    bool show_meridiem = format == EPHOTO_CLOCK_FORMAT_12H && meridiem[0] != '\0';
    layout_clock_content(settings ? settings->rotation_deg : 0, show_meridiem);
}

static void update_notification(const ephoto_app_state_t *state, bool photo_loading)
{
    const char *message = NULL;
    lv_coord_t screen_w = s_display ? (lv_coord_t)lv_display_get_horizontal_resolution(s_display) : s_profile->lcd_h_res;
    lv_coord_t screen_h = s_display ? (lv_coord_t)lv_display_get_vertical_resolution(s_display) : s_profile->lcd_v_res;
    bool landscape = state && state->settings.rotation_deg == 90;
    if (state->notification.updated_at_ms > 0 &&
        (clock_service_now_ms() - state->notification.updated_at_ms) <= 3000) {
        message = state->notification.screen_text[0] ? state->notification.screen_text : state->notification.text;
    } else if (state->storage.photo_count <= 0) {
        message = "暂无照片，请上传图片";
    } else if (photo_loading) {
        message = "正在载入照片";
    }

    bool show = message && message[0];
    set_obj_hidden(s_notification_card, !show);
    if (show) {
        bool multiline = strchr(message, '\n') != NULL;
        lv_coord_t card_w = multiline ? (landscape ? 520 : (screen_w - 24)) : (landscape ? 280 : 260);
        lv_coord_t card_h = multiline ? 86 : 44;
        if (card_w > screen_w - 24) {
            card_w = screen_w - 24;
        }
        lv_obj_set_size(s_notification_card, card_w, card_h);
        lv_obj_set_pos(s_notification_card, 12, screen_h - card_h - 12);
        lv_obj_set_size(s_notification_label, card_w - 20, LV_SIZE_CONTENT);
        lv_obj_center(s_notification_label);
        set_label_text_if_changed(s_notification_label, message);
    }
}

static void update_cache_overlay(const ephoto_app_state_t *state)
{
    bool show = state->cache_build_active && !state->interaction_locked;
    set_obj_hidden(s_cache_overlay, !show);
    if (!show) {
        return;
    }

    uint32_t total = state->cache_build_total ? state->cache_build_total : 1U;
    uint32_t done = state->cache_build_done > total ? total : state->cache_build_done;
    uint32_t photo_total = state->cache_build_photo_total > 0 ? state->cache_build_photo_total : total;
    uint32_t photo_index = state->cache_build_photo_index > 0 ? state->cache_build_photo_index : done;
    if (photo_index > photo_total) {
        photo_index = photo_total;
    }
    uint32_t percent = (done * 100U) / total;
    char line1[32];
    char line2[32];
    snprintf(line1, sizeof(line1), "第%lu张/共%lu张", (unsigned long)photo_index, (unsigned long)photo_total);
    snprintf(line2, sizeof(line2), "缓存处理中 · %lu%%", (unsigned long)percent);
    set_label_text_if_changed(s_cache_line1, line1);
    set_label_text_if_changed(s_cache_line2, line2);
    lv_bar_set_value(s_cache_bar, (int32_t)percent, LV_ANIM_OFF);
}

static void update_ota_overlay(const ephoto_app_state_t *state)
{
    bool active;
    const char *title;
    const char *line1;
    const char *line2;
    uint32_t percent = 0;
    char progress_text[64];

    if (!state || !s_ota_overlay) {
        return;
    }

    active = state->ota.updating || state->ota.reboot_required ||
             state->ota.stage == EPHOTO_OTA_STAGE_DOWNLOADING ||
             state->ota.stage == EPHOTO_OTA_STAGE_APPLYING ||
             state->ota.stage == EPHOTO_OTA_STAGE_RESTARTING;
    set_obj_hidden(s_ota_overlay, !active);
    if (!active) {
        return;
    }

    switch (state->ota.stage) {
    case EPHOTO_OTA_STAGE_DOWNLOADING:
        title = "正在下载固件";
        percent = state->ota.progress_percent;
        line1 = "请保持设备联网，不要断开电源";
        line2 = "下载完成后将自动校验并写入设备";
        break;
    case EPHOTO_OTA_STAGE_APPLYING:
        title = "正在写入固件";
        percent = 100;
        line1 = "正在写入新固件，请不要断电";
        line2 = "写入完成后设备将自动重启";
        break;
    case EPHOTO_OTA_STAGE_RESTARTING:
    default:
        title = "设备即将重启";
        percent = 100;
        line1 = "固件更新完成，请保持供电稳定";
        line2 = state->ota.message[0] ? state->ota.message : "正在准备重启设备";
        break;
    }

    if (percent > 100) {
        percent = 100;
    }
    snprintf(progress_text, sizeof(progress_text), "更新进度 · %lu%%", (unsigned long)percent);
    set_label_text_if_changed(s_ota_title, title);
    set_label_text_if_changed(s_ota_line1, progress_text);
    set_label_text_if_changed(s_ota_line2, line1);
    if (state->ota.stage == EPHOTO_OTA_STAGE_RESTARTING && state->ota.message[0]) {
        set_label_text_if_changed(s_ota_line2, line2);
    }
    lv_bar_set_value(s_ota_bar, (int32_t)percent, LV_ANIM_OFF);
    lv_obj_move_foreground(s_ota_overlay);
}

static void update_menu(const ephoto_app_state_t *state)
{
    static char s_last_title[EPHOTO_MENU_LABEL_LEN];
    static char s_last_status[EPHOTO_MENU_STATUS_COUNT][96];
    static char s_last_row_keys[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    static char s_last_row_vals[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    static bool s_last_row_visible[EPHOTO_MENU_MAX_ITEMS];
    static uint8_t s_last_selected_index = 0xFF;
    static bool s_last_editing;
    static char s_last_dropdown_title[EPHOTO_MENU_LABEL_LEN];
    static char s_last_dropdown_text[EPHOTO_MENU_MAX_ITEMS][EPHOTO_MENU_LABEL_LEN];
    static bool s_last_dropdown_visible[EPHOTO_MENU_MAX_ITEMS];
    static uint8_t s_last_dropdown_selected = 0xFF;
    static uint8_t s_last_dropdown_count = 0xFF;
    char status_line[EPHOTO_MENU_STATUS_COUNT][96];
    bool editing_dropdown_visible = state->menu_visible && state->menu.editing && state->menu.edit_choice_count > 0;

    set_obj_hidden(s_menu_panel, !state->menu_visible);
    set_obj_hidden(s_menu_dropdown, !editing_dropdown_visible);
    bool should_dim_photo = state->menu_visible || state->album.visible;
    if (s_photo &&
        (!s_last_menu_photo_dim_valid || s_last_menu_photo_dimmed != should_dim_photo)) {
        lv_obj_set_style_opa(s_photo, should_dim_photo ? (lv_opa_t)72 : LV_OPA_COVER, 0);
        s_last_menu_photo_dim_valid = true;
        s_last_menu_photo_dimmed = should_dim_photo;
    }
    if (!state->menu_visible) {
        for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
            set_obj_hidden(s_menu_rows[i], true);
            set_obj_hidden(s_menu_dropdown_rows[i], true);
            s_last_row_visible[i] = false;
            s_last_dropdown_visible[i] = false;
            s_last_row_keys[i][0] = '\0';
            s_last_row_vals[i][0] = '\0';
            s_last_dropdown_text[i][0] = '\0';
        }
        s_last_selected_index = 0xFF;
        s_last_dropdown_selected = 0xFF;
        return;
    }

    const char *ssid = "-";
    const char *ip_addr = "-";
    uint32_t capacity_gb_x10 = 0;
    uint32_t used_percent = 0;
    if (state->storage.capacity_bytes > 0) {
        const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;
        capacity_gb_x10 =
            (uint32_t)(((state->storage.capacity_bytes * 10ULL) + (gb / 2ULL)) / gb);
        used_percent = (uint32_t)((state->storage.used_bytes * 100ULL +
                                   (state->storage.capacity_bytes / 2ULL)) /
                                  state->storage.capacity_bytes);
        if (used_percent > 100U) {
            used_percent = 100U;
        }
    }

    if (state->network_mode == EPHOTO_NETWORK_STA) {
        ssid = state->current_ssid[0] ? state->current_ssid : "-";
        const char *sta_ip = wifi_admin_get_sta_ip();
        if (sta_ip && sta_ip[0]) {
            ip_addr = sta_ip;
        }
    } else if (state->network_mode == EPHOTO_NETWORK_AP) {
        const char *ap_ssid = wifi_admin_get_ap_ssid();
        const char *ap_ip = wifi_admin_get_ap_ip();
        if (ap_ssid && ap_ssid[0]) {
            ssid = ap_ssid;
        }
        if (ap_ip && ap_ip[0]) {
            ip_addr = ap_ip;
        }
    }

    snprintf(status_line[0], sizeof(status_line[0]), "网络模式: %s", network_mode_label_cn(state->network_mode));
    snprintf(status_line[1], sizeof(status_line[1]), "网络信息: %s", ssid);
    snprintf(status_line[2], sizeof(status_line[2]), "IP地址: %s", ip_addr);
    if (state->storage.photo_count >= 0) {
        snprintf(status_line[3], sizeof(status_line[3]), "照片数量: %d张", state->storage.photo_count);
    } else {
        strlcpy(status_line[3], "照片数量: -张", sizeof(status_line[3]));
    }
    if (capacity_gb_x10 > 0) {
        snprintf(status_line[4],
                 sizeof(status_line[4]),
                 "容量: %lu.%luGB",
                 (unsigned long)(capacity_gb_x10 / 10U),
                 (unsigned long)(capacity_gb_x10 % 10U));
    } else {
        strlcpy(status_line[4], "容量: -GB", sizeof(status_line[4]));
    }
    if (state->storage.capacity_bytes > 0) {
        snprintf(status_line[5], sizeof(status_line[5]), "已用: %lu%%", (unsigned long)used_percent);
    } else {
        strlcpy(status_line[5], "已用: -%", sizeof(status_line[5]));
    }
    if (state->settings.clock_visible) {
        snprintf(status_line[6],
                 sizeof(status_line[6]),
                 "时钟: 开/%s",
                 clock_format_label_cn(state->settings.clock_format));
    } else {
        strlcpy(status_line[6], "时钟: 关", sizeof(status_line[6]));
    }
    snprintf(status_line[7], sizeof(status_line[7]), "系统版本: %s", firmware_version_string());

    if (strcmp(s_last_title, state->menu.title_key[0] ? state->menu.title_key : "设置") != 0) {
        strlcpy(s_last_title, state->menu.title_key[0] ? state->menu.title_key : "设置", sizeof(s_last_title));
        set_label_text_if_changed(s_menu_title, s_last_title);
    }
    for (size_t i = 0; i < EPHOTO_MENU_STATUS_COUNT; ++i) {
        if (strcmp(s_last_status[i], status_line[i]) != 0) {
            strlcpy(s_last_status[i], status_line[i], sizeof(s_last_status[i]));
            set_label_text_if_changed(s_menu_status[i], s_last_status[i]);
        }
    }

    for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
        bool visible = i < state->menu.option_count;
        if (s_last_row_visible[i] != visible) {
            set_obj_hidden(s_menu_rows[i], !visible);
            s_last_row_visible[i] = visible;
        }
        if (!visible) {
            continue;
        }

        if (strcmp(s_last_row_keys[i], state->menu.item_keys[i]) != 0) {
            strlcpy(s_last_row_keys[i], state->menu.item_keys[i], sizeof(s_last_row_keys[i]));
            set_label_text_if_changed(s_menu_row_keys[i], s_last_row_keys[i]);
        }
        if (strcmp(s_last_row_vals[i], state->menu.item_values[i]) != 0) {
            strlcpy(s_last_row_vals[i], state->menu.item_values[i], sizeof(s_last_row_vals[i]));
            set_label_text_if_changed(s_menu_row_vals[i], s_last_row_vals[i]);
        }

        if (s_last_selected_index == 0xFF ||
            s_last_selected_index == i ||
            state->menu.selected_index == i ||
            s_last_editing != state->menu.editing) {
            set_row_style(s_menu_rows[i],
                          i == state->menu.selected_index,
                          i == state->menu.selected_index && state->menu.editing);
        }
    }
    s_last_selected_index = state->menu.selected_index;
    s_last_editing = state->menu.editing;

    if (editing_dropdown_visible) {
        uint8_t dropdown_count = state->menu.edit_choice_count > EPHOTO_MENU_MAX_ITEMS
                                     ? EPHOTO_MENU_MAX_ITEMS
                                     : state->menu.edit_choice_count;
        if (s_last_dropdown_count != dropdown_count) {
            bool landscape = s_last_layout_rotation == 90;
            lv_coord_t panel_w = lv_obj_get_width(s_menu_panel);
            lv_coord_t dropdown_w = landscape
                                        ? (panel_w > 420 ? 420 : panel_w - 40)
                                        : (panel_w - 28);
            lv_coord_t row_h = landscape ? 24 : 28;
            lv_coord_t row_step = landscape ? 28 : 32;
            lv_coord_t dropdown_h = 52 + (lv_coord_t)dropdown_count * row_step;
            if (dropdown_w < 280) {
                dropdown_w = 280;
            }
            lv_obj_set_size(s_menu_dropdown, dropdown_w, dropdown_h);
            lv_obj_center(s_menu_dropdown);
            lv_obj_set_pos(s_menu_dropdown_title, 14, 10);
            for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
                lv_obj_set_pos(s_menu_dropdown_rows[i], 14, 40 + (lv_coord_t)i * row_step);
                lv_obj_set_size(s_menu_dropdown_rows[i], dropdown_w - 28, row_h);
                lv_obj_set_width(s_menu_dropdown_labels[i], dropdown_w - 60);
            }
            s_last_dropdown_count = dropdown_count;
        }
        if (strcmp(s_last_dropdown_title,
                   state->menu.selected_key[0] ? state->menu.selected_key : "选择项目") != 0) {
            strlcpy(s_last_dropdown_title,
                    state->menu.selected_key[0] ? state->menu.selected_key : "选择项目",
                    sizeof(s_last_dropdown_title));
            set_label_text_if_changed(s_menu_dropdown_title, s_last_dropdown_title);
        }
        for (size_t i = 0; i < dropdown_count; ++i) {
            if (!s_last_dropdown_visible[i]) {
                set_obj_hidden(s_menu_dropdown_rows[i], false);
                s_last_dropdown_visible[i] = true;
            }
            if (strcmp(s_last_dropdown_text[i], state->menu.edit_choices[i]) != 0) {
                strlcpy(s_last_dropdown_text[i], state->menu.edit_choices[i], sizeof(s_last_dropdown_text[i]));
                set_label_text_if_changed(s_menu_dropdown_labels[i], s_last_dropdown_text[i]);
            }
            if (s_last_dropdown_selected == 0xFF ||
                s_last_dropdown_selected == i ||
                state->menu.edit_choice_index == i) {
                set_row_style(s_menu_dropdown_rows[i], i == state->menu.edit_choice_index, false);
            }
        }
        for (size_t i = dropdown_count; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
            if (s_last_dropdown_visible[i]) {
                set_obj_hidden(s_menu_dropdown_rows[i], true);
                s_last_dropdown_visible[i] = false;
            }
        }
        s_last_dropdown_selected = state->menu.edit_choice_index;
    } else {
        s_last_dropdown_title[0] = '\0';
        s_last_dropdown_count = 0xFF;
        for (size_t i = 0; i < EPHOTO_MENU_MAX_ITEMS; ++i) {
            if (s_last_dropdown_visible[i]) {
                set_obj_hidden(s_menu_dropdown_rows[i], true);
                s_last_dropdown_visible[i] = false;
            }
            s_last_dropdown_text[i][0] = '\0';
        }
        s_last_dropdown_selected = 0xFF;
    }
}

static void update_album(const ephoto_app_state_t *state)
{
    static char s_last_album_title[EPHOTO_MENU_LABEL_LEN];
    static char s_last_album_summary[64];
    static char s_last_album_hint[96];
    static char s_last_album_action_title[32];
    static char s_last_album_badges[EPHOTO_ALBUM_MAX_SLOTS][24];
    static char s_last_album_meta[EPHOTO_ALBUM_MAX_SLOTS][32];
    static char s_last_album_actions[EPHOTO_ALBUM_MAX_ACTIONS][24];
    static bool s_last_album_visible[EPHOTO_ALBUM_MAX_SLOTS];
    static bool s_last_album_current[EPHOTO_ALBUM_MAX_SLOTS];
    static uint8_t s_last_album_selected_slot = 0xFF;
    static uint8_t s_last_album_selected_action = 0xFF;
    static bool s_last_album_action_popup_visible = false;
    size_t missing_slots[EPHOTO_ALBUM_MAX_SLOTS];
    size_t missing_count = 0;

    set_obj_hidden(s_album_panel, !state->album.visible);

    if (!state->album.visible) {
        for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
            if (s_last_album_visible[i]) {
                set_obj_hidden(s_album_tiles[i], true);
                s_last_album_visible[i] = false;
            }
        }
        release_all_album_tile_images();
        set_obj_hidden(s_album_action_popup, true);
        s_last_album_action_popup_visible = false;
        s_last_album_selected_slot = 0xFF;
        s_last_album_selected_action = 0xFF;
        return;
    }

    if (strcmp(s_last_album_title, state->album.title) != 0) {
        strlcpy(s_last_album_title, state->album.title, sizeof(s_last_album_title));
        set_label_text_if_changed(s_album_title, s_last_album_title);
    }
    if (strcmp(s_last_album_summary, state->album.summary) != 0) {
        strlcpy(s_last_album_summary, state->album.summary, sizeof(s_last_album_summary));
        set_label_text_if_changed(s_album_summary, s_last_album_summary);
    }
    if (strcmp(s_last_album_hint, state->album.hint) != 0) {
        strlcpy(s_last_album_hint, state->album.hint, sizeof(s_last_album_hint));
        set_label_text_if_changed(s_album_hint, s_last_album_hint);
    }
    if (strcmp(s_last_album_action_title, state->album.action_title) != 0) {
        strlcpy(s_last_album_action_title, state->album.action_title, sizeof(s_last_album_action_title));
        set_label_text_if_changed(s_album_action_title, s_last_album_action_title);
    }

    if (s_last_album_action_popup_visible != state->album.action_popup_visible) {
        set_obj_hidden(s_album_action_popup, !state->album.action_popup_visible);
        s_last_album_action_popup_visible = state->album.action_popup_visible;
    }

    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_ACTIONS; ++i) {
        bool visible = state->album.action_popup_visible && i < state->album.action_count;
        set_obj_hidden(s_album_action_rows[i], !visible);
        if (!visible) {
            s_last_album_actions[i][0] = '\0';
            continue;
        }
        if (strcmp(s_last_album_actions[i], state->album.actions[i]) != 0) {
            strlcpy(s_last_album_actions[i], state->album.actions[i], sizeof(s_last_album_actions[i]));
            set_label_text_if_changed(s_album_action_labels[i], s_last_album_actions[i]);
        }
        if (s_last_album_selected_action == 0xFF ||
            s_last_album_selected_action == i ||
            state->album.selected_action == i) {
            bool destructive = strcmp(state->album.actions[i], "删除照片") == 0;
            set_action_popup_row_style(s_album_action_rows[i],
                                       s_album_action_labels[i],
                                       i == state->album.selected_action,
                                       destructive);
        }
    }
    s_last_album_selected_action = state->album.selected_action;

    s_album_pending_thumbnail_work = false;
    for (size_t i = 0; i < EPHOTO_ALBUM_MAX_SLOTS; ++i) {
        bool visible = i < state->album.slot_count;
        if (!visible) {
            set_obj_hidden(s_album_tiles[i], true);
            s_last_album_visible[i] = false;
            release_album_tile_image(i);
            continue;
        }
        if (!s_last_album_visible[i]) {
            set_obj_hidden(s_album_tiles[i], false);
            s_last_album_visible[i] = true;
        }

        const ephoto_album_slot_t *slot = &state->album.slots[i];
        if (strcmp(s_last_album_badges[i], slot->badge) != 0) {
            strlcpy(s_last_album_badges[i], slot->badge, sizeof(s_last_album_badges[i]));
            set_label_text_if_changed(s_album_tile_badges[i], s_last_album_badges[i]);
        }
        if (strcmp(s_last_album_meta[i], slot->meta) != 0) {
            strlcpy(s_last_album_meta[i], slot->meta, sizeof(s_last_album_meta[i]));
            set_label_text_if_changed(s_album_tile_meta[i], s_last_album_meta[i]);
        }
        if (s_last_album_current[i] != slot->is_current) {
            set_obj_hidden(s_album_tile_current[i], !slot->is_current);
            s_last_album_current[i] = slot->is_current;
        }
        if (slot->thumb_ready) {
            if (s_album_tile_photo_indices[i] != slot->photo_index) {
                if (missing_count < EPHOTO_ALBUM_MAX_SLOTS) {
                    if (i == state->album.selected_slot && missing_count > 0) {
                        memmove(&missing_slots[1], &missing_slots[0], missing_count * sizeof(missing_slots[0]));
                        missing_slots[0] = i;
                    } else {
                        missing_slots[missing_count] = i;
                    }
                    ++missing_count;
                }
                s_album_pending_thumbnail_work = true;
            }
        } else {
            release_album_tile_image(i);
        }

        if (s_album_tile_pixels[i]) {
            set_obj_hidden(s_album_tile_images[i], false);
        } else {
            set_obj_hidden(s_album_tile_images[i], true);
        }

        if (s_last_album_selected_slot == 0xFF ||
            s_last_album_selected_slot == i ||
            state->album.selected_slot == i) {
            set_row_style(s_album_tiles[i],
                          i == state->album.selected_slot,
                          state->album.action_popup_visible && i == state->album.selected_slot);
        }
    }
    s_last_album_selected_slot = state->album.selected_slot;

    if (missing_count > 0) {
        size_t load_budget = missing_count;
        if (load_budget > 4U) {
            load_budget = 4U;
        }
        for (size_t load_i = 0; load_i < load_budget; ++load_i) {
            size_t slot_to_load = missing_slots[load_i];
            if (slot_to_load >= state->album.slot_count) {
                continue;
            }
            const ephoto_album_slot_t *slot = &state->album.slots[slot_to_load];
            load_album_tile_image(slot_to_load, slot->photo_index);
            if (s_album_tile_pixels[slot_to_load]) {
                set_obj_hidden(s_album_tile_images[slot_to_load], false);
            } else {
                set_obj_hidden(s_album_tile_images[slot_to_load], true);
            }
        }
    } else {
        s_album_pending_thumbnail_work = false;
    }
}

esp_err_t display_lvgl_backend_init(const board_profile_t *profile,
                                    esp_lcd_panel_io_handle_t io_handle,
                                    esp_lcd_panel_handle_t panel_handle,
                                    uint16_t startup_rotation_deg)
{
    ESP_RETURN_ON_FALSE(profile, ESP_ERR_INVALID_ARG, TAG, "profile required");
    s_profile = profile;
    s_panel = panel_handle;
    s_startup_rotation_deg = startup_rotation_deg == 90 ? 90 : 0;

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 8,
        .task_stack = 8192,
        .task_affinity = -1,
        .task_max_sleep_ms = 50,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");
    style_init_once();
    init_boot_qr_asset();
    load_boot_image_asset(profile);

    uint32_t full_screen_pixels = profile->lcd_h_res * profile->lcd_v_res;
    uint32_t draw_buffer_pixels = full_screen_pixels;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = draw_buffer_pixels,
        .double_buffer = true,
        .hres = profile->lcd_h_res,
        .vres = profile->lcd_v_res,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };

    s_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(s_display, ESP_FAIL, TAG, "add lvgl display failed");

    s_display_rotation = s_startup_rotation_deg == 90 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0;
    lv_display_set_rotation(s_display, s_display_rotation);

    lvgl_port_lock(0);
    init_ui_tree();
    apply_layout(s_startup_rotation_deg);
    s_last_layout_rotation = s_startup_rotation_deg;
    lvgl_port_unlock();

    return ESP_OK;
}

void display_lvgl_backend_set_startup_scan_progress(size_t completed, size_t total)
{
    if (!s_display || !s_boot_overlay || !s_boot_bar) {
        return;
    }

    if (total > 0 && completed > total) {
        completed = total;
    }
    int percent = total > 0 ? (int)((completed * 100U) / total) : 0;
    // Reserve the final tenth of the bar for preparing the first displayed photo.
    int bar_percent = (percent * 90) / 100;
    if (s_boot_scan_last_total == total && s_boot_scan_last_percent == percent && completed < total) {
        return;
    }
    s_boot_scan_last_total = total;
    s_boot_scan_last_percent = percent;

    lvgl_port_lock(0);
    set_obj_hidden(s_boot_overlay, false);
    set_obj_hidden(s_boot_bar, false);
    set_label_text_if_changed(s_boot_brand, "E-Photo 忆风电子相框");
    set_label_text_if_changed(s_boot_title, "正在读取 TF 卡相册");
    if (total == 0) {
        set_label_text_if_changed(s_boot_hint, "正在统计 TF 卡中的文件，请稍候片刻");
        set_label_text_if_changed(s_boot_line1, "正在准备扫描图片");
        set_label_text_if_changed(s_boot_line2, "扫描进度将在统计完成后显示");
    } else {
        char line1[64];
        char line2[64];
        set_label_text_if_changed(s_boot_hint, "正在扫描 TF 卡中的图片，请保持供电稳定");
        snprintf(line1,
                 sizeof(line1),
                 "已扫描%lu / 共%lu 个文件",
                 (unsigned long)completed,
                 (unsigned long)total);
        snprintf(line2,
                 sizeof(line2),
                 completed >= total ? "扫描完成，正在整理相册" : "扫描进度 · %d%%",
                 percent);
        set_label_text_if_changed(s_boot_line1, line1);
        set_label_text_if_changed(s_boot_line2, line2);
    }
    lv_bar_set_value(s_boot_bar, bar_percent, LV_ANIM_OFF);
    lvgl_port_unlock();
}

void display_lvgl_backend_apply_settings(const ephoto_settings_t *settings)
{
    if (!settings || !s_display) {
        return;
    }
    lvgl_port_lock(0);
    lv_display_rotation_t rotation = settings->rotation_deg == 90 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0;
    if (rotation != s_display_rotation) {
        lv_display_set_rotation(s_display, rotation);
        s_display_rotation = rotation;
    }
    if (s_last_layout_rotation != settings->rotation_deg) {
        apply_layout(settings->rotation_deg);
        s_last_layout_rotation = settings->rotation_deg;
    }
    if (s_last_clock_format != settings->clock_format ||
        s_last_clock_position != settings->clock_position) {
        s_last_clock_format = settings->clock_format;
        s_last_clock_position = settings->clock_position;
        update_clock_geometry(settings->rotation_deg, settings->clock_format, settings->clock_position);
    }
    if (s_last_clock_color != settings->clock_color) {
        s_last_clock_color = settings->clock_color;
        apply_clock_colors(settings->clock_color);
    }
    if (!s_last_overlay_screen_on_valid ||
        s_last_overlay_screen_on != settings->screen_on ||
        s_last_overlay_brightness != settings->brightness) {
        apply_brightness(settings->brightness, settings->screen_on);
        s_last_overlay_screen_on = settings->screen_on;
        s_last_overlay_screen_on_valid = true;
        s_last_overlay_brightness = settings->brightness;
    }
    lvgl_port_unlock();
}

void display_lvgl_backend_set_software_brightness_enabled(bool enabled)
{
    (void)enabled;
    if (s_dim_overlay) {
        set_obj_hidden(s_dim_overlay, true);
    }
}

void display_lvgl_backend_release_album_resources(void)
{
    if (!s_display) {
        return;
    }

    lvgl_port_lock(0);
    release_all_album_tile_images();
    lvgl_port_unlock();
}

bool display_lvgl_backend_has_pending_album_thumbnail_work(void)
{
    return s_album_pending_thumbnail_work;
}

void display_lvgl_backend_render_state(const ephoto_app_state_t *state,
                                       const ephoto_settings_t *effective_settings,
                                       const void *photo_pixels,
                                       bool photo_ready,
                                       bool photo_loading,
                                       const char *clock_text)
{
    if (!s_display || !state || !effective_settings) {
        return;
    }

    lvgl_port_lock(0);
    update_photo(state, effective_settings, photo_pixels, photo_ready);
    update_clock(effective_settings,
                 clock_text,
                 effective_settings->clock_visible && effective_settings->screen_on &&
                     clock_text && clock_text[0] &&
                     !state->menu_visible && !state->album.visible);
    update_notification(state, photo_loading);
    update_startup_overlay(state);
    update_cache_overlay(state);
    update_menu(state);
    update_album(state);
    update_ota_overlay(state);
    lvgl_port_unlock();
}
