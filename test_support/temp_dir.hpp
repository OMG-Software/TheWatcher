// Shared scratch-directory helper for the test suites.
//
// Tests must never build temp paths themselves from a predictable name in the
// (typically world-writable) system temp directory: another local user could
// pre-create or swap such a path. ScopedTempDir creates a uniquely named,
// owner-only directory atomically and removes it recursively on destruction.
//
// This is the one place in the test code that queries the system temp directory.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h> // mkdtemp on some BSDs
#endif

namespace thewatcher::test
{

class ScopedTempDir
{
public:
    // `prefix` becomes the leading part of the directory name; the rest is random.
    explicit ScopedTempDir(const std::string& prefix = "thewatcher-test")
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path();
#ifdef _WIN32
        // No mkdtemp: pick a random 64-bit suffix and let create_directory
        // fail on collisions (it reports false if the path already exists).
        std::random_device rd;
        std::mt19937_64 rng((static_cast<unsigned long long>(rd()) << 32) ^ rd());
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            char suffix[17];
            std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(rng()));
            const fs::path candidate = base / (prefix + "-" + suffix);
            if (fs::create_directory(candidate))
            {
                dir_ = candidate;
                break;
            }
        }
        if (dir_.empty())
            throw std::runtime_error("cannot create scratch directory");
#else
        // mkdtemp creates the directory atomically with mode 0700.
        std::string tmpl = (base / (prefix + "-XXXXXX")).string();
        if (::mkdtemp(tmpl.data()) == nullptr)
            throw std::runtime_error("mkdtemp failed");
        dir_ = tmpl;
#endif
        fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace);
        // Canonical so path comparisons hold where the temp dir is itself a
        // symlink (macOS /var -> /private/var).
        std::error_code ec;
        const auto canonical = fs::canonical(dir_, ec);
        if (!ec)
            dir_ = canonical;
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const
    {
        return dir_;
    }

    // Convenience: a path for `name` inside the scratch directory.
    std::filesystem::path operator/(const std::string& name) const
    {
        return dir_ / name;
    }

private:
    std::filesystem::path dir_;
};

// One scratch directory shared by a whole test process, created on first use
// and removed at process exit. For suites whose helpers have no natural owner.
inline const ScopedTempDir& process_temp_dir()
{
    static const ScopedTempDir dir("thewatcher-tests");
    return dir;
}

} // namespace thewatcher::test
