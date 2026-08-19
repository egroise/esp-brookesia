/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "esp_timer.h"
#include "private/utils.hpp"
#include "touch_pad.hpp"

using namespace esp_brookesia;

namespace
{

    // TOUCH_PAD1 = IO6, TOUCH_PAD2 = IO7. On ESP32-S3, touch channel N maps directly to GPIO N.
    constexpr int PAD1_CHAN_ID = 6;
    constexpr int PAD2_CHAN_ID = 7;

    constexpr uint32_t TOUCH_POLL_INTERVAL_MS = 20;
    constexpr uint32_t TOUCH_LOG_INTERVAL_MS = 250;
    // Max delay between the two pads' touch-down edges for the gesture to still count as a slide.
    constexpr int64_t SLIDE_WINDOW_US = 300 * 1000;

    // Number of oneshot scans used to seed the benchmark before deriving the active threshold from it.
    constexpr uint32_t CHAN_INIT_SCAN_TIMES = 3;
    // Active threshold as a ratio of the measured benchmark (same starting estimate as Espressif's
    // touch_sens_basic example); re-derived per channel in calibrate() to adapt to board variance.
    constexpr float ACTIVE_THRESH_TO_BENCHMARK_RATIO = 0.015f;

    touch_channel_config_t make_default_channel_config()
    {
        return {
            .active_thresh = {2000},
            .charge_speed = TOUCH_CHARGE_SPEED_7,
            .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        };
    }

} // namespace

bool TouchPad::start(const Config &config)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    BROOKESIA_CHECK_NULL_RETURN(config.task_scheduler, false, "Task scheduler is null");
    BROOKESIA_CHECK_FALSE_RETURN(!is_started(), false, "Touch pad is already started");

    task_scheduler_ = config.task_scheduler;
    event_callback_ = config.event_callback;

    // Tear down whatever was already brought up if any step below fails.
    lib_utils::FunctionGuard cleanup_guard([this]()
                                           { stop(); });

    touch_sensor_sample_config_t sample_cfg[1] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2)};
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_new_controller(&sens_cfg, &sens_handle_), false, "Failed to create touch sensor controller");

    pad1_slot_.chan_id = PAD1_CHAN_ID;
    pad2_slot_.chan_id = PAD2_CHAN_ID;

    auto chan_cfg = make_default_channel_config();
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_new_channel(sens_handle_, pad1_slot_.chan_id, &chan_cfg, &pad1_slot_.handle),
        false, "Failed to create touch channel for pad1 (chan %1%)", pad1_slot_.chan_id);
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_new_channel(sens_handle_, pad2_slot_.chan_id, &chan_cfg, &pad2_slot_.handle),
        false, "Failed to create touch channel for pad2 (chan %1%)", pad2_slot_.chan_id);

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_config_filter(sens_handle_, &filter_cfg), false, "Failed to configure touch sensor filter");

    // Seed the benchmark with an initial scan, then re-derive each channel's active threshold from the
    // measured benchmark. This adapts to per-board capacitance variance instead of a single fixed threshold.
    BROOKESIA_CHECK_FALSE_RETURN(calibrate(), false, "Failed to calibrate touch channels");

    touch_event_callbacks_t callbacks = {};
    callbacks.on_active = &TouchPad::on_touch_active;
    callbacks.on_inactive = &TouchPad::on_touch_inactive;
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_register_callbacks(sens_handle_, &callbacks, this), false,
        "Failed to register touch sensor callbacks");

    BROOKESIA_CHECK_ESP_ERR_RETURN(touch_sensor_enable(sens_handle_), false, "Failed to enable touch sensor");
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_start_continuous_scanning(sens_handle_), false, "Failed to start touch sensor scanning");

    last_log_us_ = esp_timer_get_time();
    auto poll_task = [this]()
    {
        poll();
        return true; // Keep this periodic task running
    };
    BROOKESIA_CHECK_FALSE_RETURN(
        task_scheduler_->post_periodic(poll_task, TOUCH_POLL_INTERVAL_MS, &poll_task_id_),
        false, "Failed to post touch pad poll task");

    cleanup_guard.release();

    BROOKESIA_LOGI(
        "Touch pad started: pad1(chan %1%), pad2(chan %2%), poll_interval=%3%ms, log_interval=%4%ms",
        pad1_slot_.chan_id, pad2_slot_.chan_id, TOUCH_POLL_INTERVAL_MS, TOUCH_LOG_INTERVAL_MS);
    return true;
}

void TouchPad::stop()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();

    if (task_scheduler_)
    {
        task_scheduler_->cancel(poll_task_id_);
    }

    if (sens_handle_ != nullptr)
    {
        touch_sensor_stop_continuous_scanning(sens_handle_);
        touch_sensor_disable(sens_handle_);
        if (pad1_slot_.handle != nullptr)
        {
            touch_sensor_del_channel(pad1_slot_.handle);
            pad1_slot_.handle = nullptr;
        }
        if (pad2_slot_.handle != nullptr)
        {
            touch_sensor_del_channel(pad2_slot_.handle);
            pad2_slot_.handle = nullptr;
        }
        touch_sensor_del_controller(sens_handle_);
        sens_handle_ = nullptr;
    }

    task_scheduler_.reset();
    event_callback_ = nullptr;
    prev_pad1_touched_ = false;
    prev_pad2_touched_ = false;
    slide_emitted_ = false;
}

bool TouchPad::calibrate()
{
    BROOKESIA_CHECK_ESP_ERR_RETURN(
        touch_sensor_enable(sens_handle_), false, "Failed to enable touch sensor for calibration");

    for (uint32_t i = 0; i < CHAN_INIT_SCAN_TIMES; ++i)
    {
        auto scan_ret = touch_sensor_trigger_oneshot_scanning(sens_handle_, 2000);
        if (scan_ret != ESP_OK)
        {
            BROOKESIA_LOGE("Failed to trigger touch sensor initial scan: %1%", esp_err_to_name(scan_ret));
            touch_sensor_disable(sens_handle_);
            return false;
        }
    }

    touch_sensor_disable(sens_handle_);

    for (auto *slot : {&pad1_slot_, &pad2_slot_})
    {
        uint32_t benchmark = 0;
        BROOKESIA_CHECK_ESP_ERR_RETURN(
            touch_channel_read_data(slot->handle, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark),
            false, "Failed to read benchmark for touch channel %1%", slot->chan_id);

        auto chan_cfg = make_default_channel_config();
        chan_cfg.active_thresh[0] = static_cast<uint32_t>(benchmark * ACTIVE_THRESH_TO_BENCHMARK_RATIO);
        BROOKESIA_CHECK_ESP_ERR_RETURN(
            touch_sensor_reconfig_channel(slot->handle, &chan_cfg), false,
            "Failed to reconfigure touch channel %1%", slot->chan_id);

        BROOKESIA_LOGI(
            "Touch channel %1% calibrated: benchmark=%2%, active_thresh=%3%",
            slot->chan_id, benchmark, chan_cfg.active_thresh[0]);
    }

    return true;
}

bool TouchPad::on_touch_active(touch_sensor_handle_t sens_handle, const touch_active_event_data_t *event, void *user_ctx)
{
    auto *self = static_cast<TouchPad *>(user_ctx);
    auto &slot = (event->chan_id == self->pad1_slot_.chan_id) ? self->pad1_slot_ : self->pad2_slot_;
    slot.touch_start_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    slot.touched.store(true, std::memory_order_relaxed);
    return false;
}

bool TouchPad::on_touch_inactive(touch_sensor_handle_t sens_handle, const touch_inactive_event_data_t *event, void *user_ctx)
{
    auto *self = static_cast<TouchPad *>(user_ctx);
    auto &slot = (event->chan_id == self->pad1_slot_.chan_id) ? self->pad1_slot_ : self->pad2_slot_;
    slot.touched.store(false, std::memory_order_relaxed);
    return false;
}

void TouchPad::poll()
{
    bool touched1 = pad1_slot_.touched.load(std::memory_order_relaxed);
    bool touched2 = pad2_slot_.touched.load(std::memory_order_relaxed);
    int64_t now_us = esp_timer_get_time();

    bool edge1_down = touched1 && !prev_pad1_touched_;
    bool edge1_up = !touched1 && prev_pad1_touched_;
    bool edge2_down = touched2 && !prev_pad2_touched_;
    bool edge2_up = !touched2 && prev_pad2_touched_;

    // Pad2 was already down when pad1 came down within the slide window -> pad2 -> pad1 slide.
    if (edge1_down && touched2 && !slide_emitted_)
    {
        int64_t pad2_start_us = pad2_slot_.touch_start_us.load(std::memory_order_relaxed);
        if ((pad2_start_us > 0) && ((now_us - pad2_start_us) <= SLIDE_WINDOW_US))
        {
            emit(Event::SlidePad2ToPad1);
            slide_emitted_ = true;
        }
    }
    // Pad1 was already down when pad2 came down within the slide window -> pad1 -> pad2 slide.
    if (edge2_down && touched1 && !slide_emitted_)
    {
        int64_t pad1_start_us = pad1_slot_.touch_start_us.load(std::memory_order_relaxed);
        if ((pad1_start_us > 0) && ((now_us - pad1_start_us) <= SLIDE_WINDOW_US))
        {
            emit(Event::SlidePad1ToPad2);
            slide_emitted_ = true;
        }
    }

    // A pad released alone (the other pad never joined in) without a slide having been detected is a tap.
    if (edge1_up && !slide_emitted_ && !touched2)
    {
        emit(Event::Touch);
    }
    if (edge2_up && !slide_emitted_ && !touched1)
    {
        emit(Event::Touch);
    }

    if (!touched1 && !touched2)
    {
        slide_emitted_ = false;
    }

    prev_pad1_touched_ = touched1;
    prev_pad2_touched_ = touched2;

    if ((now_us - last_log_us_) >= static_cast<int64_t>(TOUCH_LOG_INTERVAL_MS) * 1000)
    {
        // log_values();
        last_log_us_ = now_us;
    }
}

void TouchPad::log_values()
{
    uint32_t pad1_raw = 0, pad1_smooth = 0, pad1_benchmark = 0;
    uint32_t pad2_raw = 0, pad2_smooth = 0, pad2_benchmark = 0;

    touch_channel_read_data(pad1_slot_.handle, TOUCH_CHAN_DATA_TYPE_RAW, &pad1_raw);
    touch_channel_read_data(pad1_slot_.handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &pad1_smooth);
    touch_channel_read_data(pad1_slot_.handle, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &pad1_benchmark);
    touch_channel_read_data(pad2_slot_.handle, TOUCH_CHAN_DATA_TYPE_RAW, &pad2_raw);
    touch_channel_read_data(pad2_slot_.handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &pad2_smooth);
    touch_channel_read_data(pad2_slot_.handle, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &pad2_benchmark);

    BROOKESIA_LOGI(
        "Touch pad values: pad1[raw=%1%, smooth=%2%, bm=%3%, touched=%4%] pad2[raw=%5%, smooth=%6%, bm=%7%, touched=%8%]",
        pad1_raw, pad1_smooth, pad1_benchmark, pad1_slot_.touched.load(std::memory_order_relaxed),
        pad2_raw, pad2_smooth, pad2_benchmark, pad2_slot_.touched.load(std::memory_order_relaxed));
}

void TouchPad::emit(Event event)
{
    if (event_callback_)
    {
        event_callback_(event);
    }
}
