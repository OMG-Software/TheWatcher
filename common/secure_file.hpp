#pragma once

#include <filesystem>
#include <string>

namespace thewatcher::common
{

// Atomically replaces `target` with `content`. The data is written to an
// owner-only temporary file in the same directory (0600 on POSIX; a protected
// DACL for SYSTEM, Administrators and the owner on Windows), flushed to stable
// storage and then renamed over `target`, so a crash or full disk can never
// leave a truncated file and the secret is never readable by other users.
// Throws std::runtime_error on any failure; the temporary file is removed and
// the previous `target` is left in place.
void write_private_file_atomic(const std::filesystem::path& target, const std::string& content);

// Makes sure `dir`, which will hold a secret, cannot be changed by other users,
// so nobody can pre-create it, plant a file in it or swap one the process then
// trusts.
// - Missing (every missing level): created private. Windows: a protected,
//   inheritable DACL for SYSTEM, Administrators and the owner. POSIX: mode 0700.
// - Existing: only verified, never rewritten. Unsafe is, on Windows, an owner
//   other than SYSTEM, Administrators or the current user, or an allow ACE that
//   grants write, delete, WRITE_DAC or WRITE_OWNER rights to any other account
//   (Users, Authenticated Users, Everyone, ...); on POSIX, an owner other than
//   root or the effective user, or group/other write permission.
// Throws std::runtime_error naming the directory, the reason and how to fix it
// (fix the permissions, run the installer, or choose a private --config path).
void ensure_private_directory(const std::filesystem::path& dir);

} // namespace thewatcher::common
