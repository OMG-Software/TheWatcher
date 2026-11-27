// Copyright(c) 2016-2026, James Chapman
//
// Use of this source code is governed by a BSD -
// style license that can be found in the LICENSE file or
// at https://choosealicense.com/licenses/bsd-3-clause/

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace thewatcher
{

namespace logging
{
    constexpr auto LoggerInternalBufferSize = 10240;
    constexpr size_t k_maxLogQueueSize = 4096;

    /**
     * Levels of logging available
     */
    enum class LogLevel
    {
        L_TRACE = 100,
        L_DEBUG = 200,
        L_INFO = 300,
        L_NOTICE = 400,
        L_WARNING = 500,
        L_ERROR = 600,
        L_CRITICAL = 700,
        L_OFF = 1000
    };

    /**
     * Logger class
     */
    class SingleLog final
    {
    private:
        struct LogEntry
        {
            std::string message;
            bool flush{false};
        };
        static constexpr std::size_t LowSeverityFlushInterval{100};

        /**
         * Private Constructor
         */
        SingleLog()
            : m_consoleLogLevel(LogLevel::L_INFO)
            , m_fileLogLevel(LogLevel::L_TRACE)
            , m_filePath("")
        {
            m_consoleWriter = std::thread(&SingleLog::ConsoleWriter, this);
            m_fstreamWriter = std::thread(&SingleLog::FstreamWriter, this);
        }

        /**
         * Delete copy constructor
         */
        SingleLog(SingleLog const& copy) = delete;
        SingleLog& operator=(SingleLog const& copy) = delete;

    public:
        /**
         * Return a reference to the instance of this class.
         * C++11 handles thread safety and removes the need for manual locking.
         * http://stackoverflow.com/questions/8102125/is-local-static-variable-initialization-thread-safe-in-c11
         */
        static SingleLog& GetInstance()
        {
            static SingleLog instance;
            return instance;
        }

        /**
         * Destructor
         */
        ~SingleLog()
        {
            {
                std::lock_guard<std::mutex> lock(m_consoleLogDequeLock);
                m_consoleExit = true;
            }
            m_consoleCv.notify_all();
            {
                std::lock_guard<std::mutex> lock(m_fstreamLogDequeLock);
                m_fstreamExit = true;
            }
            m_fstreamCv.notify_all();
            if (m_consoleWriter.joinable())
            {
                m_consoleWriter.join();
            }
            if (m_fstreamWriter.joinable())
            {
                m_fstreamWriter.join();
            }
            std::lock_guard<std::mutex> lock(m_fstreamLock);
            if (m_fileOut.is_open())
            {
                m_fileOut << "\n\n";
                m_fileOut.close();
            }
        }

        /**
         * Set the minimum log level for the console
         * L_TRACE, L_DEBUG, L_INFO, L_NOTICE, L_WARNING, L_ERROR, L_CRITICAL, L_OFF
         */
        void SetConsoleLogLevel(const LogLevel& logLevel)
        {
            m_consoleLogLevel.store(logLevel);
        }

        /**
         * Set the minimum log level for the log file
         * L_TRACE, L_DEBUG, L_INFO, L_NOTICE, L_WARNING, L_ERROR, L_CRITICAL, L_OFF
         */
        void SetFileLogLevel(const LogLevel& logLevel)
        {
            m_fileLogLevel.store(logLevel);
        }

        /**
         * Set the path to the log file. Existing content is kept and new
         * entries are appended, so a restart never erases earlier evidence.
         */
        void SetLogFilePath(const std::string& filePath)
        {
            std::lock_guard<std::mutex> lock(m_fstreamLock);
            m_filePath = filePath;
            OpenFileLocked(false);
        }

        /**
         * Enable size-based rotation of the log file. When a write would take
         * the file above maxBytes, the file becomes "<path>.1", older backups
         * shift up to "<path>.<maxBackups>" and the oldest is deleted. maxBytes
         * of 0 disables rotation. maxBackups of 0 discards the old content
         * instead of keeping a backup.
         */
        void SetLogRotation(std::uintmax_t maxBytes, unsigned maxBackups)
        {
            std::lock_guard<std::mutex> lock(m_fstreamLock);
            m_maxFileBytes = maxBytes;
            m_maxBackups = maxBackups;
        }

        /**
         * Set the path to the log file (wide string overload)
         */
        void SetLogFilePath(const std::wstring& filePath)
        {
            SetLogFilePath(ToNarrow(filePath));
        }

        /**
         * Log the line to console and/or file.
         * L_NOTICE and higher severity messages are flushed immediately.
         */
        void LogIt(LogLevel level, const std::string& line)
        {
            const bool flush = (level >= LogLevel::L_NOTICE);
            if (m_consoleLogLevel.load() <= level)
            {
                ConsoleLog(line, flush);
            }
            if (m_fileLogLevel.load() <= level)
            {
                FileLog(line, flush);
            }
        }

        /**
         * Log TRACE level messages
         */
        void Trace(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_TRACE, MakeLogLine("TRACE", _module, _message));
        }

        /**
         * Log TRACE level messages
         */
        void Trace(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_TRACE, MakeLogLine(L"TRACE", _module, _message));
        }

        /**
         * Log DEBUG level messages
         */
        void Debug(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_DEBUG, MakeLogLine("DEBUG", _module, _message));
        }

        /**
         * Log DEBUG level messages
         */
        void Debug(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_DEBUG, MakeLogLine(L"DEBUG", _module, _message));
        }

        /**
         * Log INFO level messages
         */
        void Info(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_INFO, MakeLogLine("INFO", _module, _message));
        }

        /**
         * Log INFO level messages
         */
        void Info(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_INFO, MakeLogLine(L"INFO", _module, _message));
        }

        /**
         * Log NOTICE level messages
         */
        void Notice(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_NOTICE, MakeLogLine("NOTICE", _module, _message));
        }

        /**
         * Log NOTICE level messages
         */
        void Notice(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_NOTICE, MakeLogLine(L"NOTICE", _module, _message));
        }

        /**
         * Log WARNING level messages
         */
        void Warning(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_WARNING, MakeLogLine("WARNING", _module, _message));
        }

        /**
         * Log WARNING level messages
         */
        void Warning(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_WARNING, MakeLogLine(L"WARNING", _module, _message));
        }

        /**
         * Log ERROR level messages
         */
        void Error(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_ERROR, MakeLogLine("ERROR", _module, _message));
        }

        /**
         * Log ERROR level messages
         */
        void Error(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_ERROR, MakeLogLine(L"ERROR", _module, _message));
        }

        /**
         * Log CRITICAL level messages
         */
        void Critical(const std::string& _module, const std::string& _message)
        {
            LogIt(LogLevel::L_CRITICAL, MakeLogLine("CRITICAL", _module, _message));
        }

        /**
         * Log CRITICAL level messages
         */
        void Critical(const std::wstring& _module, const std::wstring& _message)
        {
            LogIt(LogLevel::L_CRITICAL, MakeLogLine(L"CRITICAL", _module, _message));
        }

    private:
        // Encodes a UTF-16 (wchar_t==2) or UTF-32 (wchar_t==4) wstring to a UTF-8 string.
        // Replaces deprecated std::wstring_convert / std::codecvt_utf8_utf16.
        static std::string ToNarrow(const std::wstring& w)
        {
            std::string result;
            result.reserve(w.size() * 3);
            for (size_t i = 0; i < w.size(); ++i)
            {
                uint32_t cp = static_cast<uint32_t>(w[i]);
                if constexpr (sizeof(wchar_t) == 2)
                {
                    // Decode UTF-16 surrogate pair
                    if (cp >= 0xD800U && cp <= 0xDBFFU && (i + 1) < w.size())
                    {
                        const uint32_t lo = static_cast<uint32_t>(w[i + 1]);
                        if (lo >= 0xDC00U && lo <= 0xDFFFU)
                        {
                            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
                            ++i;
                        }
                    }
                }
                if (cp < 0x80U)
                {
                    result += static_cast<char>(cp);
                }
                else if (cp < 0x800U)
                {
                    result += static_cast<char>(0xC0U | (cp >> 6U));
                    result += static_cast<char>(0x80U | (cp & 0x3FU));
                }
                else if (cp < 0x10000U)
                {
                    result += static_cast<char>(0xE0U | (cp >> 12U));
                    result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
                    result += static_cast<char>(0x80U | (cp & 0x3FU));
                }
                else
                {
                    result += static_cast<char>(0xF0U | (cp >> 18U));
                    result += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
                    result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
                    result += static_cast<char>(0x80U | (cp & 0x3FU));
                }
            }
            return result;
        }

        // Decodes a UTF-8 string to a UTF-16 (wchar_t==2) or UTF-32 (wchar_t==4) wstring.
        // Replaces deprecated std::wstring_convert / std::codecvt_utf8_utf16.
        static std::wstring ToWide(const std::string& s)
        {
            std::wstring result;
            result.reserve(s.size());
            size_t i = 0;
            while (i < s.size())
            {
                const auto c = static_cast<uint8_t>(s[i]);
                uint32_t cp = 0;
                size_t extra = 0;
                if (c < 0x80U)
                {
                    cp = c;
                    extra = 0;
                }
                else if (c < 0xE0U)
                {
                    cp = c & 0x1FU;
                    extra = 1;
                }
                else if (c < 0xF0U)
                {
                    cp = c & 0x0FU;
                    extra = 2;
                }
                else
                {
                    cp = c & 0x07U;
                    extra = 3;
                }
                for (size_t k = 1; k <= extra && (i + k) < s.size(); ++k)
                {
                    cp = (cp << 6U) | (static_cast<uint8_t>(s[i + k]) & 0x3FU);
                }
                i += extra + 1;
                if constexpr (sizeof(wchar_t) == 2)
                {
                    if (cp < 0x10000U)
                    {
                        result += static_cast<wchar_t>(cp);
                    }
                    else
                    {
                        const uint32_t adjusted = cp - 0x10000U;
                        result += static_cast<wchar_t>(0xD800U + (adjusted >> 10U));
                        result += static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU));
                    }
                }
                else
                {
                    result += static_cast<wchar_t>(cp);
                }
            }
            return result;
        }

        // Returns current local date/time as "YYYY-MM-DD HH:mm:ss.mmm +ZZZZ"
        static std::string CurrentDateTime()
        {
            auto now = std::chrono::system_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            const std::time_t ttnow = std::chrono::system_clock::to_time_t(now);
            tm buf{};
#ifdef _WIN32
            localtime_s(&buf, &ttnow);
#else
            localtime_r(&ttnow, &buf);
#endif
            char datebuf[32];
            char zonebuf[10];
            char result[48];
            if (std::strftime(datebuf, sizeof(datebuf), "%F %T", &buf) &&
                std::strftime(zonebuf, sizeof(zonebuf), "%z", &buf))
            {
                std::snprintf(result, sizeof(result), "%s.%03d %s", datebuf, static_cast<int>(ms.count()), zonebuf);
                return result;
            }
            return {};
        }

        std::string MakeLogLine(const std::string& _level, const std::string& _module, const std::string& _message)
        {
            std::ostringstream ss;
            ss << CurrentDateTime() << "  <" << _level << ">  " << _module << ":  " << _message << "\n";
            return ss.str();
        }

        std::string MakeLogLine(const std::wstring& _level, const std::wstring& _module, const std::wstring& _message)
        {
            std::wostringstream ss;
            ss << ToWide(CurrentDateTime()) << L"  <" << _level << L">  " << _module << L":  " << _message << L"\n";
            return ToNarrow(ss.str());
        }

        // Bytes the message occupies in the file. A text-mode stream on Windows
        // writes each LF as CR LF.
        static std::uintmax_t OnDiskSize(const std::string& message)
        {
#ifdef _WIN32
            return message.size() + static_cast<std::uintmax_t>(std::count(message.begin(), message.end(), '\n'));
#else
            return message.size();
#endif
        }

        // Opens the log file for appending (or truncating after a rotation) and
        // records its current size. Caller holds m_fstreamLock.
        void OpenFileLocked(bool truncate)
        {
            if (m_fileOut.is_open())
            {
                m_fileOut.close();
            }
            m_fileOut.clear();
            m_fileOut.open(m_filePath, truncate ? std::ios_base::out | std::ios_base::trunc
                                                : std::ios_base::out | std::ios_base::app);
            m_fileBytes = 0;
            if (m_fileOut.is_open())
            {
                m_fileOut.rdbuf()->pubsetbuf(m_writeBuffer.data(), LoggerInternalBufferSize);
                std::error_code ec;
                const auto size = std::filesystem::file_size(m_filePath, ec);
                if (!ec)
                {
                    m_fileBytes = size;
                }
            }
        }

        // Shifts "<path>.N-1" to "<path>.N" and so on, moves the live file to
        // "<path>.1" and starts a fresh file. Caller holds m_fstreamLock.
        void RotateLocked()
        {
            m_fileOut.close();
            std::error_code ec;
            if (m_maxBackups == 0)
            {
                std::filesystem::remove(m_filePath, ec);
            }
            else
            {
                const auto backup = [this](unsigned index) {
                    return m_filePath + "." + std::to_string(index);
                };
                std::filesystem::remove(backup(m_maxBackups), ec);
                for (unsigned i = m_maxBackups - 1; i >= 1; --i)
                {
                    if (std::filesystem::exists(backup(i), ec))
                    {
                        std::filesystem::rename(backup(i), backup(i + 1), ec);
                    }
                }
                std::filesystem::rename(m_filePath, backup(1), ec);
            }
            OpenFileLocked(true);
        }

        void ConsoleLog(const std::string& s, bool flush)
        {
            {
                std::lock_guard<std::mutex> lock(m_consoleLogDequeLock);
                if (m_consoleLogDeque.size() < k_maxLogQueueSize)
                {
                    m_consoleLogDeque.push_back({s, flush});
                }
            }
            m_consoleCv.notify_one();
        }

        void FileLog(const std::string& s, bool flush)
        {
            {
                std::lock_guard<std::mutex> lock(m_fstreamLogDequeLock);
                if (m_fstreamLogDeque.size() < k_maxLogQueueSize)
                {
                    m_fstreamLogDeque.push_back({s, flush});
                }
            }
            m_fstreamCv.notify_one();
        }

        void ConsoleWriter()
        {
            while (true)
            {
                LogEntry entry;
                {
                    std::unique_lock<std::mutex> lock(m_consoleLogDequeLock);
                    m_consoleCv.wait(lock, [this]() {
                        return m_consoleExit || !m_consoleLogDeque.empty();
                    });
                    if (m_consoleExit && m_consoleLogDeque.empty())
                    {
                        break;
                    }
                    entry = std::move(m_consoleLogDeque.front());
                    m_consoleLogDeque.pop_front();
                }
                std::cout << entry.message;
                if (entry.flush)
                {
                    std::cout.flush();
                }
            }
        }

        void FstreamWriter()
        {
            std::size_t low_severity_since_flush{0};
            while (true)
            {
                LogEntry entry;
                {
                    std::unique_lock<std::mutex> lock(m_fstreamLogDequeLock);
                    m_fstreamCv.wait(lock, [this]() {
                        return m_fstreamExit || !m_fstreamLogDeque.empty();
                    });
                    if (m_fstreamExit && m_fstreamLogDeque.empty())
                    {
                        break;
                    }
                    entry = std::move(m_fstreamLogDeque.front());
                    m_fstreamLogDeque.pop_front();
                }
                std::lock_guard<std::mutex> lock(m_fstreamLock);
                if (m_fileOut.is_open())
                {
                    if (m_maxFileBytes > 0 && m_fileBytes > 0 &&
                        m_fileBytes + OnDiskSize(entry.message) > m_maxFileBytes)
                    {
                        RotateLocked();
                    }
                    if (!m_fileOut.is_open())
                    {
                        continue;
                    }
                    m_fileOut << entry.message;
                    m_fileBytes += OnDiskSize(entry.message);
                    if (entry.flush)
                    {
                        m_fileOut.flush();
                        low_severity_since_flush = 0;
                    }
                    else if (++low_severity_since_flush >= LowSeverityFlushInterval)
                    {
                        m_fileOut.flush();
                        low_severity_since_flush = 0;
                    }
                }
            }
        }

        std::atomic<LogLevel> m_consoleLogLevel{LogLevel::L_INFO};
        std::atomic<LogLevel> m_fileLogLevel{LogLevel::L_TRACE};
        std::ofstream m_fileOut{};
        std::string m_filePath{};
        std::uintmax_t m_fileBytes{0};
        std::uintmax_t m_maxFileBytes{0};
        unsigned m_maxBackups{0};
        std::array<char, LoggerInternalBufferSize> m_writeBuffer{};

        std::mutex m_consoleLogDequeLock{};
        std::mutex m_fstreamLogDequeLock{};
        std::mutex m_fstreamLock{};
        std::condition_variable m_consoleCv{};
        std::condition_variable m_fstreamCv{};

        std::deque<LogEntry> m_consoleLogDeque{};
        std::deque<LogEntry> m_fstreamLogDeque{};

        bool m_consoleExit{false};
        bool m_fstreamExit{false};

        std::thread m_consoleWriter{};
        std::thread m_fstreamWriter{};
    };

}; // namespace logging

namespace
{
    auto& g_globalSingleLogInstanceRef{thewatcher::logging::SingleLog::GetInstance()};

    class FunctionTrace final
    {
    public:
        explicit FunctionTrace(const std::string& functionName)
            : m_functionName{functionName}
        {
            std::ostringstream ss;
            ss << ">>> Entering: " << m_functionName;
            g_globalSingleLogInstanceRef.Trace("FunctionTrace", ss.str());
        }

        ~FunctionTrace()
        {
            std::ostringstream ss;
            ss << "<<< Exiting: " << m_functionName;
            g_globalSingleLogInstanceRef.Trace("FunctionTrace", ss.str());
        }

    private:
        std::string m_functionName;
    };
} // namespace

namespace StringTools
{
    template <typename... Args>
    std::string string_format(const std::string& format, Args&&... args)
    {
        // Check raw return value before adding 1; snprintf returns -1 on encoding error
        const int n = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
        if (n < 0)
        {
            return {};
        }
        const auto size = static_cast<size_t>(n) + 1;
        auto buffer = std::make_unique<char[]>(size);
        std::snprintf(buffer.get(), size, format.c_str(), std::forward<Args>(args)...);
        return std::string(buffer.get(), static_cast<size_t>(n));
    }
} // namespace StringTools

#define LOG_FUNCTION_TRACE thewatcher::FunctionTrace tr(__func__);

#define LOG_TRACE(message) thewatcher::g_globalSingleLogInstanceRef.Trace(__func__, message);

#define LOG_DEBUG(message) thewatcher::g_globalSingleLogInstanceRef.Debug(__func__, message);

#define LOG_INFO(message) thewatcher::g_globalSingleLogInstanceRef.Info(__func__, message);

#define LOG_NOTICE(message) thewatcher::g_globalSingleLogInstanceRef.Notice(__func__, message);

#define LOG_WARNING(message) thewatcher::g_globalSingleLogInstanceRef.Warning(__func__, message);

#define LOG_ERROR(message) thewatcher::g_globalSingleLogInstanceRef.Error(__func__, message);

#define LOG_CRITICAL(message) thewatcher::g_globalSingleLogInstanceRef.Critical(__func__, message);

#define LOGF_TRACE(format, ...)                                                                                        \
    thewatcher::g_globalSingleLogInstanceRef.Trace(__func__,                                                           \
                                                   thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_DEBUG(format, ...)                                                                                        \
    thewatcher::g_globalSingleLogInstanceRef.Debug(__func__,                                                           \
                                                   thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_INFO(format, ...)                                                                                         \
    thewatcher::g_globalSingleLogInstanceRef.Info(__func__,                                                            \
                                                  thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_NOTICE(format, ...)                                                                                       \
    thewatcher::g_globalSingleLogInstanceRef.Notice(__func__,                                                          \
                                                    thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_WARNING(format, ...)                                                                                      \
    thewatcher::g_globalSingleLogInstanceRef.Warning(__func__,                                                         \
                                                     thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_ERROR(format, ...)                                                                                        \
    thewatcher::g_globalSingleLogInstanceRef.Error(__func__,                                                           \
                                                   thewatcher::StringTools::string_format(format, __VA_ARGS__));

#define LOGF_CRITICAL(format, ...)                                                                                     \
    thewatcher::g_globalSingleLogInstanceRef.Critical(__func__,                                                        \
                                                      thewatcher::StringTools::string_format(format, __VA_ARGS__));

}; // namespace thewatcher
