/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "brookesia/service_helper/agent/custom_ia.hpp"
#include "brookesia/agent_manager/base.hpp"
#include "brookesia/agent_custom_ia/macro_configs.h"

namespace esp_brookesia::agent {

/**
 * @brief Re-exported runtime configuration type for the CustomIA agent.
 */
using CustomIAInfo = service::helper::CustomIA::Info;

/**
 * @brief Push-to-talk agent implementation backed by a self-hosted HTTP STT/TTS backend.
 *
 * Unlike the other agents, this one is not voice/wake-word driven: recording is started and
 * stopped explicitly (`ManualStartListening`/`ManualStopListening`, see `Base`), typically wired
 * to a touch press/release gesture by the application. On stop, the buffered recording is
 * uploaded to the backend, and the synthesized reply audio is streamed back for playback.
 */
class CustomIA: public Base {
public:
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////// The following are the agent specific attributes /////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /**
     * @brief Default metadata advertised by the CustomIA agent.
     */
    inline static const AgentAttributes DEFAULT_AGENT_ATTRIBUTES{
        .name = service::helper::CustomIA::get_name().data(),
        .operation_timeout = {
            .start = 3000,
        },
        .support_general_events = {
            service::helper::AgentManager::AgentGeneralEvent::SpeakingStatusChanged,
            service::helper::AgentManager::AgentGeneralEvent::ListeningStatusChanged,
            service::helper::AgentManager::AgentGeneralEvent::UserSpeakingTextGot,
        },
        .require_time_sync = false,
    };
    /**
     * @brief Default audio pipeline configuration used by the CustomIA agent.
     */
    static constexpr AudioConfig DEFAULT_AUDIO_CONFIG{
        .encoder = {
            .type = service::helper::Audio::CodecFormat::PCM,
            .general = {
                .channels = 1,
                .sample_bits = 16,
                .sample_rate = BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE,
                .frame_duration = 60,
            },
            .fetch_data_size = 1024,
            .enable_afe = true,
            .afe_wake_start_timeout_ms = 30000,
            .afe_wake_end_timeout_ms = 10000,
        },
        .decoder = {
            .type = service::helper::Audio::CodecFormat::PCM,
            .general = {
                .channels = 1,
                .sample_bits = 16,
                .sample_rate = BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE,
                .frame_duration = 60,
            },
        }
    };

    /**
     * @brief Get the singleton CustomIA agent instance.
     *
     * @return CustomIA& Singleton instance.
     */
    static CustomIA &get_instance()
    {
        static CustomIA instance;
        return instance;
    }

private:
    static std::string get_component_version();

    CustomIA()
        : Base(
              DEFAULT_AGENT_ATTRIBUTES,
              get_component_version(),
              DEFAULT_AUDIO_CONFIG
          )
    {
    }
    ~CustomIA() = default;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////// The following are the general interfaces which are inherited from the service::ServiceBase class /////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool on_init() override;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////// The following are the general interfaces which are inherited from the Base class ////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool on_activate() override;
    bool on_startup() override;
    bool on_sleep() override;
    bool on_wakeup() override;
    void on_shutdown() override;

    bool on_manual_start_listening() override;
    bool on_manual_stop_listening() override;

    bool on_encoder_data_ready(const uint8_t *data, size_t data_size) override;
    bool set_info(const boost::json::object &info) override;
    bool reset_data() override;

    std::vector<service::FunctionSchema> get_function_schemas() override
    {
        auto helper_schemas = service::helper::CustomIA::get_function_schemas();
        return std::vector<service::FunctionSchema>(helper_schemas.begin(), helper_schemas.end());
    }
    std::vector<service::EventSchema> get_event_schemas() override
    {
        auto helper_schemas = service::helper::CustomIA::get_event_schemas();
        return std::vector<service::EventSchema>(helper_schemas.begin(), helper_schemas.end());
    }
    service::ServiceBase::FunctionHandlerMap get_function_handlers() override
    {
        return {};
    }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////// The following are the agent specific interfaces ////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool validate_info(CustomIAInfo &info);

    bool is_request_in_progress() const
    {
        return is_request_in_progress_;
    }

    // Runs on a dedicated worker task; not the cooperative task scheduler, since it performs
    // blocking HTTP I/O that can take several seconds.
    void run_conversation_round(std::vector<uint8_t> recording);
    static void worker_task_trampoline(void *ctx);

    CustomIAInfo data_info_{};

    std::mutex record_mutex_;
    std::vector<uint8_t> record_buffer_;
    bool record_overflow_warned_ = false;

    std::atomic_bool is_request_in_progress_{false};
    std::atomic_bool is_shutting_down_{false};

    // Owns the heap-allocated recording handed to the worker task; freed by the task itself.
    struct WorkerTaskArgs {
        CustomIA *self = nullptr;
        std::vector<uint8_t> recording;
    };
};

} // namespace esp_brookesia::agent
