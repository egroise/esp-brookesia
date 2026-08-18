/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boost/json.hpp"
#include "brookesia/agent_custom_ia/macro_configs.h"
#if !BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG
#   define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif
#include "private/utils.hpp"
#include "brookesia/lib_utils/plugin.hpp"
#include "brookesia/agent_manager/manager.hpp"
#include "brookesia/agent_custom_ia/agent_custom_ia.hpp"

namespace esp_brookesia::agent {

namespace {

constexpr size_t WAV_HEADER_SIZE = 44;
constexpr size_t HTTP_READ_CHUNK_SIZE = 4096;
constexpr size_t JSON_RESPONSE_MAX_SIZE = 4096;

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

std::array<uint8_t, WAV_HEADER_SIZE> build_wav_header(
    uint32_t data_size, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample
)
{
    std::array<uint8_t, WAV_HEADER_SIZE> header{};
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = static_cast<uint16_t>(channels * bits_per_sample / 8);
    const uint32_t riff_chunk_size = 36 + data_size;

    auto put_u32 = [&header](size_t offset, uint32_t value) {
        header[offset + 0] = static_cast<uint8_t>(value & 0xFF);
        header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        header[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        header[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    };
    auto put_u16 = [&header](size_t offset, uint16_t value) {
        header[offset + 0] = static_cast<uint8_t>(value & 0xFF);
        header[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    };

    std::memcpy(&header[0], "RIFF", 4);
    put_u32(4, riff_chunk_size);
    std::memcpy(&header[8], "WAVE", 4);
    std::memcpy(&header[12], "fmt ", 4);
    put_u32(16, 16); // fmt chunk size (PCM)
    put_u16(20, 1);  // audio format = PCM
    put_u16(22, channels);
    put_u32(24, sample_rate);
    put_u32(28, byte_rate);
    put_u16(32, block_align);
    put_u16(34, bits_per_sample);
    std::memcpy(&header[36], "data", 4);
    put_u32(40, data_size);

    return header;
}

struct WavHeaderInfo {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_size = 0;
};

bool parse_wav_header(const uint8_t *data, size_t size, WavHeaderInfo &info)
{
    if (size < WAV_HEADER_SIZE) {
        return false;
    }
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0 ||
        std::memcmp(data + 12, "fmt ", 4) != 0 || std::memcmp(data + 36, "data", 4) != 0) {
        return false;
    }

    auto get_u32 = [data](size_t offset) {
        return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
    };
    auto get_u16 = [data](size_t offset) {
        return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    };

    info.channels = get_u16(22);
    info.sample_rate = get_u32(24);
    info.bits_per_sample = get_u16(34);
    info.data_size = get_u32(40);

    return true;
}

/**
 * @brief RAII flag reset, so `is_request_in_progress_` is always cleared regardless of which
 *        `return`/`break` path `CustomIA::run_conversation_round()` takes.
 */
struct AtomicFlagResetGuard {
    std::atomic_bool &flag;

    ~AtomicFlagResetGuard()
    {
        flag = false;
    }
};

} // namespace

std::string CustomIA::get_component_version()
{
    return make_version(
               BROOKESIA_AGENT_CUSTOM_IA_VER_MAJOR, BROOKESIA_AGENT_CUSTOM_IA_VER_MINOR,
               BROOKESIA_AGENT_CUSTOM_IA_VER_PATCH
           );
}

bool CustomIA::on_init()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_LOGI(
        "Version: %1%.%2%.%3%", BROOKESIA_AGENT_CUSTOM_IA_VER_MAJOR, BROOKESIA_AGENT_CUSTOM_IA_VER_MINOR,
        BROOKESIA_AGENT_CUSTOM_IA_VER_PATCH
    );

    return true;
}

bool CustomIA::on_activate()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_CHECK_FALSE_RETURN(validate_info(data_info_), false, "Invalid info");

    is_shutting_down_ = false;

    trigger_general_event(GeneralEvent::Activated);

    return true;
}

bool CustomIA::on_startup()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    trigger_general_event(GeneralEvent::Started);

    return true;
}

void CustomIA::on_shutdown()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    // Let any in-flight worker task notice and bail out of its poll loop early.
    is_shutting_down_ = true;

    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_buffer_.clear();
        record_overflow_warned_ = false;
    }

    trigger_general_event(GeneralEvent::Stopped);
}

bool CustomIA::on_sleep()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    trigger_general_event(GeneralEvent::Slept);

    return true;
}

bool CustomIA::on_wakeup()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    trigger_general_event(GeneralEvent::Awake);

    return true;
}

bool CustomIA::on_manual_start_listening()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    if (is_request_in_progress()) {
        BROOKESIA_LOGW("A previous request is still in progress, ignoring new recording");
        return false;
    }

    std::lock_guard<std::mutex> lock(record_mutex_);
    record_buffer_.clear();
    record_overflow_warned_ = false;

    return true;
}

bool CustomIA::on_manual_stop_listening()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    std::vector<uint8_t> recording;
    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        recording = std::move(record_buffer_);
        record_buffer_.clear();
        record_overflow_warned_ = false;
    }

    if (recording.empty()) {
        BROOKESIA_LOGD("No audio recorded, skip");
        return true;
    }
    if (is_shutting_down_) {
        return true;
    }

    is_request_in_progress_ = true;

    auto *args = new (std::nothrow) WorkerTaskArgs{this, std::move(recording)};
    if (!args) {
        BROOKESIA_LOGE("Failed to allocate worker task args");
        is_request_in_progress_ = false;
        return true;
    }

    auto task_created = xTaskCreate(
                             &CustomIA::worker_task_trampoline, "custom_ia_worker",
                             BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE, args, tskIDLE_PRIORITY + 4, nullptr
                         );
    if (task_created != pdPASS) {
        BROOKESIA_LOGE("Failed to create CustomIA worker task");
        delete args;
        is_request_in_progress_ = false;
    }

    return true;
}

bool CustomIA::on_encoder_data_ready(const uint8_t *data, size_t data_size)
{
    // BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    if (!data || data_size == 0) {
        return true;
    }

    const auto &cfg = get_audio_config().encoder.general;
    const size_t bytes_per_ms =
        static_cast<size_t>(cfg.sample_rate) * cfg.channels * (cfg.sample_bits / 8) / 1000;
    const size_t max_bytes = bytes_per_ms * BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS;

    std::lock_guard<std::mutex> lock(record_mutex_);
    if (record_buffer_.size() >= max_bytes) {
        if (!record_overflow_warned_) {
            BROOKESIA_LOGW("Max recording duration (%1% ms) reached, dropping further audio", BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS);
            record_overflow_warned_ = true;
        }
        return true;
    }

    const size_t space_left = max_bytes - record_buffer_.size();
    const size_t to_copy = std::min(space_left, data_size);
    record_buffer_.insert(record_buffer_.end(), data, data + to_copy);

    return true;
}

bool CustomIA::set_info(const boost::json::object &info)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_LOGD("Params: info(%1%)", BROOKESIA_DESCRIBE_TO_STR(info));

    CustomIAInfo custom_ia_info;

    auto success = BROOKESIA_DESCRIBE_FROM_JSON(info, custom_ia_info);
    BROOKESIA_CHECK_FALSE_RETURN(
        success, false, "Failed to deserialize CustomIA info: %1%", BROOKESIA_DESCRIBE_TO_STR(info)
    );

    auto current_info_str = BROOKESIA_DESCRIBE_JSON_SERIALIZE(data_info_);
    auto new_info_str = BROOKESIA_DESCRIBE_JSON_SERIALIZE(custom_ia_info);
    if (current_info_str == new_info_str) {
        BROOKESIA_LOGI("Info is the same, skip setting");
        return true;
    }

    BROOKESIA_CHECK_FALSE_RETURN(validate_info(custom_ia_info), false, "Invalid info");

    data_info_ = std::move(custom_ia_info);

    return true;
}

bool CustomIA::reset_data()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    data_info_ = CustomIAInfo{};

    BROOKESIA_LOGI("Reset all data");

    return true;
}

bool CustomIA::validate_info(CustomIAInfo &info)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_CHECK_FALSE_RETURN(!info.server_url.empty(), false, "Server URL is empty");

    return true;
}

void CustomIA::worker_task_trampoline(void *ctx)
{
    auto *args = static_cast<WorkerTaskArgs *>(ctx);
    if (args && args->self) {
        args->self->run_conversation_round(std::move(args->recording));
    }
    delete args;
    vTaskDelete(nullptr);
}

void CustomIA::run_conversation_round(std::vector<uint8_t> recording)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    AtomicFlagResetGuard busy_guard{is_request_in_progress_};

    if (recording.empty() || is_shutting_down_) {
        return;
    }

    // Wrap the raw PCM recording in a minimal 44-byte WAV header understood by the backend.
    const auto &encoder_cfg = get_audio_config().encoder.general;
    auto wav_header = build_wav_header(
                           static_cast<uint32_t>(recording.size()), encoder_cfg.sample_rate, encoder_cfg.channels,
                           encoder_cfg.sample_bits
                       );

    std::vector<uint8_t> wav_bytes;
    wav_bytes.reserve(wav_header.size() + recording.size());
    wav_bytes.insert(wav_bytes.end(), wav_header.begin(), wav_header.end());
    wav_bytes.insert(wav_bytes.end(), recording.begin(), recording.end());
    recording.clear();
    recording.shrink_to_fit();

    const std::string boundary = "----BrookesiaCustomIA0123456789";
    const std::string preamble =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    const std::string epilogue = "\r\n--" + boundary + "--\r\n";
    const size_t total_len = preamble.size() + wav_bytes.size() + epilogue.size();
    const std::string content_type_header = "multipart/form-data; boundary=" + boundary;

    const std::string in_url = data_info_.server_url + "/in";
    std::string in_body;

    {
        esp_http_client_config_t config = {};
        config.url = in_url.c_str();
        config.timeout_ms = BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS;
        config.buffer_size_tx = 4096;

        HttpClientGuard guard;
        guard.handle = esp_http_client_init(&config);
        if (!guard.handle) {
            BROOKESIA_LOGE("Failed to init HTTP client for /in");
            return;
        }

        esp_http_client_set_method(guard.handle, HTTP_METHOD_POST);
        esp_http_client_set_header(guard.handle, "Content-Type", content_type_header.c_str());

        if (esp_http_client_open(guard.handle, static_cast<int>(total_len)) != ESP_OK) {
            BROOKESIA_LOGE("Failed to open /in request: %1%", in_url);
            return;
        }

        auto write_all = [&guard](const uint8_t *data, size_t size) {
            int written = esp_http_client_write(guard.handle, reinterpret_cast<const char *>(data), static_cast<int>(size));
            return written == static_cast<int>(size);
        };
        bool write_ok = write_all(reinterpret_cast<const uint8_t *>(preamble.data()), preamble.size()) &&
                         write_all(wav_bytes.data(), wav_bytes.size()) &&
                         write_all(reinterpret_cast<const uint8_t *>(epilogue.data()), epilogue.size());
        wav_bytes.clear();
        wav_bytes.shrink_to_fit();
        if (!write_ok) {
            BROOKESIA_LOGE("Failed to write /in request body");
            return;
        }

        if (esp_http_client_fetch_headers(guard.handle) < 0) {
            BROOKESIA_LOGE("Failed to fetch /in response headers");
            return;
        }
        if (esp_http_client_get_status_code(guard.handle) != 200) {
            BROOKESIA_LOGE("/in request failed with status %1%", esp_http_client_get_status_code(guard.handle));
            return;
        }

        std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
        int read_len;
        while ((read_len = esp_http_client_read(guard.handle, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
            in_body.append(chunk.data(), read_len);
            if (in_body.size() > JSON_RESPONSE_MAX_SIZE) {
                BROOKESIA_LOGE("/in response body too large");
                return;
            }
        }
    }

    boost::system::error_code in_parse_ec;
    auto in_json = boost::json::parse(in_body, in_parse_ec);
    if (in_parse_ec || !in_json.is_object()) {
        BROOKESIA_LOGE("Failed to parse /in response: %1%", in_body);
        return;
    }
    const auto &in_obj = in_json.as_object();
    if (!in_obj.contains("callId") || !in_obj.at("callId").is_string()) {
        BROOKESIA_LOGE("Missing callId in /in response: %1%", in_body);
        return;
    }
    const std::string call_id(in_obj.at("callId").as_string());
    if (in_obj.contains("text") && in_obj.at("text").is_string()) {
        std::string user_text(in_obj.at("text").as_string());
        if (!user_text.empty()) {
            set_user_speaking_text(user_text);
        }
    }

    const std::string out_url = data_info_.server_url + "/out/" + call_id;
    const uint32_t poll_interval_ms = BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS;
    const uint32_t poll_timeout_ms = BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS;

    bool got_audio = false;
    bool is_speaking_flag_set = false;
    uint32_t elapsed_ms = 0;

    while (!is_shutting_down_ && elapsed_ms <= poll_timeout_ms) {
        esp_http_client_config_t poll_config = {};
        poll_config.url = out_url.c_str();
        poll_config.timeout_ms = BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS;

        HttpClientGuard guard;
        guard.handle = esp_http_client_init(&poll_config);
        if (!guard.handle) {
            BROOKESIA_LOGE("Failed to init HTTP client for /out");
            break;
        }
        esp_http_client_set_method(guard.handle, HTTP_METHOD_GET);
        if (esp_http_client_open(guard.handle, 0) != ESP_OK) {
            BROOKESIA_LOGE("Failed to open /out request: %1%", out_url);
            break;
        }
        if (esp_http_client_fetch_headers(guard.handle) < 0) {
            BROOKESIA_LOGE("Failed to fetch /out response headers");
            break;
        }
        const int status_code = esp_http_client_get_status_code(guard.handle);

        // Sniff up to the WAV header size first: enough to tell a JSON status payload apart from
        // the binary WAV reply, and (if it is WAV) to hold the whole header for parsing.
        std::vector<uint8_t> sniff_buf;
        sniff_buf.reserve(WAV_HEADER_SIZE);
        {
            std::vector<uint8_t> chunk(HTTP_READ_CHUNK_SIZE);
            while (sniff_buf.size() < WAV_HEADER_SIZE) {
                int read_len = esp_http_client_read(guard.handle, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()));
                if (read_len <= 0) {
                    break;
                }
                sniff_buf.insert(sniff_buf.end(), chunk.data(), chunk.data() + read_len);
            }
        }

        const bool is_wav = (status_code == 200) && (sniff_buf.size() >= 4) &&
                             (std::memcmp(sniff_buf.data(), "RIFF", 4) == 0);

        if (!is_wav) {
            std::string body(reinterpret_cast<char *>(sniff_buf.data()), sniff_buf.size());
            std::vector<char> chunk(HTTP_READ_CHUNK_SIZE);
            int more_len;
            while ((more_len = esp_http_client_read(guard.handle, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
                body.append(chunk.data(), more_len);
                if (body.size() > JSON_RESPONSE_MAX_SIZE) {
                    break;
                }
            }

            boost::system::error_code out_parse_ec;
            auto out_json = boost::json::parse(body, out_parse_ec);
            if (out_parse_ec || !out_json.is_object()) {
                BROOKESIA_LOGE("Failed to parse /out response (status %1%): %2%", status_code, body);
                break;
            }
            const auto &out_obj = out_json.as_object();
            std::string status_str;
            if (out_obj.contains("status") && out_obj.at("status").is_string()) {
                status_str = std::string(out_obj.at("status").as_string());
            }
            if (status_str != "processing") {
                std::string error_message;
                if (out_obj.contains("error") && out_obj.at("error").is_string()) {
                    error_message = std::string(out_obj.at("error").as_string());
                }
                BROOKESIA_LOGE("CustomIA backend reported error for call '%1%': %2%", call_id, error_message);
                break;
            }
        } else {
            if (sniff_buf.size() < WAV_HEADER_SIZE) {
                BROOKESIA_LOGE("Truncated WAV header in /out response");
                break;
            }
            WavHeaderInfo wav_info;
            if (!parse_wav_header(sniff_buf.data(), sniff_buf.size(), wav_info)) {
                BROOKESIA_LOGE("Invalid WAV header in /out response");
                break;
            }
            if (wav_info.sample_rate != get_audio_config().decoder.general.sample_rate) {
                BROOKESIA_LOGW(
                    "TTS sample rate mismatch: backend sent %1% Hz, decoder is configured for %2% Hz",
                    wav_info.sample_rate, get_audio_config().decoder.general.sample_rate
                );
            }

            set_speaking(true);
            is_speaking_flag_set = true;

            if (sniff_buf.size() > WAV_HEADER_SIZE) {
                feed_audio_decoder_data(sniff_buf.data() + WAV_HEADER_SIZE, sniff_buf.size() - WAV_HEADER_SIZE);
            }

            std::vector<uint8_t> chunk(HTTP_READ_CHUNK_SIZE);
            int more_len;
            while (!is_shutting_down_ &&
                    (more_len = esp_http_client_read(guard.handle, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()))) > 0) {
                feed_audio_decoder_data(chunk.data(), static_cast<size_t>(more_len));
            }

            got_audio = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        elapsed_ms += poll_interval_ms;
    }

    if (!got_audio && !is_shutting_down_) {
        BROOKESIA_LOGW("No reply audio received for CustomIA call '%1%'", call_id);
    }
    if (is_speaking_flag_set) {
        set_speaking(false);
    }
}

#if BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER
BROOKESIA_PLUGIN_REGISTER_SINGLETON(
    Base, CustomIA, CustomIA::get_instance().get_attributes().get_name(), CustomIA::get_instance()
);
BROOKESIA_PLUGIN_REGISTER_SINGLETON_WITH_SYMBOL(
    service::ServiceBase, CustomIA, CustomIA::get_instance().get_attributes().get_name(), CustomIA::get_instance(),
    BROOKESIA_AGENT_CUSTOM_IA_PLUGIN_SYMBOL
);
#endif

} // namespace esp_brookesia::agent
