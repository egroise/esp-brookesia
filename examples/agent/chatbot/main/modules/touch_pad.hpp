/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "driver/touch_sens.h"
#include "brookesia/lib_utils/task_scheduler.hpp"

// Two capacitive touch pads placed next to each other (TOUCH_PAD1 on IO6, TOUCH_PAD2 on IO7),
// exposing both a button-like tap and left/right slide gestures between them.
class TouchPad
{
public:
    enum class Event
    {
        Touch,             // Simple touch (tap) on either pad, like a button
        SlidePad1ToPad2,   // Slide gesture: finger moves from pad1 (IO6) to pad2 (IO7)
        SlidePad2ToPad1,   // Slide gesture: finger moves from pad2 (IO7) to pad1 (IO6)
    };

    using EventCallback = std::function<void(Event)>;

    struct Config
    {
        std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler;
        EventCallback event_callback;
    };

    static TouchPad &get_instance()
    {
        static TouchPad instance;
        return instance;
    }

    bool start(const Config &config);
    void stop();

    bool is_started() const
    {
        return (sens_handle_ != nullptr);
    }

private:
    TouchPad() = default;
    ~TouchPad() = default;
    TouchPad(const TouchPad &) = delete;
    TouchPad &operator=(const TouchPad &) = delete;

    struct ChannelSlot
    {
        touch_channel_handle_t handle = nullptr;
        int chan_id = -1;
        std::atomic_bool touched{false};
        std::atomic<int64_t> touch_start_us{0};
    };

    bool calibrate();
    void poll();
    void emit(Event event);
    void log_values();

    static bool on_touch_active(
        touch_sensor_handle_t sens_handle, const touch_active_event_data_t *event, void *user_ctx);
    static bool on_touch_inactive(
        touch_sensor_handle_t sens_handle, const touch_inactive_event_data_t *event, void *user_ctx);

    std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler_;
    EventCallback event_callback_;
    esp_brookesia::lib_utils::TaskScheduler::TaskId poll_task_id_{};

    touch_sensor_handle_t sens_handle_ = nullptr;
    ChannelSlot pad1_slot_; // IO6
    ChannelSlot pad2_slot_; // IO7

    bool prev_pad1_touched_ = false;
    bool prev_pad2_touched_ = false;
    bool slide_emitted_ = false;
    int64_t last_log_us_ = 0;
};
