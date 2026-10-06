#include "gallery_service.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage_service.h"

static const char *TAG = "gallery_service";
static SemaphoreHandle_t s_mutex;
static ephoto_photo_t *s_items;
static size_t s_count;
static int *s_random_order;
static uint32_t s_revision;
static size_t s_total_image_candidates;
static size_t s_unsupported_png_count;
static size_t s_unsupported_progressive_jpeg_count;
static size_t s_unsupported_other_count;

#define PHOTO_SNIFF_HEADER_SIZE 4096
#define PHOTO_ORIENTATION_AUTO  0U

static bool is_jpeg_file(const char *name);

typedef struct {
    gallery_service_scan_progress_cb_t callback;
    void *context;
    size_t completed;
    size_t total;
} gallery_scan_progress_t;

static void report_scan_progress(gallery_scan_progress_t *progress)
{
    if (!progress || !progress->callback) {
        return;
    }
    progress->callback(progress->context, progress->completed, progress->total);
}

static void advance_scan_progress(gallery_scan_progress_t *progress)
{
    if (!progress) {
        return;
    }
    progress->completed += 1U;
    report_scan_progress(progress);
}

static uint64_t fnv1a64_update(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint16_t normalize_rotation_deg(int rotation_deg)
{
    int normalized = rotation_deg % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    switch (normalized) {
    case 90:
    case 180:
    case 270:
        return (uint16_t)normalized;
    default:
        return 0;
    }
}

static uint16_t exif_orientation_to_rotation_deg(uint16_t exif_orientation)
{
    switch (exif_orientation) {
    case 3:
        return 180;
    case 6:
        return 90;
    case 8:
        return 270;
    default:
        return 0;
    }
}

static void apply_photo_effective_orientation(ephoto_photo_t *item)
{
    if (!item) {
        return;
    }

    uint16_t raw_width = item->raw_width ? item->raw_width : item->width;
    uint16_t raw_height = item->raw_height ? item->raw_height : item->height;
    uint16_t exif_rotation = exif_orientation_to_rotation_deg(item->exif_orientation);
    uint16_t manual_rotation = normalize_rotation_deg(item->manual_rotation_deg);
    uint16_t effective_rotation = normalize_rotation_deg((int)exif_rotation + (int)manual_rotation);

    item->effective_rotation_deg = effective_rotation;
    if (effective_rotation == 90 || effective_rotation == 270) {
        item->width = raw_height;
        item->height = raw_width;
    } else {
        item->width = raw_width;
        item->height = raw_height;
    }
}

static esp_err_t build_photo_meta_dir(char *out_path, size_t out_len)
{
    const char *mount_path = storage_service_get_mount_path();
    if (!out_path || out_len == 0 || !mount_path || !mount_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(out_path, out_len, "%s/.ephoto_meta", mount_path) <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static uint64_t photo_meta_key(const char *path)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a64_update(hash, path, strlen(path));
    return hash;
}

// Kept only for clearing metadata written by firmware released before the
// stable path-based key was introduced.
static uint64_t legacy_photo_meta_key(const char *path, size_t size_bytes, time_t mtime)
{
    uint64_t hash = photo_meta_key(path);
    hash = fnv1a64_update(hash, &size_bytes, sizeof(size_bytes));
    hash = fnv1a64_update(hash, &mtime, sizeof(mtime));
    return hash;
}

static esp_err_t build_photo_meta_path_from_key(uint64_t key, char *out_path, size_t out_len)
{
    char meta_dir[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    if (!out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(build_photo_meta_dir(meta_dir, sizeof(meta_dir)), TAG, "meta dir path failed");
    if (snprintf(out_path, out_len, "%s/%016llx.rot", meta_dir, (unsigned long long)key) <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t build_photo_meta_path(const ephoto_photo_t *item, char *out_path, size_t out_len)
{
    if (!item || !item->path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return build_photo_meta_path_from_key(photo_meta_key(item->path), out_path, out_len);
}

static esp_err_t build_legacy_photo_meta_path(const ephoto_photo_t *item, char *out_path, size_t out_len)
{
    if (!item || !item->path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return build_photo_meta_path_from_key(legacy_photo_meta_key(item->path, item->size_bytes, item->mtime),
                                          out_path,
                                          out_len);
}

static bool read_photo_manual_rotation(const char *meta_path, uint16_t *out_rotation)
{
    unsigned long stored = 0;
    FILE *file = fopen(meta_path, "r");
    if (!file) {
        return false;
    }
    bool valid = fscanf(file, "%lu", &stored) == 1 && normalize_rotation_deg((int)stored) != 0;
    fclose(file);
    if (valid && out_rotation) {
        *out_rotation = normalize_rotation_deg((int)stored);
    }
    return valid;
}

static bool build_photo_meta_sidecar_path(const char *meta_path,
                                          const char *suffix,
                                          char *out_path,
                                          size_t out_len)
{
    int len = snprintf(out_path, out_len, "%s%s", meta_path, suffix);
    return len > 0 && len < (int)out_len;
}

static void recover_photo_meta_backup(const char *meta_path)
{
    char backup_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    if (!build_photo_meta_sidecar_path(meta_path, ".backup", backup_path, sizeof(backup_path)) ||
        access(backup_path, F_OK) != 0) {
        return;
    }

    // FatFs cannot rename over an existing target. A leftover backup means a
    // previous rotation update was interrupted: finish it or restore the old value.
    if (access(meta_path, F_OK) == 0) {
        unlink(backup_path);
    } else if (rename(backup_path, meta_path) != 0) {
        ESP_LOGW(TAG, "failed to restore rotation metadata backup: %s", backup_path);
    }
}

static void load_photo_manual_rotation(ephoto_photo_t *item)
{
    char meta_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};

    if (!item || !is_jpeg_file(item->path) || build_photo_meta_path(item, meta_path, sizeof(meta_path)) != ESP_OK) {
        return;
    }

    recover_photo_meta_backup(meta_path);
    if (read_photo_manual_rotation(meta_path, &item->manual_rotation_deg)) {
        return;
    }

    // Older firmware incorporated FAT mtime into the key. It is not stable
    // across remounts, but this fallback preserves rotations when it is.
    if (build_legacy_photo_meta_path(item, meta_path, sizeof(meta_path)) == ESP_OK) {
        (void)read_photo_manual_rotation(meta_path, &item->manual_rotation_deg);
    }
}

static esp_err_t save_photo_manual_rotation(const ephoto_photo_t *item, uint16_t manual_rotation_deg)
{
    char meta_dir[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    char meta_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    char temp_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    char backup_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    if (!item) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_jpeg_file(item->path)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(build_photo_meta_dir(meta_dir, sizeof(meta_dir)), TAG, "meta dir path failed");
    if (mkdir(meta_dir, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(build_photo_meta_path(item, meta_path, sizeof(meta_path)), TAG, "meta path failed");

    uint16_t normalized = normalize_rotation_deg(manual_rotation_deg);
    if (normalized == PHOTO_ORIENTATION_AUTO) {
        unlink(meta_path);
        char backup_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
        if (build_photo_meta_sidecar_path(meta_path, ".backup", backup_path, sizeof(backup_path))) {
            unlink(backup_path);
        }
        if (build_legacy_photo_meta_path(item, meta_path, sizeof(meta_path)) == ESP_OK) {
            unlink(meta_path);
        }
        return ESP_OK;
    }

    if (!build_photo_meta_sidecar_path(meta_path, ".tmp", temp_path, sizeof(temp_path)) ||
        !build_photo_meta_sidecar_path(meta_path, ".backup", backup_path, sizeof(backup_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(temp_path, "w");
    if (!file) {
        return ESP_FAIL;
    }
    bool write_failed = fprintf(file, "%u\n", (unsigned)normalized) < 0 ||
                        fflush(file) != 0 ||
                        fsync(fileno(file)) != 0;
    if (fclose(file) != 0) {
        write_failed = true;
    }
    if (write_failed) {
        unlink(temp_path);
        return ESP_FAIL;
    }
    bool had_previous_value = access(meta_path, F_OK) == 0;
    if (had_previous_value) {
        unlink(backup_path);
        if (rename(meta_path, backup_path) != 0) {
            unlink(temp_path);
            return ESP_FAIL;
        }
    }
    if (rename(temp_path, meta_path) != 0) {
        if (had_previous_value) {
            (void)rename(backup_path, meta_path);
        }
        unlink(temp_path);
        return ESP_FAIL;
    }
    if (had_previous_value) {
        unlink(backup_path);
    }
    return ESP_OK;
}

static void remove_photo_manual_rotation_artifact(const ephoto_photo_t *item)
{
    char meta_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
    if (!item) {
        return;
    }
    if (build_photo_meta_path(item, meta_path, sizeof(meta_path)) == ESP_OK) {
        unlink(meta_path);
        char backup_path[EPHOTO_MAX_PHOTO_PATH_LEN] = {0};
        if (build_photo_meta_sidecar_path(meta_path, ".backup", backup_path, sizeof(backup_path))) {
            unlink(backup_path);
        }
    }
    if (build_legacy_photo_meta_path(item, meta_path, sizeof(meta_path)) == ESP_OK) {
        unlink(meta_path);
    }
}

static uint16_t read_be16(const uint8_t *ptr)
{
    return (uint16_t)(((uint16_t)ptr[0] << 8) | (uint16_t)ptr[1]);
}

static uint16_t read_le16(const uint8_t *ptr)
{
    return (uint16_t)(((uint16_t)ptr[1] << 8) | (uint16_t)ptr[0]);
}

static uint32_t read_be32(const uint8_t *ptr)
{
    return ((uint32_t)ptr[0] << 24) |
           ((uint32_t)ptr[1] << 16) |
           ((uint32_t)ptr[2] << 8) |
           (uint32_t)ptr[3];
}

static uint32_t read_le32_mem(const uint8_t *ptr)
{
    return ((uint32_t)ptr[3] << 24) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[1] << 8) |
           (uint32_t)ptr[0];
}

static uint32_t read_le32(const uint8_t *ptr)
{
    return ((uint32_t)ptr[3] << 24) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[1] << 8) |
           (uint32_t)ptr[0];
}

static int32_t read_le32s(const uint8_t *ptr)
{
    return (int32_t)read_le32(ptr);
}

static uint16_t read_tiff_u16(const uint8_t *ptr, bool little_endian)
{
    return little_endian ? read_le16(ptr) : read_be16(ptr);
}

static uint32_t read_tiff_u32(const uint8_t *ptr, bool little_endian)
{
    return little_endian ? read_le32_mem(ptr) : read_be32(ptr);
}

static uint16_t sniff_jpeg_exif_orientation(const uint8_t *segment, size_t segment_len)
{
    if (!segment || segment_len < 14) {
        return 1;
    }

    if (memcmp(segment, "Exif\0\0", 6) != 0) {
        return 1;
    }

    const uint8_t *tiff = segment + 6;
    size_t tiff_len = segment_len - 6;
    if (tiff_len < 8) {
        return 1;
    }

    bool little_endian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        little_endian = true;
    } else if (!(tiff[0] == 'M' && tiff[1] == 'M')) {
        return 1;
    }

    if (read_tiff_u16(&tiff[2], little_endian) != 0x002A) {
        return 1;
    }

    uint32_t ifd0_offset = read_tiff_u32(&tiff[4], little_endian);
    if (ifd0_offset + 2 > tiff_len) {
        return 1;
    }

    const uint8_t *ifd = tiff + ifd0_offset;
    uint16_t entry_count = read_tiff_u16(ifd, little_endian);
    if (ifd0_offset + 2U + (uint32_t)entry_count * 12U > tiff_len) {
        return 1;
    }

    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint8_t *entry = ifd + 2 + (size_t)i * 12U;
        uint16_t tag = read_tiff_u16(entry, little_endian);
        if (tag != 0x0112) {
            continue;
        }

        uint16_t type = read_tiff_u16(entry + 2, little_endian);
        uint32_t count = read_tiff_u32(entry + 4, little_endian);
        if (type != 3 || count < 1) {
            return 1;
        }

        uint16_t orientation = read_tiff_u16(entry + 8, little_endian);
        if (orientation >= 1 && orientation <= 8) {
            return orientation;
        }
        return 1;
    }

    return 1;
}

static bool sniff_png_dimensions(const uint8_t *header, size_t bytes_read, ephoto_photo_t *item)
{
    if (!header || !item || bytes_read < 24) {
        return false;
    }

    if (memcmp(header, "\x89PNG\r\n\x1A\n", 8) != 0) {
        return false;
    }

    item->raw_width = (uint16_t)read_be32(&header[16]);
    item->raw_height = (uint16_t)read_be32(&header[20]);
    return true;
}

static bool sniff_bmp_dimensions(const uint8_t *header, size_t bytes_read, ephoto_photo_t *item)
{
    if (!header || !item || bytes_read < 26) {
        return false;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        return false;
    }

    uint32_t dib_size = read_le32(&header[14]);
    if (dib_size < 40 || bytes_read < 38) {
        return false;
    }

    int32_t width = read_le32s(&header[18]);
    int32_t height = read_le32s(&header[22]);
    if (width <= 0 || height == 0) {
        return false;
    }

    uint32_t abs_height = (height < 0) ? (uint32_t)(-height) : (uint32_t)height;
    item->raw_width = (uint16_t)((uint32_t)width > UINT16_MAX ? UINT16_MAX : (uint32_t)width);
    item->raw_height = (uint16_t)(abs_height > UINT16_MAX ? UINT16_MAX : abs_height);
    return true;
}

static bool sniff_jpeg_dimensions(const uint8_t *header, size_t bytes_read, ephoto_photo_t *item)
{
    if (!header || !item || bytes_read < 4) {
        return false;
    }
    if (!(header[0] == 0xFF && header[1] == 0xD8)) {
        return false;
    }

    size_t pos = 2;
    uint16_t exif_orientation = 1;
    while (pos + 3 < bytes_read) {
        while (pos < bytes_read && header[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= bytes_read) {
            break;
        }

        uint8_t marker = header[pos++];
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        if (pos + 2 > bytes_read) {
            break;
        }

        uint16_t segment_len = read_be16(&header[pos]);
        pos += 2;
        if (segment_len < 2 || pos + segment_len - 2 > bytes_read) {
            break;
        }

        if (marker == 0xE1 && segment_len > 8) {
            exif_orientation = sniff_jpeg_exif_orientation(&header[pos], segment_len - 2);
        }

        if ((marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF)) {
            if (segment_len >= 7) {
                item->raw_height = read_be16(&header[pos + 1]);
                item->raw_width = read_be16(&header[pos + 3]);
                item->exif_orientation = exif_orientation;
                return true;
            }
            break;
        }

        pos += segment_len - 2;
    }

    return false;
}

static bool photo_matches_filter(const ephoto_photo_t *item, ephoto_orientation_filter_t filter)
{
    if (!item) {
        return false;
    }
    if (filter == EPHOTO_ORIENTATION_ALL || item->width == 0 || item->height == 0) {
        return true;
    }
    if (filter == EPHOTO_ORIENTATION_LANDSCAPE) {
        return item->width >= item->height;
    }
    if (filter == EPHOTO_ORIENTATION_PORTRAIT) {
        return item->height > item->width;
    }
    return true;
}

static void sniff_photo_dimensions(const char *path, ephoto_photo_t *item)
{
    if (!path || !item) {
        return;
    }

    item->exif_orientation = 1;
    item->manual_rotation_deg = 0;
    item->effective_rotation_deg = 0;

    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }

    uint8_t *header = malloc(PHOTO_SNIFF_HEADER_SIZE);
    if (!header) {
        fclose(file);
        return;
    }
    size_t bytes_read = fread(header, 1, PHOTO_SNIFF_HEADER_SIZE, file);
    if (sniff_jpeg_dimensions(header, bytes_read, item)) {
        apply_photo_effective_orientation(item);
        free(header);
        fclose(file);
        return;
    }

    if (sniff_png_dimensions(header, bytes_read, item)) {
        apply_photo_effective_orientation(item);
        free(header);
        fclose(file);
        return;
    }

    if (sniff_bmp_dimensions(header, bytes_read, item)) {
        apply_photo_effective_orientation(item);
        free(header);
        fclose(file);
        return;
    }

    free(header);
    fclose(file);
}

static bool is_jpeg_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0;
}

static bool is_png_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".png") == 0;
}

static bool is_bmp_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    return strcasecmp(ext, ".bmp") == 0;
}

static bool is_known_image_like_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    return is_jpeg_file(name) ||
           is_png_file(name) ||
           is_bmp_file(name) ||
           strcasecmp(ext, ".gif") == 0 ||
           strcasecmp(ext, ".webp") == 0 ||
           strcasecmp(ext, ".heic") == 0 ||
           strcasecmp(ext, ".heif") == 0;
}

static bool is_upload_temp_artifact(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }

    size_t len = strlen(name);
    return len > 10 && strcmp(name + len - 10, ".uploading") == 0;
}

static bool is_upload_backup_artifact(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    return len > 7 && strcmp(name + len - 7, ".backup") == 0;
}

static void recover_or_remove_upload_backup(const char *backup_path)
{
    static const char suffix[] = ".backup";
    char original_path[EPHOTO_MAX_PHOTO_PATH_LEN];
    size_t backup_len = backup_path ? strlen(backup_path) : 0;

    if (backup_len <= sizeof(suffix) - 1 || backup_len >= sizeof(original_path)) {
        return;
    }
    strlcpy(original_path, backup_path, sizeof(original_path));
    original_path[backup_len - (sizeof(suffix) - 1)] = '\0';

    // A power loss can occur after moving the old file aside but before the
    // replacement is committed. Restore the old file instead of deleting it.
    if (access(original_path, F_OK) != 0) {
        if (rename(backup_path, original_path) == 0) {
            ESP_LOGW(TAG, "restored interrupted upload backup: %s", original_path);
        } else {
            ESP_LOGW(TAG, "failed to restore interrupted upload backup: %s", backup_path);
        }
        return;
    }

    if (unlink(backup_path) == 0) {
        ephoto_photo_t replacement = {0};
        strlcpy(replacement.path, original_path, sizeof(replacement.path));
        // The replacement is a different image at the same path, so it must
        // not inherit the previous image's manual rotation.
        remove_photo_manual_rotation_artifact(&replacement);
        ESP_LOGI(TAG, "removed committed upload backup: %s", backup_path);
    }
}

static bool path_starts_with(const char *path, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0 &&
           (path[prefix_len] == '\0' || path[prefix_len] == '/');
}

static bool path_equal_ignore_case(const char *lhs, const char *rhs)
{
    return strcasecmp(lhs, rhs) == 0;
}

static bool has_item_path(const ephoto_photo_t *items, size_t count, const char *path)
{
    for (size_t i = 0; i < count; ++i) {
        if (path_equal_ignore_case(items[i].path, path)) {
            return true;
        }
    }
    return false;
}

static bool path_exists_as_dir(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool root_already_added(const char *const *roots, size_t count, const char *path)
{
    for (size_t i = 0; i < count; ++i) {
        if (roots[i] && path_equal_ignore_case(roots[i], path)) {
            return true;
        }
    }
    return false;
}

static int compare_photo_asc(const void *lhs, const void *rhs)
{
    const ephoto_photo_t *a = lhs;
    const ephoto_photo_t *b = rhs;
    if (a->mtime < b->mtime) {
        return -1;
    }
    if (a->mtime > b->mtime) {
        return 1;
    }
    return strcasecmp(a->path, b->path);
}

static void rebuild_random_order_locked(void)
{
    free(s_random_order);
    s_random_order = NULL;
    if (s_count == 0) {
        return;
    }

    s_random_order = calloc(s_count, sizeof(int));
    if (!s_random_order) {
        return;
    }
    for (size_t i = 0; i < s_count; ++i) {
        s_random_order[i] = (int)i;
    }
    for (size_t i = s_count - 1; i > 0; --i) {
        uint32_t j = esp_random() % (i + 1);
        int tmp = s_random_order[i];
        s_random_order[i] = s_random_order[j];
        s_random_order[j] = tmp;
    }
}

static bool photo_lists_equal(const ephoto_photo_t *lhs,
                              size_t lhs_count,
                              const ephoto_photo_t *rhs,
                              size_t rhs_count)
{
    if (lhs_count != rhs_count) {
        return false;
    }

    for (size_t i = 0; i < lhs_count; ++i) {
        if (strcasecmp(lhs[i].path, rhs[i].path) != 0 ||
            lhs[i].mtime != rhs[i].mtime ||
            lhs[i].size_bytes != rhs[i].size_bytes ||
            lhs[i].raw_width != rhs[i].raw_width ||
            lhs[i].raw_height != rhs[i].raw_height ||
            lhs[i].width != rhs[i].width ||
            lhs[i].height != rhs[i].height ||
            lhs[i].exif_orientation != rhs[i].exif_orientation ||
            lhs[i].effective_rotation_deg != rhs[i].effective_rotation_deg ||
            lhs[i].manual_rotation_deg != rhs[i].manual_rotation_deg) {
            return false;
        }
    }

    return true;
}

static esp_err_t append_photo(ephoto_photo_t **items, size_t *count, const char *name, const char *path, const struct stat *st)
{
    if (has_item_path(*items, *count, path)) {
        return ESP_OK;
    }

    ephoto_photo_t *grown = realloc(*items, sizeof(ephoto_photo_t) * (*count + 1));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    *items = grown;
    memset(&grown[*count], 0, sizeof(ephoto_photo_t));
    strlcpy(grown[*count].name, name, sizeof(grown[*count].name));
    strlcpy(grown[*count].path, path, sizeof(grown[*count].path));
    grown[*count].size_bytes = st->st_size;
    grown[*count].mtime = st->st_mtime;
    sniff_photo_dimensions(path, &grown[*count]);
    load_photo_manual_rotation(&grown[*count]);
    apply_photo_effective_orientation(&grown[*count]);
    ++(*count);
    return ESP_OK;
}

static size_t count_scannable_files(const char *dir_path, int depth)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 0;
    }

    size_t total = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || strcasecmp(entry->d_name, ".ephoto_cache") == 0) {
            continue;
        }

        char full_path[EPHOTO_MAX_PHOTO_PATH_LEN];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                total += count_scannable_files(full_path, depth - 1);
            }
        } else if (S_ISREG(st.st_mode)) {
            total += 1U;
        }
    }

    closedir(dir);
    return total;
}

static esp_err_t scan_directory(const char *dir_path,
                                int depth,
                                ephoto_photo_t **items,
                                size_t *count,
                                gallery_scan_progress_t *progress)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (strcasecmp(entry->d_name, ".ephoto_cache") == 0) {
            continue;
        }

        char full_path[EPHOTO_MAX_PHOTO_PATH_LEN];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(full_path)) {
            ESP_LOGW(TAG, "skip path too long under %s: %s", dir_path, entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                esp_err_t err = scan_directory(full_path, depth - 1, items, count, progress);
                if (err == ESP_ERR_NO_MEM) {
                    closedir(dir);
                    return err;
                }
            }
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        // Count each regular file before parsing it so progress still moves
        // when an unsupported or damaged file is encountered.
        advance_scan_progress(progress);

        if (is_upload_backup_artifact(entry->d_name)) {
            recover_or_remove_upload_backup(full_path);
            continue;
        }

        if (is_upload_temp_artifact(entry->d_name)) {
            if (unlink(full_path) == 0) {
                ESP_LOGW(TAG, "removed stale upload temp file: %s", full_path);
            }
            continue;
        }

        if (st.st_size <= 0) {
            if (is_known_image_like_file(entry->d_name) && unlink(full_path) == 0) {
                ESP_LOGW(TAG, "removed empty image file: %s", full_path);
            } else {
                ESP_LOGW(TAG, "skip empty image candidate: %s", full_path);
            }
            continue;
        }

        if (!is_jpeg_file(entry->d_name) && !is_png_file(entry->d_name) && !is_bmp_file(entry->d_name)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".gif") == 0 ||
                        strcasecmp(ext, ".webp") == 0 ||
                        strcasecmp(ext, ".heic") == 0 ||
                        strcasecmp(ext, ".heif") == 0)) {
                ++s_total_image_candidates;
                ++s_unsupported_other_count;
            }
            continue;
        }

        ++s_total_image_candidates;

        esp_err_t err = append_photo(items, count, entry->d_name, full_path, &st);
        if (err != ESP_OK) {
            closedir(dir);
            return err;
        }
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t gallery_service_init(const board_profile_t *profile)
{
    (void)profile;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gallery_service_rescan(void)
{
    return gallery_service_rescan_with_progress(NULL, NULL);
}

esp_err_t gallery_service_rescan_with_progress(gallery_service_scan_progress_cb_t progress_cb, void *progress_ctx)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *mount_path = storage_service_get_mount_path();
    const char *photo_dir = storage_service_get_photo_dir();
    if (!mount_path[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    ephoto_photo_t *new_items = NULL;
    size_t new_count = 0;
    s_total_image_candidates = 0;
    s_unsupported_png_count = 0;
    s_unsupported_progressive_jpeg_count = 0;
    s_unsupported_other_count = 0;

    char dcim_dir[EPHOTO_MAX_PHOTO_PATH_LEN];
    char pictures_dir[EPHOTO_MAX_PHOTO_PATH_LEN];
    char photos_dir[EPHOTO_MAX_PHOTO_PATH_LEN];
    snprintf(dcim_dir, sizeof(dcim_dir), "%s/DCIM", mount_path);
    snprintf(pictures_dir, sizeof(pictures_dir), "%s/Pictures", mount_path);
    snprintf(photos_dir, sizeof(photos_dir), "%s/Photos", mount_path);

    const char *candidate_roots[] = {
        photo_dir,
        dcim_dir,
        pictures_dir,
        photos_dir,
    };
    const char *scan_roots[sizeof(candidate_roots) / sizeof(candidate_roots[0])] = {0};
    size_t scan_root_count = 0;

    for (size_t i = 0; i < sizeof(candidate_roots) / sizeof(candidate_roots[0]); ++i) {
        const char *root = candidate_roots[i];
        if (!path_exists_as_dir(root) || root_already_added(scan_roots, scan_root_count, root)) {
            continue;
        }
        scan_roots[scan_root_count++] = root;
    }

    if (scan_root_count == 0) {
        scan_roots[scan_root_count++] = mount_path;
    }

    gallery_scan_progress_t progress = {
        .callback = progress_cb,
        .context = progress_ctx,
    };
    for (size_t i = 0; i < scan_root_count; ++i) {
        progress.total += count_scannable_files(scan_roots[i], 2);
    }
    report_scan_progress(&progress);

    for (size_t i = 0; i < scan_root_count; ++i) {
        esp_err_t err = scan_directory(scan_roots[i], 2, &new_items, &new_count, &progress);
        if (err == ESP_ERR_NO_MEM) {
            free(new_items);
            return err;
        }
    }

    qsort(new_items, new_count, sizeof(ephoto_photo_t), compare_photo_asc);

    bool changed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    changed = !photo_lists_equal(s_items, s_count, new_items, new_count);
    if (changed) {
        free(s_items);
        s_items = new_items;
        s_count = new_count;
        ++s_revision;
        rebuild_random_order_locked();
        new_items = NULL;
    }
    xSemaphoreGive(s_mutex);

    free(new_items);
    progress.completed = progress.total;
    report_scan_progress(&progress);

    ESP_LOGI(TAG,
             "rescanned %u supported photos from %s (changed=%d candidates=%u png=%u progressive_jpeg=%u other=%u)",
             (unsigned)new_count,
             mount_path,
             changed,
             (unsigned)s_total_image_candidates,
             (unsigned)s_unsupported_png_count,
             (unsigned)s_unsupported_progressive_jpeg_count,
             (unsigned)s_unsupported_other_count);
    return ESP_OK;
}

int gallery_service_get_count(void)
{
    if (!s_mutex) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = (int)s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

int gallery_service_get_count_filtered(ephoto_orientation_filter_t filter)
{
    if (!s_mutex) {
        return 0;
    }
    int count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_count; ++i) {
        if (photo_matches_filter(&s_items[i], filter)) {
            ++count;
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}

uint32_t gallery_service_get_revision(void)
{
    if (!s_mutex) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t revision = s_revision;
    xSemaphoreGive(s_mutex);
    return revision;
}

size_t gallery_service_get_total_image_candidates(void)
{
    return s_total_image_candidates;
}

size_t gallery_service_get_unsupported_png_count(void)
{
    return s_unsupported_png_count;
}

size_t gallery_service_get_unsupported_progressive_jpeg_count(void)
{
    return s_unsupported_progressive_jpeg_count;
}

size_t gallery_service_get_unsupported_other_count(void)
{
    return s_unsupported_other_count;
}

esp_err_t gallery_service_get_item(int index, ephoto_photo_t *out_item)
{
    if (!s_mutex || index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if ((size_t)index >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out_item = s_items[index];
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t gallery_service_get_all(ephoto_photo_t **out_items, size_t *out_count)
{
    if (!s_mutex || !out_items || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count == 0) {
        *out_items = NULL;
        *out_count = 0;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    ephoto_photo_t *copy = calloc(s_count, sizeof(ephoto_photo_t));
    if (!copy) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_items, sizeof(ephoto_photo_t) * s_count);
    *out_items = copy;
    *out_count = s_count;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t gallery_service_get_range(size_t offset,
                                    size_t limit,
                                    ephoto_photo_t **out_items,
                                    size_t *out_count,
                                    size_t *out_total)
{
    if (!s_mutex || !out_items || !out_count || !out_total) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_items = NULL;
    *out_count = 0;
    *out_total = 0;
    if (limit == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_total = s_count;
    if (offset >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    size_t count = s_count - offset;
    if (count > limit) {
        count = limit;
    }
    ephoto_photo_t *copy = calloc(count, sizeof(ephoto_photo_t));
    if (!copy) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, &s_items[offset], sizeof(ephoto_photo_t) * count);
    *out_items = copy;
    *out_count = count;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void gallery_service_release_all(ephoto_photo_t *items)
{
    free(items);
}

int gallery_service_find_index_by_path(const char *path)
{
    if (!s_mutex || !path || !path[0]) {
        return -1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].path, path) == 0) {
            xSemaphoreGive(s_mutex);
            return (int)i;
        }
    }
    xSemaphoreGive(s_mutex);
    return -1;
}

int gallery_service_step_index(int current_index, ephoto_playback_mode_t mode, ephoto_orientation_filter_t filter, int delta)
{
    if (!s_mutex || delta == 0) {
        return current_index;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    int resolved = current_index;
    if (resolved < 0 || (size_t)resolved >= s_count) {
        resolved = 0;
    }

    if (!photo_matches_filter(&s_items[resolved], filter)) {
        resolved = -1;
    }

    int next = resolved;
    for (size_t attempts = 0; attempts < s_count; ++attempts) {
        if (mode == EPHOTO_PLAYBACK_TIME_ASC) {
            next = (next < 0) ? 0 : (next + delta + (int)s_count) % (int)s_count;
        } else if (mode == EPHOTO_PLAYBACK_TIME_DESC) {
            next = (next < 0) ? ((int)s_count - 1) : (next - delta + (int)s_count) % (int)s_count;
        } else {
            int pos = 0;
            if (next >= 0) {
                for (size_t i = 0; i < s_count; ++i) {
                    if (s_random_order && s_random_order[i] == next) {
                        pos = (int)i;
                        break;
                    }
                }
                pos = (pos + delta + (int)s_count) % (int)s_count;
            }
            next = s_random_order ? s_random_order[pos] : 0;
        }

        if (photo_matches_filter(&s_items[next], filter)) {
            resolved = next;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return resolved;
}

static esp_err_t update_photo_manual_rotation_locked(const char *path,
                                                     uint16_t next_manual_rotation,
                                                     ephoto_photo_t *out_item)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_jpeg_file(path)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].path, path) != 0) {
            continue;
        }

        next_manual_rotation = normalize_rotation_deg(next_manual_rotation);
        esp_err_t err = save_photo_manual_rotation(&s_items[i], next_manual_rotation);
        if (err != ESP_OK) {
            return err;
        }

        s_items[i].manual_rotation_deg = next_manual_rotation;
        apply_photo_effective_orientation(&s_items[i]);
        ++s_revision;
        if (out_item) {
            *out_item = s_items[i];
        }
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t gallery_service_set_photo_manual_rotation_by_path(const char *path,
                                                            uint16_t manual_rotation_deg,
                                                            ephoto_photo_t *out_item)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = update_photo_manual_rotation_locked(path, manual_rotation_deg, out_item);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t gallery_service_reset_photo_orientation_by_path(const char *path, ephoto_photo_t *out_item)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = update_photo_manual_rotation_locked(path, PHOTO_ORIENTATION_AUTO, out_item);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t gallery_service_delete_by_path(const char *path)
{
    const char *mount_path = storage_service_get_mount_path();
    if (!path || !path[0] || !mount_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!path_starts_with(path, mount_path) ||
        (!is_jpeg_file(path) && !is_png_file(path) && !is_bmp_file(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    ephoto_photo_t deleting = {0};
    bool have_item = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (size_t i = 0; i < s_count; ++i) {
            if (strcmp(s_items[i].path, path) == 0) {
                deleting = s_items[i];
                have_item = true;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
    }
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "unlink failed for %s errno=%d", path, errno);
        return ESP_FAIL;
    }
    if (have_item) {
        remove_photo_manual_rotation_artifact(&deleting);
    }
    return ESP_OK;
}
