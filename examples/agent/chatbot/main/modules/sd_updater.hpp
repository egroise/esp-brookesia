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

// On the first Wi-Fi connection, fetches the list of files to update from the SD update web
// service, compares it (by MD5) against what's already under /sdcard/update, and downloads any
// file that's missing or out of date.
class SdUpdater
{
public:
    struct Config
    {
        std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> task_scheduler;
    };

    static SdUpdater &get_instance()
    {
        static SdUpdater instance;
        return instance;
    }

    bool init(const Config &config);

    bool is_initialized() const
    {
        return is_initialized_.load();
    }

    // Subscribes to the Wi-Fi "Connected" event; the update check runs once, the first time it fires.
    // No-op (but still returns true) when `CONFIG_EXAMPLE_SD_UPDATE_ENABLE` is disabled.
    bool start();

private:
    SdUpdater() = default;
    ~SdUpdater() = default;
    SdUpdater(const SdUpdater &) = delete;
    SdUpdater &operator=(const SdUpdater &) = delete;

    void run_update_check();

    std::atomic<bool> is_initialized_{false};
    std::atomic<bool> has_run_{false};
    Config config_{};
    // Holds the active event subscription; reset() to unsubscribe once the update check has run
    std::shared_ptr<esp_brookesia::service::EventRegistry::SignalConnection> active_conn_;
};
