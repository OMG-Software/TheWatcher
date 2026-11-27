#include "secure_file.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <sodium.h>

#ifdef _WIN32
#include <windows.h>
// windows.h first: sddl.h depends on its declarations.
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace thewatcher::common
{

namespace fs = std::filesystem;

namespace
{
#ifdef _WIN32
    // Owner-only from creation, independent of the parent directory ACL:
    // protected DACL (no inheritance) granting full access to SYSTEM,
    // Administrators and the file's owner (OWNER RIGHTS). MoveFileExW keeps
    // this DACL when it replaces the target.
    fs::path write_temp_file(const fs::path& target, const std::string& content)
    {
        const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
        const std::string base = target.filename().string() + ".tmp.";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)",
                                                                  SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            throw std::runtime_error("Cannot build security descriptor for " + target.string());

        HANDLE h = INVALID_HANDLE_VALUE;
        DWORD create_error = ERROR_SUCCESS;
        fs::path tmp;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            std::uint64_t r = 0;
            randombytes_buf(&r, sizeof(r));
            char suffix[17];
            std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(r));
            tmp = dir / (base + suffix);
            // CREATE_NEW fails if the name exists, so a temp file cannot be pre-planted.
            h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            create_error = (h == INVALID_HANDLE_VALUE) ? GetLastError() : ERROR_SUCCESS;
            if (h != INVALID_HANDLE_VALUE || create_error != ERROR_FILE_EXISTS)
                break;
        }
        LocalFree(sa.lpSecurityDescriptor);
        if (h == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create temporary file beside " + target.string() + " (error " +
                                     std::to_string(create_error) + ")");

        bool ok = true;
        const char* p = content.data();
        std::size_t left = content.size();
        while (left > 0)
        {
            const DWORD chunk = left > (1u << 30) ? (1u << 30) : static_cast<DWORD>(left);
            DWORD written = 0;
            if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0)
            {
                ok = false;
                break;
            }
            p += written;
            left -= written;
        }
        if (ok && !FlushFileBuffers(h))
            ok = false;
        if (!CloseHandle(h))
            ok = false;
        if (!ok)
        {
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Failed writing " + tmp.string());
        }
        return tmp;
    }

    void replace_file(const fs::path& tmp, const fs::path& target)
    {
        // Atomic replace without deleting the old file first.
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot replace " + target.string() + " (error " + std::to_string(error) + ")");
        }
    }
#else
    // mkstemp creates the file with mode 0600; the data is then written and fsync-ed.
    fs::path write_temp_file(const fs::path& target, const std::string& content)
    {
        const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
        std::string tmpl = (dir / (target.filename().string() + ".tmp.XXXXXX")).string();

        const int fd = ::mkstemp(tmpl.data());
        if (fd < 0)
            throw std::runtime_error("Cannot create temporary file beside " + target.string() + ": " +
                                     std::strerror(errno));

        int err = 0;
        // Explicit 0600 in case a libc creates mkstemp files with looser modes.
        if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
            err = errno;
        if (err == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
            err = errno;

        const char* p = content.data();
        std::size_t left = content.size();
        while (err == 0 && left > 0)
        {
            const ssize_t n = ::write(fd, p, left);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                err = errno;
                break;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        if (err == 0 && ::fsync(fd) != 0)
            err = errno;
        if (::close(fd) != 0 && err == 0)
            err = errno;

        if (err != 0)
        {
            ::unlink(tmpl.c_str());
            throw std::runtime_error("Failed writing " + tmpl + ": " + std::strerror(err));
        }
        return fs::path(tmpl);
    }

    void replace_file(const fs::path& tmp, const fs::path& target)
    {
        std::error_code ec;
        fs::rename(tmp, target, ec);
        if (ec)
        {
            std::error_code cleanup_ec;
            fs::remove(tmp, cleanup_ec);
            throw std::runtime_error("Cannot replace " + target.string() + ": " + ec.message());
        }
    }
#endif
} // namespace

void write_private_file_atomic(const fs::path& target, const std::string& content)
{
    replace_file(write_temp_file(target, content), target);
}

} // namespace thewatcher::common
