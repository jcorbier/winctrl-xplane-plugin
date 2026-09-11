#ifndef LOGGER_HPP
#define LOGGER_HPP

#include "config.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

// printf-style checking on the log methods; member functions count `this`, hence 2/3.
#if defined(__MINGW32__)
#define WINCTRL_PRINTF_FORMAT __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)))
#elif defined(__GNUC__) || defined(__clang__)
#define WINCTRL_PRINTF_FORMAT __attribute__((format(printf, 2, 3)))
#else
#define WINCTRL_PRINTF_FORMAT
#endif

enum class LogLevel {
    VERBOSE = 0,
    INFO = 1,
    WARN = 2,
    CRITICAL = 3,
};

// Thread-safe logger. X-Plane requires XPLMDebugString to be called from the
// main (simulator) thread only. Log calls made from any other thread are
// queued and flushed on the main thread via a flight-loop callback, so the
// underlying XPLMDebugString call always happens on the main thread.
//
// Call initialize() once from the main thread (XPluginStart) and destroy()
// from XPluginStop. Before initialize() runs there are no other threads yet,
// so logging falls back to emitting directly.
class Logger {
    public:
        static Logger *getInstance() {
            static Logger instance;
            return &instance;
        }

        void initialize() {
            mainThreadId = std::this_thread::get_id();
            XPLMRegisterFlightLoopCallback(flushCallback, -1.0f, nullptr);
            initialized = true;
        }

        void destroy() {
            initialized = false;
            XPLMUnregisterFlightLoopCallback(flushCallback, nullptr);
            flush(); // Drain anything still queued (we are on the main thread here).
        }

        void setLogLevel(LogLevel level) {
            currentLogLevel = level;
        }

        LogLevel getLogLevel() const {
            return currentLogLevel;
        }

        void debug(const char *format, ...) WINCTRL_PRINTF_FORMAT {
            va_list args;
            va_start(args, format);
            log(LogLevel::VERBOSE, format, args);
            va_end(args);
        }

        void info(const char *format, ...) WINCTRL_PRINTF_FORMAT {
            va_list args;
            va_start(args, format);
            log(LogLevel::INFO, format, args);
            va_end(args);
        }

        void warn(const char *format, ...) WINCTRL_PRINTF_FORMAT {
            va_list args;
            va_start(args, format);
            log(LogLevel::WARN, format, args);
            va_end(args);
        }

        void critical(const char *format, ...) WINCTRL_PRINTF_FORMAT {
            va_list args;
            va_start(args, format);
            log(LogLevel::CRITICAL, format, args);
            va_end(args);
        }

        void logForce(const char *format, ...) WINCTRL_PRINTF_FORMAT {
            va_list args;
            va_start(args, format);
            logInternal(LogLevel::CRITICAL, format, args, true);
            va_end(args);
        }

    private:
        // mainThreadId is set here as well as in initialize(): the singleton is constructed on
        // first use, which is the main thread in practice, and this keeps the thread check
        // meaningful for anything logged before initialize() runs.
        Logger() : mainThreadId(std::this_thread::get_id()) {}

        ~Logger() = default;
        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        void log(LogLevel level, const char *format, va_list args) {
            logInternal(level, format, args, false);
        }

        void logInternal(LogLevel level, const char *format, va_list args, bool force) {
            if (!force && level < currentLogLevel.load(std::memory_order_relaxed)) {
                return;
            }

            const char *levelStr = "";
            switch (level) {
                case LogLevel::VERBOSE:
                    levelStr = "Debug";
                    break;
                case LogLevel::INFO:
                    levelStr = "Info";
                    break;
                case LogLevel::WARN:
                    levelStr = "Warning";
                    break;
                case LogLevel::CRITICAL:
                    levelStr = "Error";
                    break;
            }

            char buffer[1024];
            vsnprintf(buffer, sizeof(buffer), format, args);

            char finalBuffer[1360];
            snprintf(finalBuffer, sizeof(finalBuffer), "[%s] %s: %s", PRODUCT_NAME, levelStr, buffer);

            if (std::this_thread::get_id() == mainThreadId) {
                // On the main thread: emit directly.
                emit(finalBuffer);
            } else if (initialized.load(std::memory_order_acquire)) {
                // Off the main thread: hand off to the flight-loop flush so
                // XPLMDebugString is only ever touched from the main thread.
                std::lock_guard<std::mutex> lock(queueMutex);
                pending.emplace(finalBuffer);
            } else {
                // Off the main thread while the plugin is down (destroy() has run) or not yet up:
                // no flush callback exists to drain the queue, and XPLMDebugString off the main
                // thread is fatal - X-Plane aborts with "forbidden to run loop on threads other
                // than event-loop thread". stdout is all that is left. The lock only keeps
                // concurrent lines from interleaving.
                std::lock_guard<std::mutex> lock(queueMutex);
                printf("%s", finalBuffer);
                fflush(stdout);
            }
        }

        // Only ever called on the main thread (directly, or from flush()).
        void emit(const char *message) {
            XPLMDebugString(message);
            printf("%s", message);
            fflush(stdout);
        }

        void flush() {
            std::queue<std::string> drained;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                std::swap(drained, pending);
            }

            while (!drained.empty()) {
                emit(drained.front().c_str());
                drained.pop();
            }
        }

        static float flushCallback(float, float, int, void *) {
            Logger::getInstance()->flush();
            return -1.0f; // Run again on the next flight loop.
        }

        std::mutex queueMutex;
        std::queue<std::string> pending;
        std::thread::id mainThreadId;
        std::atomic<bool> initialized{false};
        std::atomic<LogLevel> currentLogLevel{LogLevel::INFO};
};

#endif
