#pragma once

#include <string>
#include <utility>

#include "logging.h"

#ifdef USE_QT
#include <QByteArray>
#endif

namespace util
{
std::string hmac_sha256(std::string_view msg, std::string_view key);
std::string base64_raw_url_encode(std::string const& text);
std::string base64_encode(std::string const& text);
#ifdef USE_QT
QByteArray base64_raw_url_encode(QByteArray const& text);
QByteArray base64_encode(QByteArray const& text);
#endif

/**
 * @brief Executes the given function with arguments and logs any exception.
 *
 * If the function throws an exception it is logged and rethrown.
 * Handles both standard exception types and unknown exception types.
 */
template <typename F>
void log_exception_and_rethrow(std::string label, F&& f)
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
