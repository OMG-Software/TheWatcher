#include "secure_file.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sodium.h>

#ifdef _WIN32
#include <windows.h>
// windows.h first: sddl.h depends on its declarations.
#include <aclapi.h>
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

namespace
{
    std::string fix_advice(const fs::path& dir)
    {
#ifdef _WIN32
        return "Run the installer, or fix it with: icacls \"" + dir.string() +
               "\" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F, "
               "or pass --config with a path in a private directory.";
#else
        return "Run: chmod 700 '" + dir.string() +
               "' (and chown it to the user that runs the server), or pass --config with a path in a private "
               "directory.";
#endif
    }

    [[noreturn]] void refuse(const fs::path& dir, const std::string& reason)
    {
        throw std::runtime_error("The directory " + dir.string() + " is not private: " + reason + ". " +
                                 fix_advice(dir));
    }

#ifdef _WIN32
    // Frees memory returned by the security APIs (LocalFree).
    struct LocalFreeDeleter
    {
        void operator()(void* p) const
        {
            if (p != nullptr)
                LocalFree(p);
        }
    };
    using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

    struct HandleCloser
    {
        void operator()(void* h) const
        {
            if (h != nullptr)
                CloseHandle(h);
        }
    };

    // Rights that let an account change what is in the directory or who may.
    constexpr DWORD write_like_rights = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_WRITE_DATA | FILE_APPEND_DATA |
                                        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC |
                                        WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

    std::vector<unsigned char> well_known_sid(WELL_KNOWN_SID_TYPE type)
    {
        std::vector<unsigned char> sid(SECURITY_MAX_SID_SIZE);
        DWORD size = static_cast<DWORD>(sid.size());
        if (!CreateWellKnownSid(type, nullptr, sid.data(), &size))
            throw std::runtime_error("Cannot build a well-known SID (error " + std::to_string(GetLastError()) + ")");
        sid.resize(size);
        return sid;
    }

    // SID of the user this process runs as.
    std::vector<unsigned char> current_user_sid()
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            throw std::runtime_error("Cannot open the process token (error " + std::to_string(GetLastError()) + ")");
        std::unique_ptr<void, HandleCloser> token(raw);
        DWORD size = 0;
        GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (size == 0 || !GetTokenInformation(raw, TokenUser, buffer.data(), size, &size))
            throw std::runtime_error("Cannot read the process user (error " + std::to_string(GetLastError()) + ")");
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        const auto length = GetLengthSid(user->User.Sid);
        const auto* begin = reinterpret_cast<const unsigned char*>(user->User.Sid);
        return std::vector<unsigned char>(begin, begin + length);
    }

    // "DOMAIN\name" when the SID resolves, the SID string otherwise.
    std::string describe_sid(PSID sid)
    {
        char name[256] = {};
        char domain[256] = {};
        DWORD name_size = sizeof(name);
        DWORD domain_size = sizeof(domain);
        SID_NAME_USE use{};
        if (LookupAccountSidA(nullptr, sid, name, &name_size, domain, &domain_size, &use))
            return std::string(domain) + "\\" + name;
        LPSTR text = nullptr;
        if (ConvertSidToStringSidA(sid, &text))
        {
            LocalMemory guard(text);
            return text;
        }
        return "an unknown account";
    }

    // Checks an existing directory; throws via refuse() when it is not private.
    void verify_private_directory(const fs::path& dir)
    {
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
        const DWORD status =
            GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &owner, nullptr, &dacl, nullptr, &raw_descriptor);
        if (status != ERROR_SUCCESS)
            refuse(dir, "its permissions cannot be read (error " + std::to_string(status) + ")");
        LocalMemory descriptor(raw_descriptor);

        const auto system_sid = well_known_sid(WinLocalSystemSid);
        const auto admins_sid = well_known_sid(WinBuiltinAdministratorsSid);
        const auto creator_owner_sid = well_known_sid(WinCreatorOwnerSid);
        // OWNER RIGHTS (S-1-3-4): rights of whoever owns the directory, which is checked above.
        const auto owner_rights_sid = well_known_sid(WinCreatorOwnerRightsSid);
        const auto user_sid = current_user_sid();
        const auto same = [](PSID a, const std::vector<unsigned char>& b) {
            return EqualSid(a, const_cast<unsigned char*>(b.data())) != FALSE;
        };

        if (owner == nullptr || !(same(owner, system_sid) || same(owner, admins_sid) || same(owner, user_sid)))
            refuse(dir, "it is owned by " + (owner != nullptr ? describe_sid(owner) : std::string("nobody")) +
                            ", not by SYSTEM, Administrators or the user running the server");

        // A missing DACL means everybody has full access.
        if (dacl == nullptr)
            refuse(dir, "it has no access control list, so every user has full access");

        for (DWORD i = 0; i < dacl->AceCount; ++i)
        {
            void* ace_raw = nullptr;
            if (!GetAce(dacl, i, &ace_raw))
                refuse(dir, "its access control list cannot be read");
            const auto* header = static_cast<const ACE_HEADER*>(ace_raw);
            // Plain and callback allow entries share one layout. Inherit-only entries
            // count too: they apply to the files created in the directory.
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
                continue;
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(ace_raw);
            PSID trustee = const_cast<DWORD*>(&ace->SidStart);
            if ((ace->Mask & write_like_rights) == 0)
                continue;
            if (same(trustee, system_sid) || same(trustee, admins_sid) || same(trustee, creator_owner_sid) ||
                same(trustee, owner_rights_sid) || same(trustee, user_sid) || EqualSid(trustee, owner))
                continue;
            refuse(dir, describe_sid(trustee) + " is allowed to write to it");
        }
    }

    // Creates one directory with a protected DACL: SYSTEM, Administrators and the
    // owner get full control, inherited by files and subfolders; nobody else.
    void create_private_directory_level(const fs::path& dir)
    {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = FALSE;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
                &attributes.lpSecurityDescriptor, nullptr))
            throw std::runtime_error("Cannot build the security descriptor for " + dir.string());
        LocalMemory descriptor(attributes.lpSecurityDescriptor);
        if (!CreateDirectoryW(dir.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create " + dir.string() + " (error " + std::to_string(GetLastError()) +
                                     ")");
    }
#else
    void verify_private_directory(const fs::path& dir)
    {
        struct stat info{};
        if (::stat(dir.c_str(), &info) != 0)
            refuse(dir, std::string("it cannot be inspected: ") + std::strerror(errno));
        if (!S_ISDIR(info.st_mode))
            refuse(dir, "it is not a directory");
        if (info.st_uid != 0 && info.st_uid != ::geteuid())
            refuse(dir, "it is owned by user id " + std::to_string(info.st_uid) +
                            ", not by root or the user running the server");
        if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
            refuse(dir, "it is writable by its group or by other users");
    }

    void create_private_directory_level(const fs::path& dir)
    {
        if (::mkdir(dir.c_str(), S_IRWXU) != 0 && errno != EEXIST)
            throw std::runtime_error("Cannot create " + dir.string() + ": " + std::strerror(errno));
        // mkdir applies the umask; make the mode exact.
        if (::chmod(dir.c_str(), S_IRWXU) != 0)
            throw std::runtime_error("Cannot restrict " + dir.string() + ": " + std::strerror(errno));
    }
#endif
} // namespace

void ensure_private_directory(const fs::path& dir)
{
    // Levels that do not exist yet, deepest first; each is created private.
    // Levels that already exist (such as C:\ProgramData or /etc) are not judged:
    // only the directory that holds the secret is.
    std::vector<fs::path> missing;
    std::error_code ec;
    for (fs::path level = dir; !level.empty() && !fs::exists(level, ec); level = level.parent_path())
    {
        missing.push_back(level);
        if (level == level.parent_path())
            break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
        create_private_directory_level(*it);

    verify_private_directory(dir);
}

} // namespace thewatcher::common
