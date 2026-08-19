/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <optional>
#include <string>

#include "boost/json/object.hpp"

// Loads and caches the `settings.json` file from the root of the SD card, exposing simple typed
// lookups so other modules can read runtime overrides without duplicating file/JSON handling.
class SdSettings
{
public:
    static constexpr const char *kDefaultPath = "/sdcard/settings.json";

    static SdSettings &get_instance()
    {
        static SdSettings instance;
        return instance;
    }

    // (Re)loads and parses the settings file from the SD card. Returns false if the file is
    // missing or not valid JSON; cached values are left untouched in that case.
    bool reload(const std::string &path = kDefaultPath);

    std::optional<std::string> get_string(const std::string &key);

private:
    SdSettings() = default;
    ~SdSettings() = default;
    SdSettings(const SdSettings &) = delete;
    SdSettings &operator=(const SdSettings &) = delete;

    void ensure_loaded();

    bool loaded_ = false;
    boost::json::object settings_;
};
