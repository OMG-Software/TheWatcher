// Windows ACL classification behind ensure_private_directory. Exposed so tests
// can feed it an ACL built in memory instead of depending on what the file
// system accepts.
#pragma once

#ifdef _WIN32

#include <optional>
#include <string>

#include <windows.h>

namespace thewatcher::common::detail
{

// Which directory an ACL belongs to.
enum class AclRole
{
    // The directory that holds the secret. Every entry counts, inherit-only ones
    // too (they apply to the files created in it), and any write-type right
    // granted to another account is refused.
    Leaf,
    // A directory above it. Only entries that apply to the directory itself
    // count (inherit-only ones are skipped), and only the rights that let an
    // account rename, replace or re-permission it or a child are refused:
    // DELETE, FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER and GENERIC_ALL.
    // FILE_ADD_FILE / FILE_ADD_SUBDIRECTORY are fine: C:\ and C:\ProgramData
    // grant them to Users, and they cannot replace an existing directory.
    Ancestor,
};

// Returns why `dacl` (owned by `owner`) is not safe for `role`, or nothing when
// it is. Fails closed:
// - a null DACL (everybody has full access) is refused;
// - allow and callback-allow entries are checked against the role's rights;
// - deny entries (plain, callback, object, callback-object) are skipped, since
//   they can only remove access;
// - any other entry type, such as an object-specific allow entry, is refused
//   because it cannot be checked.
// Trusted trustees are SYSTEM, Administrators, TrustedInstaller (Ancestor only),
// CREATOR OWNER, OWNER RIGHTS, the current user and `owner`.
std::optional<std::string> find_unsafe_dacl_entry(PACL dacl, PSID owner, AclRole role);

} // namespace thewatcher::common::detail

#endif
