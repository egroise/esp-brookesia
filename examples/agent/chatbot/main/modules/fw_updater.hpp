/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <atomic>
#include <memory>

#include "brookesia/service_manager/event/registry.hpp"
#include "brookesia/service_manager/service/manager.hpp"

// On the first Wi-Fi connection, checks the firmware update web service for a newer firmware
// version than the one recorded in /sdcard/VERSION (created with "0" if missing). If a newer
// version is available: downloads it to the SD card (verifying its MD5), copies it into the
// inactive ota_0/ota_1 partition (verifying the MD5 of what landed in flash), switches the
// bootloader to boot from that partition, and reboots.
class FwUpdater
{
public:
    struct Config
    {
        std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler;
    };

    static FwUpdater &get_instance()
    {
        static FwUpdater instance;
        return instance;
    }

    bool init(const Config &config);

    bool is_initialized() const
    {
        return is_initialized_.load();
    }

    // Subscribes to the Wi-Fi "Connected" event; the update check runs once, the first time it fires.
    // No-op (but still returns true) when `CONFIG_EXAMPLE_FW_UPDATE_ENABLE` is disabled.
    bool start();

private:
    FwUpdater() = default;
    ~FwUpdater() = default;
    FwUpdater(const FwUpdater &) = delete;
    FwUpdater &operator=(const FwUpdater &) = delete;

    void run_update_check();

    std::atomic<bool> is_initialized_{false};
    std::atomic<bool> has_run_{false};
    Config config_{};
    // Holds the active event subscription; reset() to unsubscribe once the update check has run
    std::shared_ptr<esp_brookesia::service::EventRegistry::SignalConnection> active_conn_;
};
