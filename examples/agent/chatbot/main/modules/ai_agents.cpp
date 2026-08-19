/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "boost/format.hpp"
#include "sdkconfig.h"
#include "esp_log.h"
#include "brookesia/lib_utils/function_guard.hpp"
#include "brookesia/service_manager.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/mcp_utils/mcp_utils.hpp"
#include "brookesia/hal_interface.hpp"
#include "brookesia/service_helper/media/bt_speaker.hpp"
#include "private/utils.hpp"
#include "modules/display/display.hpp"
#include "modules/touch_pad.hpp"
#include "modules/sd_settings.hpp"
#include "ai_agents.hpp"

using namespace esp_brookesia;

using AgentHelper = esp_brookesia::service::helper::AgentManager;
using CustomIAHelper = esp_brookesia::service::helper::CustomIA;
using EmoteHelper = esp_brookesia::service::helper::ExpressionEmote;
using AudioHelper = esp_brookesia::service::helper::Audio;
using AudioPlaybackHelper = esp_brookesia::service::helper::AudioPlayback;
using BtSpeakerHelper = esp_brookesia::service::helper::BtSpeaker;
using WifiHelper = esp_brookesia::service::helper::Wifi;
using DeviceHelper = esp_brookesia::service::helper::Device;

#define XIAO_ZHI_AUDIO_URL_PREFIX "file://littlefs/xiaozhi/"
#define SD_SETTINGS_CUSTOM_IA_SERVER_URL_KEY "CONFIG_EXAMPLE_AGENTS_CUSTOM_IA_SERVER_URL"

bool AI_Agents::init(const Config &config)
{
    BROOKESIA_CHECK_NULL_RETURN(config.task_scheduler, false, "Task scheduler is not available");

    if (is_initialized())
    {
        BROOKESIA_LOGW("AI agents are already initialized");
        return true;
    }

    if (!AgentHelper::is_available())
    {
        BROOKESIA_LOGW("Agent manager is not available, skip initialization");
        return false;
    }

    task_scheduler_ = config.task_scheduler;
    emote_animation_duration_ms_ = config.emote_animation_duration_ms;
    agent_restart_delay_s_ = config.agent_restart_delay_s;

    auto binding = service::ServiceManager::get_instance().bind(AgentHelper::get_name().data());
    BROOKESIA_CHECK_FALSE_RETURN(binding.is_valid(), false, "Failed to bind Agent manager service");

    service_bindings_.push_back(std::move(binding));

#ifdef IDF_CI_BUILD
    BROOKESIA_LOGI("CI build detected, resetting Agent manager data");
    auto reset_data_result = AgentHelper::call_function_sync(AgentHelper::FunctionId::ResetData);
    BROOKESIA_CHECK_FALSE_RETURN(
        reset_data_result, false, "Failed to reset Agent manager data: %1%", reset_data_result.error());
#endif

    process_agent_general_events();
    process_emote();
    process_wifi_events();
    process_touch_pad_events();

    BROOKESIA_LOGI("AI agents initialized successfully");

    return true;
}

void AI_Agents::init_custom_ia()
{
    if (!CustomIAHelper::is_available())
    {
        BROOKESIA_LOGW("CustomIA agent is not available, skip initialization");
        return;
    }

    BROOKESIA_CHECK_FALSE_EXIT(is_initialized(), "AI agents are not initialized");

#if !CONFIG_EXAMPLE_AGENTS_ENABLE_CUSTOM_IA
    BROOKESIA_LOGW("CustomIA agent is not enabled, skip initialization");
#else
    BROOKESIA_LOGI("Setting CustomIA agent info...");
    auto result = AgentHelper::call_function_sync(
        AgentHelper::FunctionId::SetAgentInfo, CustomIAHelper::get_name().data(),
        get_agent_custom_ia_info());
    if (!result)
    {
        BROOKESIA_LOGE("Failed to set CustomIA agent info: %1%", result.error());
    }
    else
    {
        BROOKESIA_LOGI("Set CustomIA agent info successfully");
    }
#endif // CONFIG_EXAMPLE_AGENTS_ENABLE_CUSTOM_IA
}

void AI_Agents::process_agent_general_unexpected_events()
{
    auto general_event_happened_slot =
        [this](const std::string &event_name, const std::string &general_event, bool is_unexpected)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD(
            "Params: event_name(%1%), general_event(%2%), is_unexpected(%3%)", event_name, general_event,
            BROOKESIA_DESCRIBE_TO_STR(is_unexpected));

        if (!is_unexpected)
        {
            return;
        }

        BROOKESIA_LOGW("Detected unexpected general event: %1%", general_event);

        AgentHelper::GeneralEvent general_event_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(general_event, general_event_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert general event: %1%", general_event);

        switch (general_event_enum)
        {
        case AgentHelper::GeneralEvent::Stopped:
        {
            if (!is_wifi_connected())
            {
                BROOKESIA_LOGW("WiFi is not connected, skip restart agent");
                return;
            }

            BROOKESIA_LOGW("Try to restart agent in %1% seconds...", agent_restart_delay_s_);

            auto task_func = []()
            {
                BROOKESIA_LOG_TRACE_GUARD();
                BROOKESIA_LOGW("Restarting agent...");
                auto result = AgentHelper::call_function_sync(
                    AgentHelper::FunctionId::TriggerGeneralAction,
                    BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Start));
                if (!result)
                {
                    BROOKESIA_LOGW("Failed to restart agent: %1%", result.error());
                }
            };
            auto result = task_scheduler_->post_delayed(std::move(task_func), agent_restart_delay_s_ * 1000);
            BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to post restart agent task");
            break;
        }
        default:
            break;
        }
    };
    auto connection = AgentHelper::subscribe_event(
        AgentHelper::EventId::GeneralEventHappened, general_event_happened_slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent general event happened event");
    }
}

void AI_Agents::process_agent_general_suspend_status_changed()
{
    auto slot = [this](const std::string &event_name, bool is_suspended)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), is_suspended(%2%)", event_name, is_suspended);

        if (is_suspended)
        {
            // Trigger sleep action
            AgentHelper::call_function_async(
                AgentHelper::FunctionId::TriggerGeneralAction,
                BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Sleep));
        }
        is_suspended_ = is_suspended;
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::SuspendStatusChanged, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent general suspend status changed event");
    }
}

bool AI_Agents::start_agent()
{
    // Ensure target agent is activated after switching
    auto activate_handler = [this](service::FunctionResult &&result)
    {
        if (!result.success)
        {
            BROOKESIA_LOGE("Failed to activate agent: %1%", result.error_message);
        }
    };
    auto activate_result = AgentHelper::call_function_async(
        AgentHelper::FunctionId::TriggerGeneralAction,
        BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Activate), activate_handler);
    BROOKESIA_CHECK_FALSE_RETURN(activate_result, false, "Failed to activate agent");

    auto start_handler = [this](service::FunctionResult &&result)
    {
        if (!result.success)
        {
            BROOKESIA_LOGE("Failed to trigger general action: %1%", result.error_message);
        }
    };
    auto trigger_general_action_result = AgentHelper::call_function_async(
        AgentHelper::FunctionId::TriggerGeneralAction,
        BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Start), start_handler);
    BROOKESIA_CHECK_FALSE_RETURN(trigger_general_action_result, false, "Failed to trigger general action");

    return true;
}

void AI_Agents::stop_agent()
{
    AgentHelper::call_function_async(
        AgentHelper::FunctionId::TriggerGeneralAction, BROOKESIA_DESCRIBE_TO_STR(AgentHelper::GeneralAction::Stop));
}

void AI_Agents::process_agent_general_events()
{
    // Process unexpected general events:
    //   1. Stopped: Restart the agent after a delay
    process_agent_general_unexpected_events();
    process_agent_general_suspend_status_changed();
    process_bt_speaker_events();
}

void AI_Agents::process_bt_speaker_events()
{
    if (!BtSpeakerHelper::is_available())
    {
        BROOKESIA_LOGW("BtSpeaker service is not available, skip Agent playback coordination");
        return;
    }

    auto slot = [this](const std::string &, const std::string &general_event, bool)
    {
        AgentHelper::GeneralEvent event;
        if (!BROOKESIA_DESCRIBE_STR_TO_ENUM(general_event, event))
        {
            BROOKESIA_LOGW("Failed to parse Agent general event: %1%", general_event);
            return;
        }

        if (event == AgentHelper::GeneralEvent::Awake)
        {
            auto state_result = BtSpeakerHelper::call_function_sync<boost::json::object>(
                BtSpeakerHelper::FunctionId::GetState);
            if (!state_result)
            {
                BROOKESIA_LOGW("BtSpeaker GetState failed while handling Agent Awake: %1%", state_result.error());
                return;
            }

            BtSpeakerHelper::State state;
            if (!BROOKESIA_DESCRIBE_FROM_JSON(*state_result, state))
            {
                BROOKESIA_LOGW("Failed to parse BtSpeaker state while handling Agent Awake");
                return;
            }
            if (should_resume_bt_speaker_)
            {
                if (state.is_connected && state.connection &&
                    state.connection->connection_id == paused_bt_connection_id_.load())
                {
                    return;
                }
                should_resume_bt_speaker_ = false;
                paused_bt_connection_id_ = -1;
            }
            if (!state.is_music_active || !state.is_connected || !state.connection)
            {
                return;
            }

            auto pause_result = BtSpeakerHelper::call_function_sync(BtSpeakerHelper::FunctionId::Pause);
            if (!pause_result)
            {
                BROOKESIA_LOGW("BtSpeaker Pause failed while handling Agent Awake: %1%", pause_result.error());
                return;
            }
            paused_bt_connection_id_ = state.connection->connection_id;
            should_resume_bt_speaker_ = true;
            return;
        }

        if ((event == AgentHelper::GeneralEvent::Slept || event == AgentHelper::GeneralEvent::Stopped) &&
            should_resume_bt_speaker_)
        {
            auto state_result = BtSpeakerHelper::call_function_sync<boost::json::object>(
                BtSpeakerHelper::FunctionId::GetState);
            BtSpeakerHelper::State state;
            if (!state_result || !BROOKESIA_DESCRIBE_FROM_JSON(*state_result, state) || !state.is_started ||
                !state.is_connected || !state.connection ||
                state.connection->connection_id != paused_bt_connection_id_.load())
            {
                should_resume_bt_speaker_ = false;
                paused_bt_connection_id_ = -1;
                return;
            }
            auto resume_result = BtSpeakerHelper::call_function_sync(BtSpeakerHelper::FunctionId::Resume);
            if (!resume_result)
            {
                BROOKESIA_LOGW("BtSpeaker Resume failed while handling Agent idle event: %1%", resume_result.error());
                return;
            }
            should_resume_bt_speaker_ = false;
            paused_bt_connection_id_ = -1;
        }
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::GeneralEventHappened, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGW("Failed to subscribe to Agent events for BtSpeaker playback coordination");
    }

    auto connection_state_slot = [this](
                                     const std::string &, const std::string &state,
                                     const boost::json::object &)
    {
        BtSpeakerHelper::ConnectionState connection_state;
        if (BROOKESIA_DESCRIBE_STR_TO_ENUM(state, connection_state) &&
            connection_state == BtSpeakerHelper::ConnectionState::Disconnected)
        {
            should_resume_bt_speaker_ = false;
            paused_bt_connection_id_ = -1;
        }
    };
    auto bt_connection = BtSpeakerHelper::subscribe_event(
        BtSpeakerHelper::EventId::ConnectionStateChanged, connection_state_slot);
    if (bt_connection.connected())
    {
        service_connections_.push_back(std::move(bt_connection));
    }
    else
    {
        BROOKESIA_LOGW("Failed to subscribe to BtSpeaker connection events");
    }
}

void AI_Agents::process_emote_when_general_action_triggered()
{
    auto slot = [this](const std::string &event_name, const std::string &general_action)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), general_action(%2%)", event_name, general_action);

        AgentHelper::GeneralAction general_action_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(general_action, general_action_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert general action: %1%", general_action);

        switch (general_action_enum)
        {
        case AgentHelper::GeneralAction::TimeSync:
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::SetEmoji, "winking");
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Time Syncing...");
            break;
        case AgentHelper::GeneralAction::Activate:
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::SetEmoji, "winking");
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Activating...");
            break;
        case AgentHelper::GeneralAction::Start:
            is_sleeping_ = false;
            is_suspended_ = false;
            is_stopped_ = false;
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Starting...");
            break;
        case AgentHelper::GeneralAction::Sleep:
        {
            is_sleeping_ = true;
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::SetEmoji, "sleepy");
            if (!is_suspended())
            {
                EmoteHelper::call_function_async(
                    EmoteHelper::FunctionId::SetEventMessage,
                    BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Idle));
            }
            break;
        }
        case AgentHelper::GeneralAction::WakeUp:
            is_sleeping_ = false;
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Waking Up...");
            break;
        case AgentHelper::GeneralAction::Stop:
            is_stopped_ = true;
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::HideEmoji);
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Stopping...");
            break;
        default:
            break;
        }
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::GeneralActionTriggered, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent general action triggered event");
    }
}

void AI_Agents::process_emote_when_general_event_happened()
{
    auto slot = [this](const std::string &event_name, const std::string &general_event, bool is_unexpected)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD(
            "Params: event_name(%1%), general_event(%2%), is_unexpected(%3%)", event_name, general_event,
            BROOKESIA_DESCRIBE_TO_STR(is_unexpected));

        AgentHelper::GeneralEvent general_event_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(general_event, general_event_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert general event: %1%", general_event);

        switch (general_event_enum)
        {
        case AgentHelper::GeneralEvent::TimeSynced:
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Time Synced");
            break;
        }
        case AgentHelper::GeneralEvent::Activated:
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Activated");
            break;
        }
        case AgentHelper::GeneralEvent::Started:
        {
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::SetEmoji, "neutral");
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Started");
            break;
        }
        case AgentHelper::GeneralEvent::Awake:
        {
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::SetEmoji, "neutral");
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Awake");
            break;
        }
        case AgentHelper::GeneralEvent::Stopped:
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage, BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Idle));
            break;
        }
        default:
            break;
        }
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::GeneralEventHappened, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent general event happened event");
    }
}

void AI_Agents::process_emote_when_suspend_status_changed()
{
    auto slot = [this](const std::string &event_name, bool is_suspended)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), is_suspended(%2%)", event_name, is_suspended);

        BROOKESIA_LOGI("Suspend status changed to %1%", BROOKESIA_DESCRIBE_TO_STR(is_suspended));

        if (is_suspended)
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), "[Agent] Suspended");
        }
        else
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Idle));
        }
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::SuspendStatusChanged, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent suspend status changed event");
    }
}

void AI_Agents::process_emote_when_speaking_status_changed()
{
    auto slot = [this](const std::string &event_name, bool is_speaking)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        auto speaking_status = BROOKESIA_DESCRIBE_TO_STR(is_speaking);
        BROOKESIA_LOGD(
            "Params: event_name(%1%), is_speaking(%2%)", event_name, speaking_status);

        BROOKESIA_LOGI("Speaking status changed to %1%", speaking_status);

        if (is_inactive())
        {
            BROOKESIA_LOGD("Agent is inactive, skip");
            return;
        }

        if (is_speaking)
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Speak));
        }
        else if (!is_listening())
        {
            // Only hide event message when the agent is not listening
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::HideEventMessage);
        }
        is_speaking_ = is_speaking;
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::SpeakingStatusChanged, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent speaking status changed event");
    }
}

void AI_Agents::process_emote_when_listening_status_changed()
{
    auto slot = [this](const std::string &event_name, bool is_listening)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        auto listening_status = BROOKESIA_DESCRIBE_TO_STR(is_listening);
        BROOKESIA_LOGD(
            "Params: event_name(%1%), is_listening(%2%)", event_name, listening_status);

        BROOKESIA_LOGI("Listening status changed to %1%", listening_status);

        if (is_inactive())
        {
            BROOKESIA_LOGD("Agent is inactive, skip");
            return;
        }

        if (is_listening)
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Listen));
        }
        else if (!is_speaking())
        {
            // Only hide event message when the agent is not speaking
            EmoteHelper::call_function_async(EmoteHelper::FunctionId::HideEventMessage);
        }
        is_listening_ = is_listening;
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::ListeningStatusChanged, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent listening status changed event");
    }
}

void AI_Agents::process_emote_when_agent_speaking_text_got()
{
    auto slot = [this](const std::string &event_name, const std::string &text)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), text(%2%)", event_name, text);

        BROOKESIA_LOGI("Got agent speaking text: %1%", text);

        if (is_inactive())
        {
            BROOKESIA_LOGD("Agent is inactive, skip");
            return;
        }

        EmoteHelper::call_function_async(
            EmoteHelper::FunctionId::SetEventMessage,
            BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Speak), text);
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::AgentSpeakingTextGot, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent agent speaking text got event");
    }
}

void AI_Agents::process_emote_when_user_speaking_text_got()
{
    auto slot = [this](const std::string &event_name, const std::string &text)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), text(%2%)", event_name, text);

        BROOKESIA_LOGI("Got user speaking text: %1%", text);

        if (is_inactive())
        {
            BROOKESIA_LOGD("Agent is inactive, skip");
            return;
        }

        EmoteHelper::call_function_async(
            EmoteHelper::FunctionId::SetEventMessage,
            BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::User), text);
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::UserSpeakingTextGot, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent user speaking text got event");
    }
}

void AI_Agents::process_emote_when_power_battery_state_changed()
{
    auto slot = [this](const std::string &event_name, const boost::json::object &state)
    {
        BROOKESIA_LOG_TRACE_GUARD();
        BROOKESIA_LOGD("Params: event_name(%1%), state(%2%)", event_name, state);

        if (!is_inactive())
        {
            BROOKESIA_LOGD("Agent is inactive, skip");
        }

        hal::power::BatteryIface::State battery_state;
        auto parse_result = BROOKESIA_DESCRIBE_FROM_JSON(state, battery_state);
        BROOKESIA_CHECK_FALSE_EXIT(parse_result, "Failed to parse power battery state");

        std::string battery_message = "";
        if ((battery_state.charge_state == hal::power::BatteryIface::ChargeState::Unknown) ||
            (battery_state.charge_state == hal::power::BatteryIface::ChargeState::NotCharging))
        {
            battery_message = "0,";
        }
        else
        {
            battery_message = "1,";
        }
        battery_message += std::to_string(battery_state.percentage.value_or(0));
        BROOKESIA_LOGD("Battery message: %1%", battery_message);

        EmoteHelper::call_function_async(
            EmoteHelper::FunctionId::SetEventMessage, BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::Battery),
            battery_message);
    };
    auto connection = DeviceHelper::subscribe_event(DeviceHelper::EventId::PowerBatteryStateChanged, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent power battery state changed event");
    }
}

void AI_Agents::process_emote_when_emote_got()
{
    auto slot = [this](const std::string &event_name, std::string emote)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), emote(%2%)", event_name, emote);

        BROOKESIA_LOGI("Got emote: %1%", emote);

        if (is_inactive())
        {
            BROOKESIA_LOGD("Agent is sleeping or suspended or stopped, skip");
            return;
        }

        // There is no "cool" emote in the emote.json file, so we need to replace it with "winking"
        if (emote == "cool")
        {
            emote = "winking";
        }

        EmoteHelper::call_function_async(
            EmoteHelper::FunctionId::InsertAnimation, emote, emote_animation_duration_ms_);
    };
    auto connection = AgentHelper::subscribe_event(AgentHelper::EventId::EmoteGot, slot);
    if (connection.connected())
    {
        service_connections_.push_back(std::move(connection));
    }
    else
    {
        BROOKESIA_LOGE("Failed to subscribe to Agent emote got event");
    }
}

void AI_Agents::process_emote()
{
    if (!EmoteHelper::is_available())
    {
        BROOKESIA_LOGW("Emote service is not available, skip");
        return;
    }

    process_emote_when_general_action_triggered();
    process_emote_when_general_event_happened();
    process_emote_when_suspend_status_changed();
    process_emote_when_speaking_status_changed();
    process_emote_when_listening_status_changed();
    process_emote_when_agent_speaking_text_got();
    process_emote_when_user_speaking_text_got();
    process_emote_when_emote_got();
    process_emote_when_power_battery_state_changed();
}

void AI_Agents::process_wifi_events()
{
    auto general_action_triggered_slot =
        [this](const std::string &event_name, const std::string &general_action)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), general_action(%2%)", event_name, general_action);

        WifiHelper::GeneralAction general_action_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(general_action, general_action_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert general action: %1%", general_action);

        std::string emote_message = "";
        lib_utils::TaskScheduler::OnceTask task_func;

        switch (general_action_enum)
        {
        case WifiHelper::GeneralAction::Start:
            emote_message = "[WiFi] Starting...";
            break;
        case WifiHelper::GeneralAction::Connect:
            task_func = [this]()
            {
                BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

                auto get_ap_result = WifiHelper::call_function_sync<boost::json::object>(
                    WifiHelper::FunctionId::GetConnectAp);
                BROOKESIA_CHECK_FALSE_EXIT(get_ap_result, "Failed to get connect AP");

                WifiHelper::ConnectApInfo connect_ap_info;
                auto parse_result = BROOKESIA_DESCRIBE_FROM_JSON(get_ap_result.value(), connect_ap_info);
                BROOKESIA_CHECK_FALSE_EXIT(parse_result, "Failed to parse connect AP info");

                auto emote_message = (boost::format("[WiFi] Connecting to %1% ...") % connect_ap_info.ssid).str();
                auto set_emote_result = EmoteHelper::call_function_async(
                    EmoteHelper::FunctionId::SetEventMessage,
                    BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), emote_message);
                BROOKESIA_CHECK_FALSE_EXIT(set_emote_result, "Failed to set event message");
            };
            break;
        case WifiHelper::GeneralAction::Disconnect:
            emote_message = "[WiFi] Disconnecting...";
            break;
        case WifiHelper::GeneralAction::Stop:
            emote_message = "[WiFi] Stopping...";
            break;
        default:
            break;
        }

        if (!emote_message.empty())
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), emote_message);
        }
        if (task_func)
        {
            auto result = task_scheduler_->post(std::move(task_func));
            BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to post task function");
        }
    };
    auto general_action_triggered_connection = WifiHelper::subscribe_event(
        WifiHelper::EventId::GeneralActionTriggered, general_action_triggered_slot);
    BROOKESIA_CHECK_FALSE_EXIT(
        general_action_triggered_connection.connected(), "Failed to subscribe to Agent WiFi general action triggered event");
    service_connections_.push_back(std::move(general_action_triggered_connection));

    auto general_event_happened_slot =
        [this](const std::string &event_name, const std::string &wifi_event, bool is_unexpected)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD(
            "Params: event_name(%1%), wifi_event(%2%), is_unexpected(%3%)", event_name, wifi_event, is_unexpected);

        WifiHelper::GeneralEvent wifi_event_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(wifi_event, wifi_event_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert wifi event: %1%", wifi_event);

        std::string emote_message = "";
        lib_utils::TaskScheduler::OnceTask task_func;

        switch (wifi_event_enum)
        {
        case WifiHelper::GeneralEvent::Connected:
        {
            emote_message = "[WiFi] Connected";
            is_wifi_connected_ = true;
            task_func = [this]()
            {
                BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
                BROOKESIA_CHECK_FALSE_EXIT(start_agent(), "Failed to start agent");
            };
            break;
        }
        case WifiHelper::GeneralEvent::Disconnected:
        {
            emote_message = "[WiFi] Disconnected";
            is_wifi_connected_ = false;
            task_func = [this]()
            {
                BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
                stop_agent();
            };
            break;
        }
        default:
            break;
        }

        if (!emote_message.empty())
        {
            EmoteHelper::call_function_async(
                EmoteHelper::FunctionId::SetEventMessage,
                BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), emote_message);
        }
        if (task_func)
        {
            auto result = task_scheduler_->post(std::move(task_func));
            BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to post task function");
        }
    };
    auto general_event_happened_connection = WifiHelper::subscribe_event(
        WifiHelper::EventId::GeneralEventHappened, general_event_happened_slot);
    BROOKESIA_CHECK_FALSE_EXIT(
        general_event_happened_connection.connected(), "Failed to subscribe to Agent WiFi general event happened event");
    service_connections_.push_back(std::move(general_event_happened_connection));

    auto soft_ap_event_happened_slot =
        [this](const std::string &event_name, const std::string &soft_ap_event)
    {
        BROOKESIA_LOG_TRACE_GUARD();

        BROOKESIA_LOGD("Params: event_name(%1%), soft_ap_event(%2%)", event_name, soft_ap_event);

        WifiHelper::SoftApEvent soft_ap_event_enum;
        auto convert_result = BROOKESIA_DESCRIBE_STR_TO_ENUM(soft_ap_event, soft_ap_event_enum);
        BROOKESIA_CHECK_FALSE_EXIT(convert_result, "Failed to convert soft AP event: %1%", soft_ap_event);

        switch (soft_ap_event_enum)
        {
        case WifiHelper::SoftApEvent::Started:
        {
            auto task_func = [this]()
            {
                BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
                auto get_soft_ap_params_result = WifiHelper::call_function_sync<boost::json::object>(
                    WifiHelper::FunctionId::GetSoftApParams);
                BROOKESIA_CHECK_FALSE_EXIT(get_soft_ap_params_result, "Failed to get soft AP params");

                WifiHelper::SoftApParams soft_ap_params;
                auto parse_result = BROOKESIA_DESCRIBE_FROM_JSON(get_soft_ap_params_result.value(), soft_ap_params);
                BROOKESIA_CHECK_FALSE_EXIT(parse_result, "Failed to parse soft AP params");

                auto emote_message = (boost::format("[WiFi] SoftAP started. Please scan the QR code or connect to %1%(PWD:%2%)") %
                                      soft_ap_params.ssid % soft_ap_params.password)
                                         .str();
                auto set_emote_result = EmoteHelper::call_function_async(
                    EmoteHelper::FunctionId::SetEventMessage,
                    BROOKESIA_DESCRIBE_TO_STR(EmoteHelper::EventMessageType::System), emote_message);
                BROOKESIA_CHECK_FALSE_EXIT(set_emote_result, "Failed to set event message");

                std::string qr_string = "WIFI:T:nopass;S:" + soft_ap_params.ssid + ";P:" + soft_ap_params.password + ";;";
                auto set_qr_result = EmoteHelper::call_function_async(
                    EmoteHelper::FunctionId::SetQrcode, qr_string);
                BROOKESIA_CHECK_FALSE_EXIT(set_qr_result, "Failed to set QR code");
            };
            auto result = task_scheduler_->post(std::move(task_func));
            BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to post task function");
            break;
        }
        default:
            break;
        }
    };
    auto soft_ap_event_happened_connection = WifiHelper::subscribe_event(
        WifiHelper::EventId::SoftApEventHappened, soft_ap_event_happened_slot);
    BROOKESIA_CHECK_FALSE_EXIT(
        soft_ap_event_happened_connection.connected(), "Failed to subscribe to Agent WiFi soft AP event happened event");
    service_connections_.push_back(std::move(soft_ap_event_happened_connection));
}

void AI_Agents::process_touch_pad_events()
{
    auto touch_pad_event_slot = [this](TouchPad::Event event)
    {
        switch (event)
        {
        case TouchPad::Event::Touch:
            toggle_custom_ia_manual_listening();
            break;
        case TouchPad::Event::SlidePad1ToPad2:
            BROOKESIA_LOGD("Touch pad slide: pad1 -> pad2");
            break;
        case TouchPad::Event::SlidePad2ToPad1:
            BROOKESIA_LOGD("Touch pad slide: pad2 -> pad1");
            break;
        }
    };

    auto start_result = TouchPad::get_instance().start({
        .task_scheduler = task_scheduler_,
        .event_callback = touch_pad_event_slot,
    });
    BROOKESIA_CHECK_FALSE_EXIT(start_result, "Failed to start touch pad");
}

void AI_Agents::toggle_custom_ia_manual_listening()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    // Simple touch is a toggle (tap to start, tap again to stop), unlike the screen's press-and-hold.
    auto active_agent_result = AgentHelper::call_function_sync<std::string>(AgentHelper::FunctionId::GetActiveAgent);
    if (!active_agent_result || (active_agent_result.value() != CustomIAHelper::get_name()))
    {
        BROOKESIA_LOGD("Active agent is not CustomIA, ignore touch pad tap");
        return;
    }

    auto listening_result = AgentHelper::call_function_sync<bool>(AgentHelper::FunctionId::GetListeningStatus);
    bool is_listening = listening_result.value_or(false);

    auto function_id = is_listening ? AgentHelper::FunctionId::ManualStopListening : AgentHelper::FunctionId::ManualStartListening;
    auto result = AgentHelper::call_function_async(function_id);
    BROOKESIA_CHECK_FALSE_EXIT(result, "Failed to toggle manual listening from touch pad");
}

boost::json::object AI_Agents::get_agent_custom_ia_info()
{
#if CONFIG_EXAMPLE_AGENTS_ENABLE_CUSTOM_IA
    CustomIAHelper::Info custom_ia_info{
        .server_url = CONFIG_EXAMPLE_AGENTS_CUSTOM_IA_SERVER_URL,
    };

    if (auto server_url = SdSettings::get_instance().get_string(SD_SETTINGS_CUSTOM_IA_SERVER_URL_KEY);
        server_url.has_value())
    {
        BROOKESIA_LOGI("Overriding CustomIA server URL from SD settings: %1%", *server_url);
        custom_ia_info.server_url = std::move(*server_url);
    }

    return BROOKESIA_DESCRIBE_TO_JSON(custom_ia_info).as_object();
#else
    return boost::json::object{};
#endif // CONFIG_EXAMPLE_AGENTS_ENABLE_CUSTOM_IA
}
