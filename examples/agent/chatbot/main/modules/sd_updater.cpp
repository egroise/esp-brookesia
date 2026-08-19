/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define BROOKESIA_LOG_TAG "SdUpdater"
#include "sdkconfig.h"
#include "sd_updater.hpp"

#if CONFIG_EXAMPLE_SD_UPDATE_ENABLE

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "esp_http_client.h"
#include "esp_rom_md5.h"
#include "boost/json.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_manager.hpp"
#include "brookesia/service_helper/network/wifi.hpp"
#include "sd_settings.hpp"

using namespace esp_brookesia;

using WifiHelper = service::helper::Wifi;

#define SD_SETTINGS_UPDATE_SERVER_URL_KEY "CONFIG_EXAMPLE_SD_UPDATE_SERVER_URL"

namespace {

constexpr const char *SD_UPDATE_ROOT_DIR = "/sdcard/update";
constexpr size_t HTTP_READ_CHUNK_SIZE = 4096;
constexpr size_t FILE_LIST_MAX_SIZE = 64 * 1024;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;

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

std::optional<std::string> compute_file_md5(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    md5_context_t ctx;
    esp_rom_md5_init(&ctx);

    std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        auto read_len = file.gcount();
        if (read_len > 0) {
            esp_rom_md5_update(&ctx, chunk.data(), static_cast<uint32_t>(read_len));
        }
    }

    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
    esp_rom_md5_final(digest, &ctx);

    return to_hex(digest, ESP_ROM_MD5_DIGEST_LEN);
}

std::optional<boost::json::array> fetch_file_list(const std::string &base_url)
{
    const std::string url = base_url + "/update";

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
        if (body.size() > FILE_LIST_MAX_SIZE) {
            BROOKESIA_LOGE("Update file list response too large");
            return std::nullopt;
        }
    }

    boost::system::error_code parse_ec;
    auto parsed = boost::json::parse(body, parse_ec);
    if (parse_ec || !parsed.is_object()) {
        BROOKESIA_LOGE("Failed to parse update file list response: %1%", body);
        return std::nullopt;
    }
    const auto *files = parsed.as_object().if_contains("files");
    if ((files == nullptr) || !files->is_array()) {
        BROOKESIA_LOGE("Update file list response is missing a \"files\" array: %1%", body);
        return std::nullopt;
    }

    return files->as_array();
}

bool download_file(const std::string &base_url, const std::string &name, const std::filesystem::path &dest_path)
{
    std::error_code fs_ec;
    std::filesystem::create_directories(dest_path.parent_path(), fs_ec);
    if (fs_ec) {
        BROOKESIA_LOGE("Failed to create directory for %1%: %2%", name, fs_ec.message());
        return false;
    }

    boost::json::object request_body_obj;
    request_body_obj["file"] = name;
    const std::string request_body = boost::json::serialize(request_body_obj);

    const std::string url = base_url + "/update";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size_tx = 1024;

    HttpClientGuard guard;
    guard.handle = esp_http_client_init(&config);
    if (!guard.handle) {
        BROOKESIA_LOGE("Failed to init HTTP client to download %1%", name);
        return false;
    }

    esp_http_client_set_method(guard.handle, HTTP_METHOD_POST);
    esp_http_client_set_header(guard.handle, "Content-Type", "application/json");

    if (esp_http_client_open(guard.handle, static_cast<int>(request_body.size())) != ESP_OK) {
        BROOKESIA_LOGE("Failed to open download request for %1%", name);
        return false;
    }
    if (esp_http_client_write(guard.handle, request_body.data(), static_cast<int>(request_body.size())) < 0) {
        BROOKESIA_LOGE("Failed to write download request body for %1%", name);
        return false;
    }
    if (esp_http_client_fetch_headers(guard.handle) < 0) {
        BROOKESIA_LOGE("Failed to fetch download response headers for %1%", name);
        return false;
    }
    const int status_code = esp_http_client_get_status_code(guard.handle);
    if (status_code != 200) {
        BROOKESIA_LOGE("Download failed with status %1% for %2%", status_code, name);
        return false;
    }

    // Download into a temp file and rename into place, so a failed download never leaves a
    // truncated file at `dest_path` overwriting a previously good one.
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
            out_file.write(chunk.data(), read_len);
            if (!out_file) {
                BROOKESIA_LOGE("Failed to write to %1%", tmp_path);
                return false;
            }
        }
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp_path, dest_path, rename_ec);
    if (rename_ec) {
        BROOKESIA_LOGE("Failed to move downloaded file into place for %1%: %2%", name, rename_ec.message());
        return false;
    }

    return true;
}

} // namespace

bool SdUpdater::init(const Config &config)
{
    BROOKESIA_CHECK_FALSE_RETURN(!is_initialized_.load(), false, "Already initialized");
    BROOKESIA_CHECK_FALSE_RETURN(config.task_scheduler != nullptr, false, "Invalid task scheduler");

    config_ = config;
    is_initialized_.store(true);

    return true;
}

bool SdUpdater::start()
{
    BROOKESIA_CHECK_FALSE_RETURN(is_initialized_.load(), false, "Not initialized");
    BROOKESIA_CHECK_FALSE_RETURN(WifiHelper::is_available(), false, "WiFi service not available");

    auto post_run_update_check = [this]() {
        active_conn_.reset();
        run_update_check();
    };

    auto on_general_event = [this, post_run_update_check](const std::string &, const std::string &event, bool) {
        if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Connected)) {
            BROOKESIA_LOGI("WiFi connected, checking SD card for updates");
            config_.task_scheduler->post(post_run_update_check);
        }
    };

    auto conn = WifiHelper::subscribe_event(WifiHelper::EventId::GeneralEventHappened, on_general_event);
    active_conn_ = std::make_shared<service::EventRegistry::SignalConnection>(std::move(conn));

    return true;
}

void SdUpdater::run_update_check()
{
    if (has_run_.exchange(true)) {
        return;
    }

    std::string base_url = CONFIG_EXAMPLE_SD_UPDATE_SERVER_URL;
    if (auto override_url = SdSettings::get_instance().get_string(SD_SETTINGS_UPDATE_SERVER_URL_KEY);
        override_url.has_value()) {
        BROOKESIA_LOGI("Overriding SD update server URL from SD settings: %1%", *override_url);
        base_url = std::move(*override_url);
    }

    BROOKESIA_LOGI("Checking for SD file updates from %1%", base_url);

    auto files = fetch_file_list(base_url);
    if (!files) {
        BROOKESIA_LOGW("Failed to fetch SD update file list, skipping update");
        return;
    }

    size_t updated_count = 0;
    for (const auto &entry : *files) {
        if (!entry.is_object()) {
            BROOKESIA_LOGW("Skipping malformed file entry in update list");
            continue;
        }
        const auto &obj = entry.as_object();
        const auto *name_value = obj.if_contains("name");
        const auto *md5_value = obj.if_contains("md5");
        if ((name_value == nullptr) || !name_value->is_string() ||
            (md5_value == nullptr) || !md5_value->is_string()) {
            BROOKESIA_LOGW("Skipping malformed file entry in update list");
            continue;
        }

        const std::string name(name_value->as_string());
        const std::string expected_md5 = to_lower(std::string(md5_value->as_string()));

        const std::filesystem::path relative_path(name);
        if (name.empty() || relative_path.is_absolute() || (name.find("..") != std::string::npos)) {
            BROOKESIA_LOGW("Skipping unsafe file path in update list: %1%", name);
            continue;
        }

        const std::filesystem::path dest_path = std::filesystem::path(SD_UPDATE_ROOT_DIR) / relative_path;

        bool need_download = true;
        if (std::filesystem::exists(dest_path)) {
            if (auto local_md5 = compute_file_md5(dest_path); local_md5 && (*local_md5 == expected_md5)) {
                need_download = false;
            }
        }
        if (!need_download) {
            continue;
        }

        BROOKESIA_LOGI("Downloading updated file: %1%", name);
        if (download_file(base_url, name, dest_path)) {
            ++updated_count;
        }
    }

    BROOKESIA_LOGI("SD update check complete, %1% file(s) updated", updated_count);
}

#else // !CONFIG_EXAMPLE_SD_UPDATE_ENABLE

bool SdUpdater::init(const Config &config)
{
    config_ = config;
    is_initialized_.store(true);
    return true;
}

bool SdUpdater::start()
{
    return true;
}

void SdUpdater::run_update_check() {}

#endif // CONFIG_EXAMPLE_SD_UPDATE_ENABLE
