/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"

/**
 * @brief Enable automatic plugin registration for the CustomIA agent component.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER)
#       define BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER  (0)
#   endif
#endif

/**
 * @brief Linker symbol exported when automatic plugin registration is enabled.
 */
#if BROOKESIA_AGENT_CUSTOM_IA_ENABLE_AUTO_REGISTER
#   if !defined(BROOKESIA_AGENT_CUSTOM_IA_PLUGIN_SYMBOL)
#       define BROOKESIA_AGENT_CUSTOM_IA_PLUGIN_SYMBOL  agent_custom_ia_symbol
#   endif
#endif

/**
 * @brief Default log tag used by the CustomIA agent component.
 */
#define BROOKESIA_AGENT_CUSTOM_IA_LOG_TAG "CustomIA"

#if !defined(BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG)
#       define BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_ENABLE_DEBUG_LOG  (0)
#   endif
#endif

/**
 * @brief Uplink (recording) sample rate in Hz sent to the backend's STT endpoint.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE)
#       define BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_STT_SAMPLE_RATE  (16000)
#   endif
#endif

/**
 * @brief Downlink (playback) sample rate in Hz assumed for the backend's TTS WAV replies.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE)
#       define BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_TTS_SAMPLE_RATE  (24000)
#   endif
#endif

/**
 * @brief Maximum recording duration in milliseconds, used to bound the uplink PCM buffer size.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS)
#       define BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_MAX_RECORD_MS  (15000)
#   endif
#endif

/**
 * @brief Timeout in milliseconds applied to each individual HTTP request.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS)
#       define BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_HTTP_TIMEOUT_MS  (10000)
#   endif
#endif

/**
 * @brief Interval in milliseconds between `/out/{callId}` polls while waiting for the TTS reply.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS)
#       define BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_POLL_INTERVAL_MS  (300)
#   endif
#endif

/**
 * @brief Maximum total time in milliseconds to wait for the TTS reply before giving up.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS)
#       define BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_POLL_TIMEOUT_MS  (30000)
#   endif
#endif

/**
 * @brief Stack size in bytes of the dedicated worker task that performs the blocking HTTP work.
 */
#if !defined(BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE)
#   if defined(CONFIG_BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE)
#       define BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE  CONFIG_BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE
#   else
#       define BROOKESIA_AGENT_CUSTOM_IA_WORKER_TASK_STACK_SIZE  (12288)
#   endif
#endif
