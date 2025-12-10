#pragma once

#include <string>

#include "logging.h"

namespace util
{
std::string hmac_sha256(std::string_view msg, std::string_view key);

/**
 * @brief Executes the given function with arguments and logs any exception.
 *
 * If the function throws an exception it is logged and rethrown.
 * Handles both standard exception types and unknown exception types.
 */
template <typename F>
void log_exception_and_rethrow(const char* label, F&& f)
{
    try {
        f();
    }
    catch (std::exception const& e) {
        loon_log_macro(
            Fatal, loon::log_level(), loon::log_message, loon::Logger)
            << label << ": " << e.what();
        throw e;
    }
    catch (...) {
        loon_log_macro(
            Fatal, loon::log_level(), loon::log_message, loon::Logger)
            << label << ": unknown exception";
        throw std::runtime_error("unknown error");
    }
}
} // namespace util
