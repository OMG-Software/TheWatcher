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
// - Ancestors: every existing directory above `dir`, up to the filesystem root
//   (on the resolved path), is checked too, because whoever can rename or
//   replace one can swap `dir` between the check and the open. POSIX (like
//   OpenSSH StrictModes): owned by root or the effective user, and not group- or
//   other-writable unless the sticky bit is set (as on /tmp). Windows: owned by
//   SYSTEM, Administrators, TrustedInstaller or the current user, and no other
//   account holds DELETE, FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER or
//   GENERIC_ALL on it; entries that only apply to children are ignored, and the
//   FILE_ADD_FILE / FILE_ADD_SUBDIRECTORY that Users hold on C:\ProgramData are
//   fine.
// - Windows ACL entries are classified fail-closed: allow entries are checked,
//   deny entries are skipped (they only remove access), and any other entry type
//   (such as an object-specific allow entry) makes the directory unsafe.
// Throws std::runtime_error naming the directory (and the ancestor, when that is
// the problem), the reason and how to fix it (fix the permissions, run the
// installer, or choose a private --config path).
void ensure_private_directory(const std::filesystem::path& dir);

namespace detail
{
    // Creates one private directory level (POSIX mode 0700; Windows protected ACL).
    // Never follows a link: a symlink or junction already at `dir` is refused, and
    // an existing real directory is left as it is. Exposed for tests.
    void create_private_directory_level(const std::filesystem::path& dir);
} // namespace detail

} // namespace thewatcher::common
