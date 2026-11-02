#include "log_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <system_error>

#include <sys/stat.h>

#ifdef _WIN32
#include <cwctype>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace thewatcher::agent
{

namespace
{

    // SEC-10 hard bounds. The server supplies path + pattern; without these
    // caps a hostile or careless configuration could make the agent read
    // arbitrary amounts of data (or burn CPU in catastrophic backtracking)
    // inside its collection thread.
    constexpr std::size_t max_pattern_len = 256;
    constexpr std::size_t max_line_len = 64 * 1024;
    constexpr std::size_t max_matches_per_tick = 100;
    constexpr int64_t max_bytes_per_tick = 1024 * 1024;

    constexpr std::size_t read_chunk_size = 64 * 1024;

    // Path components compare case-insensitively on Windows (NTFS), where
    // GetFinalPathNameByHandleW may report a different case than the allowlist.
    bool same_component(const std::filesystem::path& a, const std::filesystem::path& b)
    {
#ifdef _WIN32
        const auto& x = a.native();
        const auto& y = b.native();
        return x.size() == y.size() &&
               std::equal(x.begin(), x.end(), y.begin(), [](wchar_t l, wchar_t r) { return std::towlower(l) == std::towlower(r); });
#else
        return a == b;
#endif
    }

    // True when path (already resolved) lies under dir, comparing whole
    // path components so "/allowed2" does not count as under "/allowed".
    // An empty dir never matches (it would otherwise match everything).
    bool is_under(const std::filesystem::path& path, const std::filesystem::path& dir)
    {
        if (dir.empty())
            return false;
        auto p = path.begin();
        for (auto d = dir.begin(); d != dir.end(); ++d, ++p)
        {
            if (p == path.end() || !same_component(*p, *d))
                return false;
        }
        return true;
    }

    bool is_allowed(const std::filesystem::path& path, const std::vector<std::filesystem::path>& dirs)
    {
        return std::any_of(dirs.begin(), dirs.end(), [&](const auto& dir) { return is_under(path, dir); });
    }

    // A file opened exactly once. Every later check (type, identity, size,
    // location) and read goes through this one descriptor/handle, so the
    // pathname cannot be swapped for a symlink or FIFO between the checks and
    // the reads (SEC-10 TOCTOU).
    class ReadOnlyFile
    {
    public:
        enum class Status
        {
            ok,
            missing,
            not_regular,
            failed
        };

        ReadOnlyFile() = default;
        ReadOnlyFile(const ReadOnlyFile&) = delete;
        ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
        ~ReadOnlyFile()
        {
            close();
        }

        // Opens without following a final symlink and without blocking on
        // FIFOs, then requires a regular file. `error` is set for Status::failed.
        Status open(const std::filesystem::path& path, std::string& error);

        int64_t id() const noexcept
        {
            return id_;
        }
        int64_t size() const noexcept
        {
            return size_;
        }
        // Where the OS says the opened object really lives; empty when the
        // platform cannot tell (the caller then relies on its pre-open check).
        const std::filesystem::path& real_path() const noexcept
        {
            return real_path_;
        }

        // Reads up to `length` bytes at `offset`; returns bytes read, 0 at EOF, -1 on error.
        int64_t read_at(int64_t offset, char* buffer, std::size_t length) const;

    private:
        // Closes the descriptor or handle if open. Safe to call more than once.
        void close() noexcept;

#ifdef _WIN32
        HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
        int fd_ = -1;
#endif
        int64_t id_ = -1;
        int64_t size_ = 0;
        std::filesystem::path real_path_;
    };

#ifdef _WIN32

    void ReadOnlyFile::close() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    ReadOnlyFile::Status ReadOnlyFile::open(const std::filesystem::path& path, std::string& error)
    {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                return Status::missing;
            error = std::system_category().message(static_cast<int>(err));
            return Status::failed;
        }

        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_, &info))
        {
            error = std::system_category().message(static_cast<int>(GetLastError()));
            close();
            return Status::failed;
        }
        // Reparse points (symlinks, junctions) are opened as themselves, not
        // followed; refuse them along with directories and non-disk objects.
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            GetFileType(handle_) != FILE_TYPE_DISK)
        {
            close();
            return Status::not_regular;
        }

        // Fail closed: if the final location cannot be determined, do not read.
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD length = GetFinalPathNameByHandleW(handle_, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length >= buffer.size())
        {
            buffer.assign(length + 1, L'\0');
            length = GetFinalPathNameByHandleW(handle_, buffer.data(), static_cast<DWORD>(buffer.size()),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        if (length == 0 || length >= buffer.size())
        {
            error = "GetFinalPathNameByHandleW failed";
            close();
            return Status::failed;
        }
        buffer.resize(length);
        // Strip the \\?\ (or \\?\UNC\) namespace prefix so the result compares with ordinary paths.
        if (buffer.rfind(L"\\\\?\\UNC\\", 0) == 0)
            buffer = L"\\\\" + buffer.substr(8);
        else if (buffer.rfind(L"\\\\?\\", 0) == 0)
            buffer = buffer.substr(4);
        real_path_ = buffer;

        id_ = static_cast<int64_t>((static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
        size_ = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
        return Status::ok;
    }

    int64_t ReadOnlyFile::read_at(int64_t offset, char* buffer, std::size_t length) const
    {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
        position.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
        DWORD read = 0;
        if (!ReadFile(handle_, buffer, static_cast<DWORD>(length), &read, &position))
            return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
        return static_cast<int64_t>(read);
    }

#else

    void ReadOnlyFile::close() noexcept
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // The path the kernel says an open descriptor refers to, or empty when
    // unavailable (no /proc, unsupported platform).
    std::filesystem::path descriptor_path(int fd)
    {
#if defined(__linux__)
        std::error_code ec;
        auto target = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd), ec);
        return ec ? std::filesystem::path{} : target;
#elif defined(__APPLE__)
        std::array<char, PATH_MAX> buffer{};
        if (::fcntl(fd, F_GETPATH, buffer.data()) == 0)
            return std::filesystem::path(buffer.data());
        return {};
#else
        (void)fd;
        return {};
#endif
    }

    ReadOnlyFile::Status ReadOnlyFile::open(const std::filesystem::path& path, std::string& error)
    {
        // O_NOFOLLOW: never traverse a symlink swapped in at the last
        // component. O_NONBLOCK: opening a FIFO must not block the
        // collection thread (it is rejected below by fstat).
        fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd_ < 0)
        {
            const int err = errno;
            if (err == ENOENT || err == ENOTDIR)
                return Status::missing;
            // ELOOP (Linux/macOS) or EMLINK (FreeBSD): the last component is a symlink.
            if (err == ELOOP || err == EMLINK)
                return Status::not_regular;
            error = std::system_category().message(err);
            return Status::failed;
        }

        struct stat st{};
        if (::fstat(fd_, &st) != 0)
        {
            error = std::system_category().message(errno);
            close();
            return Status::failed;
        }
        if (!S_ISREG(st.st_mode))
        {
            close();
            return Status::not_regular;
        }

        id_ = static_cast<int64_t>(st.st_ino);
        size_ = static_cast<int64_t>(st.st_size);
        real_path_ = descriptor_path(fd_);
        return Status::ok;
    }

    int64_t ReadOnlyFile::read_at(int64_t offset, char* buffer, std::size_t length) const
    {
        for (;;)
        {
            const auto n = ::pread(fd_, buffer, length, static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR)
                continue;
            return static_cast<int64_t>(n);
        }
    }

#endif

} // namespace

// Parses LOG_ALLOWED_DIRS (platform path-list separator: ';' on Windows,
// ':' elsewhere) into the allowlist handed to LogCollector.
std::vector<std::filesystem::path> parse_log_allowed_dirs_from_env()
{
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::vector<std::filesystem::path> dirs;

    std::string value;
#ifdef _WIN32
    {
        // MSVC deprecates getenv (C4996, fatal under werror); _dupenv_s is
        // the documented replacement. size includes the NUL terminator.
        char* buffer = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&buffer, &size, "LOG_ALLOWED_DIRS") == 0 && buffer != nullptr)
        {
            value.assign(buffer, size > 0 ? size - 1 : 0);
            std::free(buffer);
        }
    }
#else
    if (const char* raw = std::getenv("LOG_ALLOWED_DIRS"); raw != nullptr)
        value = raw;
#endif
    if (value.empty())
        return dirs;

    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find(separator, start);
        const auto entry = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!entry.empty())
        {
            std::error_code ec;
            auto resolved = std::filesystem::weakly_canonical(entry, ec);
            if (!ec)
                dirs.push_back(std::move(resolved));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return dirs;
}

void LogCollector::set_configs(std::vector<LogMonitorConfig> configs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    configs_ = std::move(configs);
    // Remove file states for paths that are no longer configured.
    for (auto it = file_states_.begin(); it != file_states_.end();)
    {
        bool found = false;
        for (const auto& cfg : configs_)
        {
            if (cfg.path == it->first)
            {
                found = true;
                break;
            }
        }
        it = found ? std::next(it) : file_states_.erase(it);
    }
}

void LogCollector::set_allowed_dirs(std::vector<std::filesystem::path> dirs)
{
    // tail_file compares fully resolved file paths against this list, so
    // resolve the entries the same way (macOS /var -> /private/var, Windows
    // 8.3 names, symlinked or "a/../b" directories). Unresolvable or empty
    // entries are dropped — an empty entry would otherwise allow everything.
    std::vector<std::filesystem::path> resolved;
    resolved.reserve(dirs.size());
    for (const auto& dir : dirs)
    {
        std::error_code ec;
        auto canonical = dir.empty() ? std::filesystem::path{} : std::filesystem::weakly_canonical(dir, ec);
        if (ec || canonical.empty())
        {
            LOGF_WARNING("LogCollector ignoring unresolvable allowlist entry path=%s", dir.string().c_str());
            continue;
        }
        resolved.push_back(std::move(canonical));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    allowed_dirs_ = std::move(resolved);
    LOGF_DEBUG("LogCollector allowlist updated entry_count=%zu", allowed_dirs_.size());
}

void LogCollector::tick()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // SEC-10: with no allowlist configured the log monitor is disabled
    // outright — the monitored contents land on the dashboard, so an empty
    // allowlist must not fall back to "read anything".
    if (allowed_dirs_.empty() && !configs_.empty())
    {
        LOG_WARNING("Log monitoring disabled: LOG_ALLOWED_DIRS is not configured");
        return;
    }
    for (const auto& cfg : configs_)
    {
        if (!cfg.enabled || cfg.path.empty() || cfg.pattern.empty() || cfg.indicator_name.empty())
            continue;
        if (cfg.pattern.size() > max_pattern_len)
        {
            LOGF_WARNING("LogCollector pattern too long path=%s pattern_len=%zu", cfg.path.c_str(), cfg.pattern.size());
            continue;
        }
        try
        {
            tail_file(cfg);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("LogCollector tail_file path=%s error=%s", cfg.path.c_str(), e.what());
        }
    }
}

std::vector<proto::LogMatch> LogCollector::take_matches()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(pending_, {});
}

void LogCollector::tail_file(const LogMonitorConfig& cfg)
{
    // SEC-10: resolve symlinks and require the real location to sit inside
    // a locally allowlisted directory. The server cannot extend this list.
    std::error_code resolve_ec;
    const auto resolved = std::filesystem::weakly_canonical(cfg.path, resolve_ec);
    if (resolve_ec)
    {
        file_states_.erase(cfg.path);
        return;
    }
    if (!is_allowed(resolved, allowed_dirs_))
    {
        LOGF_WARNING("LogCollector path outside LOG_ALLOWED_DIRS path=%s", cfg.path.c_str());
        file_states_.erase(cfg.path);
        return;
    }

    // SEC-10: open the file once; the type check, identity, size and every
    // read below use this one descriptor, so swapping the pathname for a
    // symlink or FIFO after the check above gains an attacker nothing.
    ReadOnlyFile file;
    std::string open_error;
    switch (file.open(resolved, open_error))
    {
    case ReadOnlyFile::Status::ok:
        break;
    case ReadOnlyFile::Status::missing:
        // File doesn't exist yet; reset state so we start from byte 0 when it appears.
        file_states_.erase(cfg.path);
        return;
    case ReadOnlyFile::Status::not_regular:
        // Directories, devices, fifos and symlinks are rejected outright.
        LOGF_WARNING("LogCollector path is not a regular file path=%s", cfg.path.c_str());
        file_states_.erase(cfg.path);
        return;
    case ReadOnlyFile::Status::failed:
        LOGF_WARNING("LogCollector open path=%s error=%s", cfg.path.c_str(), open_error.c_str());
        file_states_.erase(cfg.path);
        return;
    }

    // Re-check containment on the object we actually opened. Where the OS
    // cannot report its location (no /proc, FreeBSD) the pre-open check above
    // is all there is.
    if (const auto& opened_path = file.real_path(); !opened_path.empty() && !is_allowed(opened_path, allowed_dirs_))
    {
        LOGF_WARNING("LogCollector opened file outside LOG_ALLOWED_DIRS path=%s", cfg.path.c_str());
        file_states_.erase(cfg.path);
        return;
    }

    auto& state = file_states_[cfg.path];

    // Detect rotation: inode changed or file is shorter than our saved offset.
    if (state.inode != file.id() || file.size() < state.offset)
    {
        LOGF_DEBUG("LogCollector rotation detected path=%s", cfg.path.c_str());
        state.inode = file.id();
        state.offset = 0;
        state.discarding = false;
    }

    if (state.offset >= file.size())
        return; // nothing new

    std::regex pattern;
    try
    {
        pattern = std::regex(cfg.pattern, std::regex::ECMAScript | std::regex::optimize);
    }
    catch (const std::regex_error& e)
    {
        LOGF_WARNING("LogCollector invalid regex pattern=%s error=%s", cfg.pattern.c_str(), e.what());
        return;
    }

    std::size_t matches = 0;
    const auto emit_if_match = [&](const std::string& text)
    {
        if (text.empty() || !std::regex_search(text, pattern))
            return;
        proto::LogMatch m;
        m.indicator_name = cfg.indicator_name;
        m.path = cfg.path;
        m.matched_line = text;
        m.severity = cfg.severity;
        LOGF_DEBUG("LogCollector match indicator=%s path=%s line=%.80s", cfg.indicator_name.c_str(), cfg.path.c_str(),
                   text.c_str());
        pending_.push_back(std::move(m));
        ++matches;
    };

    // SEC-10: bounded reading with hard caps on bytes and matches per tick
    // (std::getline would be unbounded). Semantics:
    //  - Only lines terminated by '\n' are matched. A trailing unterminated
    //    line (a writer mid-write) and a line cut by the per-tick byte cap
    //    are not consumed: the offset goes back to the line start and the
    //    line is read again, whole, on a later tick — so it is matched
    //    exactly once. A file that stays unterminated forever just re-reads
    //    that one (< max_line_len) line each tick and matches nothing.
    //  - A line longer than max_line_len is evaluated once, truncated, as
    //    soon as it overflows; the remainder is dropped up to the next '\n'
    //    (state.discarding carries this across ticks).
    int64_t pos = state.offset;        // next byte to read
    int64_t line_start = pos;          // where the current line began
    bool discarding = state.discarding; // inside the tail of an over-long line
    int64_t bytes_read = 0;
    std::string line;
    std::array<char, read_chunk_size> chunk;
    while (bytes_read < max_bytes_per_tick && matches < max_matches_per_tick)
    {
        const auto want = static_cast<std::size_t>(std::min<int64_t>(chunk.size(), max_bytes_per_tick - bytes_read));
        const int64_t n = file.read_at(pos, chunk.data(), want);
        if (n <= 0)
            break; // EOF or read error

        for (int64_t i = 0; i < n && matches < max_matches_per_tick; ++i)
        {
            const char c = chunk[static_cast<std::size_t>(i)];
            ++pos;
            ++bytes_read;
            if (c == '\n')
            {
                if (!discarding)
                    emit_if_match(line);
                line.clear();
                discarding = false;
                line_start = pos;
            }
            else if (discarding)
            {
                // Dropping the rest of an over-long line.
            }
            else if (line.size() < max_line_len)
            {
                line.push_back(c);
            }
            else
            {
                emit_if_match(line);
                line.clear();
                discarding = true;
            }
        }
        // Bytes of this chunk not consumed (match cap) are simply re-read next tick.
    }

    state.discarding = discarding;
    // Mid-line (not discarding): rewind to the line start so it is processed whole.
    state.offset = (!discarding && pos != line_start) ? line_start : pos;
}

} // namespace thewatcher::agent