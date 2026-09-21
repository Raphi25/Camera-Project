/* Validate names and expose daily summary files through command replies. */

#include "summary_commands.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device_settings.h"
#include "esp_err.h"
#include "sd_storage.h"

static bool is_valid_summary_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strncmp(name, "summary_", 8) != 0) {
        return false;
    }

    const char *ext = strrchr(name, '.');
    if (ext == NULL || strcasecmp(ext, ".txt") != 0) {
        return false;
    }

    for (size_t i = 0; name[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static bool is_orientation_stage_name(const char *name)
{
    if (name == NULL || strncmp(name, "summary_", 8) != 0) return false;
    const char *ext = strrchr(name, '.');
    return ext != NULL && strcasecmp(ext, ".orientation") == 0;
}

static bool require_sd(command_reply_t *reply, const char *command)
{
    esp_err_t err = sd_storage_require_mounted();
    if (err == ESP_OK) {
        return true;
    }
    command_reply_printf(reply, "ERR %s SD_NOT_READY %s\n", command, esp_err_to_name(err));
    return false;
}

static void trim_newline(char *text)
{
    size_t len = strlen(text);
    while (len > 0U && (text[len - 1U] == '\r' || text[len - 1U] == '\n')) {
        text[--len] = '\0';
    }
}

int summary_commands_count(void)
{
    if (!sd_storage_is_mounted()) {
        return -1;
    }

    sd_storage_lock();
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        return -1;
    }

    int count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (is_valid_summary_name(entry->d_name)) {
            count++;
        }
    }

    closedir(dir);
    sd_storage_unlock();
    return count;
}

void summary_command_list(app_context_t *ctx, char *cmd, const char *args,
                          command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    (void)args;
    if (!require_sd(reply, "SUMMARY_LIST")) {
        return;
    }

    sd_storage_lock();
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        command_reply_printf(reply, "ERR SUMMARY_LIST open_dir_failed\n");
        return;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_valid_summary_name(entry->d_name)) {
            continue;
        }

        char path[320];
        int length = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name);
        struct stat info = {0};
        if (length < 0 || length >= (int)sizeof(path) || stat(path, &info) != 0) {
            continue;
        }
        command_reply_printf(reply, "SUMMARY %s %u\n",
                             entry->d_name, (unsigned)info.st_size);
    }

    closedir(dir);
    sd_storage_unlock();
    command_reply_printf(reply, "OK SUMMARY_LIST\n");
}

void summary_command_get(app_context_t *ctx, char *cmd, const char *args,
                         command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    if (!is_valid_summary_name(args)) {
        command_reply_printf(reply, "ERR SUMMARY_GET invalid_name\n");
        return;
    }
    if (!require_sd(reply, "SUMMARY_GET")) {
        return;
    }

    char path[320];
    int length = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", args);
    if (length < 0 || length >= (int)sizeof(path)) {
        command_reply_printf(reply, "ERR SUMMARY_GET path_too_long\n");
        return;
    }

    sd_storage_lock();
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        sd_storage_unlock();
        command_reply_printf(reply, "ERR SUMMARY_GET open_failed\n");
        return;
    }

    command_reply_printf(reply, "BEGIN_SUMMARY %s\n", args);
    char line[160];
    while (fgets(line, sizeof(line), file) != NULL) {
        trim_newline(line);
        command_reply_printf(reply, "TEXT %s\n", line);
    }
    fclose(file);
    sd_storage_unlock();
    command_reply_printf(reply, "END_SUMMARY %s\n", args);
}

void summary_command_delete_all(app_context_t *ctx, char *cmd, const char *args,
                                command_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    (void)args;
    if (!require_sd(reply, "SUMMARY_DELETE_ALL")) {
        return;
    }

    sd_storage_lock();
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        command_reply_printf(reply, "ERR SUMMARY_DELETE_ALL open_dir_failed\n");
        return;
    }

    int deleted = 0;
    int failed = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_valid_summary_name(entry->d_name) &&
            !is_orientation_stage_name(entry->d_name)) {
            continue;
        }

        char path[320];
        int length = snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name);
        if (length < 0 || length >= (int)sizeof(path) || unlink(path) != 0) {
            failed++;
        } else {
            deleted++;
        }
    }

    closedir(dir);
    sd_storage_unlock();
    if (failed > 0) {
        command_reply_printf(reply,
                             "ERR SUMMARY_DELETE_ALL partial deleted=%d failed=%d\n",
                             deleted, failed);
        return;
    }
    command_reply_printf(reply, "OK SUMMARY_DELETE_ALL deleted=%d\n", deleted);
}
