/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define BROOKESIA_LOG_TAG "FwUpdater"
#include "sdkconfig.h"
#include "fw_updater.hpp"

#if CONFIG_EXAMPLE_FW_UPDATE_ENABLE

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "boost/json.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_manager.hpp"
#include "brookesia/service_helper/network/wifi.hpp"
#include "sd_settings.hpp"

using namespace esp_brookesia;

using WifiHelper = service::helper::Wifi;

#define SD_SETTINGS_UPDATE_SERVER_URL_KEY "CONFIG_EXAMPLE_FW_UPDATE_SERVER_URL"

namespace {

constexpr const char *SD_VERSION_FILE_PATH = "/sdcard/VERSION";
constexpr const char *SD_FW_DOWNLOAD_PATH = "/sdcard/update/firmware.bin";
constexpr size_t HTTP_READ_CHUNK_SIZE = 4096;
constexpr size_t MANIFEST_MAX_SIZE = 4 * 1024;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

/**
 * @brief RAII wrapper that closes and frees an `esp_http_client_handle_t` on scope exit.
 */
struct HttpClientGuard {
    esp_http_client_handle_t handle = nullptr;

    ~HttpClientGuard()
    {
        if (handle) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
        }
    }
};

struct FirmwareManifest {
    long version = 0;
    std::string md5;
};

std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return value;
}

std::string to_hex(const uint8_t *digest, size_t len)
{
    static constexpr char HEX_CHARS[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        hex.push_back(HEX_CHARS[digest[i] >> 4]);
        hex.push_back(HEX_CHARS[digest[i] & 0x0F]);
    }
    return hex;
}

long read_local_version()
{
    std::ifstream file(SD_VERSION_FILE_PATH);
    if (!file.is_open()) {
        BROOKESIA_LOGI("No VERSION file on SD card, initializing it with 0");
        std::ofstream out_file(SD_VERSION_FILE_PATH, std::ios::binary | std::ios::trunc);
        if (out_file.is_open()) {
            out_file << 0;
        } else {
            BROOKESIA_LOGW("Failed to create VERSION file at %1%", SD_VERSION_FILE_PATH);
        }
        return 0;
    }

    std::string content;
    std::getline(file, content);
    try {
        return content.empty() ? 0 : std::stol(content);
    } catch (const std::exception &) {
        BROOKESIA_LOGW("Invalid content in VERSION file, treating installed version as 0");
        return 0;
    }
}

bool write_local_version(long version)
{
    std::ofstream out_file(SD_VERSION_FILE_PATH, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
        BROOKESIA_LOGE("Failed to update VERSION file at %1%", SD_VERSION_FILE_PATH);
        return false;
    }
    out_file << version;
    return static_cast<bool>(out_file);
}

std::optional<FirmwareManifest> fetch_manifest(const std::string &base_url)
{
    const std::string url = base_url + "/firmware";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = HTTP_TIMEOUT_MS;

    HttpClientGuard guard;
    guard.handle = esp_http_client_init(&config);
    if (!guard.handle) {
        BROOKESIA_LOGE("Failed to init HTTP client for %1%", url);
        return std::nullopt;
    }

    esp_http_client_set_method(guard.handle, HTTP_METHOD_GET);
    if (esp_http_client_open(guard.handle, 0) != ESP_OK) {
        BROOKESIA_LOGE("Failed to open request: %1%", url);
        return std::nullopt;
    }
    if (esp_http_client_fetch_headers(guard.handle) < 0) {
        BROOKESIA_LOGE("Failed to fetch response headers: %1%", url);
        return std::nullopt;
    }
    const int status_code = esp_http_client_get_status_code(guard.handle);
    if (status_code != 200) {
        BROOKESIA_LOGE("Request failed with status %1%: %2%", status_code, url);
        return std::nullopt;
    }

    std::string body;
    std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
    int read_len;
    while ((read_len = esp_http_client_read(guard.handle, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
        body.append(chunk.data(), read_len);
        if (body.size() > MANIFEST_MAX_SIZE) {
            BROOKESIA_LOGE("Firmware manifest response too large");
            return std::nullopt;
        }
    }

    boost::system::error_code parse_ec;
    auto parsed = boost::json::parse(body, parse_ec);
    if (parse_ec || !parsed.is_object()) {
        BROOKESIA_LOGE("Failed to parse firmware manifest response: %1%", body);
        return std::nullopt;
    }
    const auto &obj = parsed.as_object();
    const auto *version_value = obj.if_contains("version");
    const auto *md5_value = obj.if_contains("md5");
    if ((version_value == nullptr) || (md5_value == nullptr) || !md5_value->is_string()) {
        BROOKESIA_LOGE("Firmware manifest response is missing \"version\"/\"md5\": %1%", body);
        return std::nullopt;
    }

    FirmwareManifest manifest;
    try {
        if (version_value->is_string()) {
            manifest.version = std::stol(std::string(version_value->as_string()));
        } else if (version_value->is_int64()) {
            manifest.version = static_cast<long>(version_value->as_int64());
        } else if (version_value->is_uint64()) {
            manifest.version = static_cast<long>(version_value->as_uint64());
        } else {
            BROOKESIA_LOGE("Firmware manifest \"version\" has an unsupported type");
            return std::nullopt;
        }
    } catch (const std::exception &) {
        BROOKESIA_LOGE("Firmware manifest \"version\" is not a valid number: %1%", body);
        return std::nullopt;
    }
    manifest.md5 = to_lower(std::string(md5_value->as_string()));

    return manifest;
}

// Downloads the firmware image for `version` into `dest_path`, verifying its MD5 against
// `expected_md5` before the temp file is renamed into place.
bool download_firmware(
    const std::string &base_url, long version, const std::string &expected_md5,
    const std::filesystem::path &dest_path)
{
    std::error_code fs_ec;
    std::filesystem::create_directories(dest_path.parent_path(), fs_ec);
    if (fs_ec) {
        BROOKESIA_LOGE("Failed to create directory for firmware download: %1%", fs_ec.message());
        return false;
    }

    const std::string url = base_url + "/firmware/" + std::to_string(version);

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = HTTP_TIMEOUT_MS;

    HttpClientGuard guard;
    guard.handle = esp_http_client_init(&config);
    if (!guard.handle) {
        BROOKESIA_LOGE("Failed to init HTTP client to download firmware version %1%", version);
        return false;
    }

    esp_http_client_set_method(guard.handle, HTTP_METHOD_GET);
    if (esp_http_client_open(guard.handle, 0) != ESP_OK) {
        BROOKESIA_LOGE("Failed to open firmware download request: %1%", url);
        return false;
    }
    if (esp_http_client_fetch_headers(guard.handle) < 0) {
        BROOKESIA_LOGE("Failed to fetch firmware download response headers: %1%", url);
        return false;
    }
    const int status_code = esp_http_client_get_status_code(guard.handle);
    if (status_code != 200) {
        BROOKESIA_LOGE("Firmware download failed with status %1%: %2%", status_code, url);
        return false;
    }

    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);

    // Download into a temp file and rename into place, so a failed/interrupted download never
    // leaves a truncated file at `dest_path` that a later run would mistake for a good copy.
    const auto tmp_path = dest_path.string() + ".tmp";
    {
        std::ofstream out_file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out_file.is_open()) {
            BROOKESIA_LOGE("Failed to open %1% for writing", tmp_path);
            return false;
        }

        std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
        int read_len;
        while ((read_len = esp_http_client_read(guard.handle, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
            esp_rom_md5_update(&md5_ctx, chunk.data(), static_cast<uint32_t>(read_len));
            out_file.write(chunk.data(), read_len);
            if (!out_file) {
                BROOKESIA_LOGE("Failed to write to %1%", tmp_path);
                return false;
            }
        }
    }

    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
    esp_rom_md5_final(digest, &md5_ctx);
    const std::string actual_md5 = to_hex(digest, ESP_ROM_MD5_DIGEST_LEN);

    if (actual_md5 != expected_md5) {
        BROOKESIA_LOGE("Downloaded firmware MD5 mismatch: expected %1%, got %2%", expected_md5, actual_md5);
        std::filesystem::remove(tmp_path, fs_ec);
        return false;
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp_path, dest_path, rename_ec);
    if (rename_ec) {
        BROOKESIA_LOGE("Failed to move downloaded firmware into place: %1%", rename_ec.message());
        return false;
    }

    return true;
}

// Flashes `src_path` into the given OTA `partition`, then reads the partition back from flash to
// verify it against `expected_md5` before the caller switches the bootloader to it.
bool flash_partition_from_file(
    const std::filesystem::path &src_path, const esp_partition_t *partition, const std::string &expected_md5)
{
    std::error_code fs_ec;
    const auto file_size = std::filesystem::file_size(src_path, fs_ec);
    if (fs_ec) {
        BROOKESIA_LOGE("Failed to stat downloaded firmware: %1%", fs_ec.message());
        return false;
    }
    if ((file_size == 0) || (file_size > partition->size)) {
        BROOKESIA_LOGE("Firmware size %1% does not fit in OTA partition of size %2%", file_size, partition->size);
        return false;
    }

    std::ifstream in_file(src_path, std::ios::binary);
    if (!in_file.is_open()) {
        BROOKESIA_LOGE("Failed to open downloaded firmware for reading: %1%", src_path.string());
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, file_size, &ota_handle);
    if (err != ESP_OK) {
        BROOKESIA_LOGE("esp_ota_begin failed: %1%", esp_err_to_name(err));
        return false;
    }

    std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
    bool write_failed = false;
    while (in_file) {
        in_file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read_len = in_file.gcount();
        if (read_len <= 0) {
            break;
        }
        err = esp_ota_write(ota_handle, chunk.data(), static_cast<size_t>(read_len));
        if (err != ESP_OK) {
            BROOKESIA_LOGE("esp_ota_write failed: %1%", esp_err_to_name(err));
            write_failed = true;
            break;
        }
    }

    if (write_failed) {
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        BROOKESIA_LOGE("esp_ota_end failed: %1%", esp_err_to_name(err));
        return false;
    }

    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);

    std::vector<uint8_t> read_buf(HTTP_READ_CHUNK_SIZE);
    size_t offset = 0;
    const size_t total_size = static_cast<size_t>(file_size);
    while (offset < total_size) {
        const size_t to_read = std::min(read_buf.size(), total_size - offset);
        err = esp_partition_read(partition, offset, read_buf.data(), to_read);
        if (err != ESP_OK) {
            BROOKESIA_LOGE("esp_partition_read failed while verifying flashed firmware: %1%", esp_err_to_name(err));
            return false;
        }
        esp_rom_md5_update(&md5_ctx, read_buf.data(), static_cast<uint32_t>(to_read));
        offset += to_read;
    }

    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
    esp_rom_md5_final(digest, &md5_ctx);
    const std::string actual_md5 = to_hex(digest, ESP_ROM_MD5_DIGEST_LEN);

    if (actual_md5 != expected_md5) {
        BROOKESIA_LOGE("Flashed firmware MD5 mismatch: expected %1%, got %2%", expected_md5, actual_md5);
        return false;
    }

    return true;
}

} // namespace

bool FwUpdater::init(const Config &config)
{
    BROOKESIA_CHECK_FALSE_RETURN(!is_initialized_.load(), false, "Already initialized");
    BROOKESIA_CHECK_FALSE_RETURN(config.task_scheduler != nullptr, false, "Invalid task scheduler");

    config_ = config;
    is_initialized_.store(true);

    return true;
}

bool FwUpdater::start()
{
    BROOKESIA_CHECK_FALSE_RETURN(is_initialized_.load(), false, "Not initialized");
    BROOKESIA_CHECK_FALSE_RETURN(WifiHelper::is_available(), false, "WiFi service not available");

    auto post_run_update_check = [this]() {
        active_conn_.reset();
        run_update_check();
    };

    auto on_general_event = [this, post_run_update_check](const std::string &, const std::string &event, bool) {
        if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Connected)) {
            BROOKESIA_LOGI("WiFi connected, checking for firmware updates");
            config_.task_scheduler->post(post_run_update_check);
        }
    };

    auto conn = WifiHelper::subscribe_event(WifiHelper::EventId::GeneralEventHappened, on_general_event);
    active_conn_ = std::make_shared<service::EventRegistry::SignalConnection>(std::move(conn));

    return true;
}

void FwUpdater::run_update_check()
{
    if (has_run_.exchange(true)) {
        return;
    }

    std::string base_url = CONFIG_EXAMPLE_FW_UPDATE_SERVER_URL;
    if (auto override_url = SdSettings::get_instance().get_string(SD_SETTINGS_UPDATE_SERVER_URL_KEY);
        override_url.has_value()) {
        BROOKESIA_LOGI("Overriding firmware update server URL from SD settings: %1%", *override_url);
        base_url = std::move(*override_url);
    }

    BROOKESIA_LOGI("Checking for firmware updates from %1%", base_url);

    auto manifest = fetch_manifest(base_url);
    if (!manifest) {
        BROOKESIA_LOGW("Failed to fetch firmware manifest, skipping update");
        return;
    }

    const long local_version = read_local_version();
    BROOKESIA_LOGI("Installed firmware version: %1%, available version: %2%", local_version, manifest->version);

    if (manifest->version <= local_version) {
        BROOKESIA_LOGI("Firmware is up to date");
        return;
    }

    const std::filesystem::path fw_path(SD_FW_DOWNLOAD_PATH);

    BROOKESIA_LOGI("Downloading firmware version %1%", manifest->version);
    if (!download_firmware(base_url, manifest->version, manifest->md5, fw_path)) {
        BROOKESIA_LOGE("Failed to download firmware version %1%", manifest->version);
        return;
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        BROOKESIA_LOGE("No OTA partition available to receive the new firmware");
        return;
    }
    BROOKESIA_LOGI(
        "Flashing firmware version %1% into partition \"%2%\" (currently running \"%3%\")", manifest->version,
        update_partition->label, running_partition ? running_partition->label : "?");

    if (!flash_partition_from_file(fw_path, update_partition, manifest->md5)) {
        BROOKESIA_LOGE("Failed to flash firmware version %1%", manifest->version);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        BROOKESIA_LOGE("esp_ota_set_boot_partition failed: %1%", esp_err_to_name(err));
        return;
    }

    if (!write_local_version(manifest->version)) {
        BROOKESIA_LOGW(
            "Failed to persist new firmware version to %1%, update will be retried next boot", SD_VERSION_FILE_PATH);
    }

    std::error_code fs_ec;
    std::filesystem::remove(fw_path, fs_ec);

    BROOKESIA_LOGI("Firmware version %1% installed, restarting", manifest->version);
    esp_restart();
}

#else // !CONFIG_EXAMPLE_FW_UPDATE_ENABLE

bool FwUpdater::init(const Config &config)
{
    config_ = config;
    is_initialized_.store(true);
    return true;
}

bool FwUpdater::start()
{
    return true;
}

void FwUpdater::run_update_check() {}

#endif // CONFIG_EXAMPLE_FW_UPDATE_ENABLE
