/**
 * @file    core/logging.h
 * @brief   MGRN_* logging macros bound to the "native" log category.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR: developer-only messages are enabled by MGR_NATIVE_DEV=1
 * instead of re:Blue's devmode setting.
 */
#pragma once
#include <cstdlib>

#include <rex/logging.h>

REXLOG_DEFINE_CATEGORY(native)

#define MGRN_TRACE(...) REXLOG_CAT_TRACE(::rex::log::native(), __VA_ARGS__)
#define MGRN_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::native(), __VA_ARGS__)
#define MGRN_INFO(...) REXLOG_CAT_INFO(::rex::log::native(), __VA_ARGS__)
#define MGRN_WARN(...) REXLOG_CAT_WARN(::rex::log::native(), __VA_ARGS__)
#define MGRN_ERROR(...) REXLOG_CAT_ERROR(::rex::log::native(), __VA_ARGS__)
#define MGRN_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::native(), __VA_ARGS__)

namespace mgrn {
inline bool DevMode() {
  static const bool enabled = [] {
    const char *value = std::getenv("MGR_NATIVE_DEV");
    return value && value[0] == '1';
  }();
  return enabled;
}
} // namespace mgrn

#define MGRN_DEV_WARN(...)                                                     \
  do {                                                                         \
    if (::mgrn::DevMode())                                                     \
      MGRN_WARN(__VA_ARGS__);                                                  \
  } while (0)

#define MGRN_DEV_INFO(...)                                                     \
  do {                                                                         \
    if (::mgrn::DevMode())                                                     \
      MGRN_INFO(__VA_ARGS__);                                                  \
  } while (0)
