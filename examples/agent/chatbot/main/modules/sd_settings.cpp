/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#define BROOKESIA_LOG_TAG "SdSettings"
#include <fstream>
#include <sstream>
#include <utility>

#include "boost/json.hpp"
#include "brookesia/lib_utils/log.hpp"
#include "sd_settings.hpp"

bool SdSettings::reload(const std::string &path)
{
    std::ifstream settings_file(path);
    if (!settings_file.is_open())
    {
        BROOKESIA_LOGW("SD settings file not found: %1%", path);
        return false;
    }

    std::ostringstream buffer;
    buffer << settings_file.rdbuf();

    boost::system::error_code error_code;
    auto parsed = boost::json::parse(buffer.str(), error_code);
    if (error_code || !parsed.is_object())
    {
        BROOKESIA_LOGW("Failed to parse SD settings file %1%: %2%", path, error_code.message());
        return false;
    }

    settings_ = std::move(parsed).as_object();
    loaded_ = true;

    return true;
}

std::optional<std::string> SdSettings::get_string(const std::string &key)
{
    ensure_loaded();

    if (!loaded_)
    {
        return std::nullopt;
    }

    const auto *value = settings_.if_contains(key);
    if ((value == nullptr) || !value->is_string() || value->as_string().empty())
    {
        return std::nullopt;
    }

    return std::string(value->as_string().c_str());
}

void SdSettings::ensure_loaded()
{
    if (!loaded_)
    {
        reload();
    }
}
