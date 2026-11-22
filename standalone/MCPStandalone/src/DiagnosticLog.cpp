#include "DiagnosticLog.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace {

std::mutex g_log_lock;
std::filesystem::path g_log_path;
std::filesystem::path g_backup_path;
std::ofstream g_log_stream;
std::uintmax_t g_max_bytes = 2U * 1024U * 1024U;
std::atomic<std::uint64_t> g_error_count{0};

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) %
        1000;

    std::tm local = {};
    localtime_s(&local, &time);

    std::ostringstream stream;
    stream.fill('0');
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << milliseconds.count();
    return stream.str();
}

void OpenLocked() {
    g_log_stream.open(g_log_path, std::ios::out | std::ios::app | std::ios::binary);
}

void RotateIfNeededLocked(std::size_t incoming_size) {
    if (!g_log_stream.is_open() || g_log_path.empty())
        return;

    g_log_stream.flush();
    std::error_code size_error;
    const auto current_size = std::filesystem::file_size(g_log_path, size_error);
    if (size_error || current_size == 0)
        return;
    if (current_size + incoming_size <= g_max_bytes)
        return;

    g_log_stream.close();
    std::error_code remove_error;
    std::filesystem::remove(g_backup_path, remove_error);
    std::error_code rename_error;
    std::filesystem::rename(g_log_path, g_backup_path, rename_error);
    OpenLocked();
}

}  // namespace

void DiagnosticLog::Initialize(const std::filesystem::path& path,
                               std::uintmax_t max_bytes) {
    std::lock_guard lock(g_log_lock);
    if (g_log_stream.is_open())
        g_log_stream.close();

    g_log_path = path;
    g_backup_path = path.string() + ".1";
    g_max_bytes = max_bytes == 0 ? 1 : max_bytes;
    g_error_count.store(0, std::memory_order_relaxed);

    std::error_code error;
    if (g_log_path.has_parent_path())
        std::filesystem::create_directories(g_log_path.parent_path(), error);
    OpenLocked();
}

void DiagnosticLog::Shutdown() {
    std::lock_guard lock(g_log_lock);
    if (g_log_stream.is_open()) {
        g_log_stream.flush();
        g_log_stream.close();
    }
}

void DiagnosticLog::Info(std::string_view message) {
    Write("INFO", message);
}

void DiagnosticLog::Warn(std::string_view message) {
    Write("WARN", message);
}

void DiagnosticLog::Error(std::string_view message) {
    g_error_count.fetch_add(1, std::memory_order_relaxed);
    Write("ERROR", message);
}

std::filesystem::path DiagnosticLog::Path() {
    std::lock_guard lock(g_log_lock);
    return g_log_path;
}

std::uint64_t DiagnosticLog::ErrorCount() {
    return g_error_count.load(std::memory_order_relaxed);
}

void DiagnosticLog::Write(std::string_view level, std::string_view message) {
    std::lock_guard lock(g_log_lock);
    if (!g_log_stream.is_open())
        return;

    std::ostringstream line;
    line << '[' << CurrentTimestamp() << "] [" << level << "] ";
    line.write(message.data(), static_cast<std::streamsize>(message.size()));
    line << '\n';
    const auto text = line.str();
    RotateIfNeededLocked(text.size());
    if (!g_log_stream.is_open())
        return;
    g_log_stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    g_log_stream.flush();
}
