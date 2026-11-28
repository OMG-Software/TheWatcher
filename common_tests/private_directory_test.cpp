// ensure_private_directory: the directory that holds a secret (the server
// config with the CURVE secret key) must not be writable by other users, or a
// local user could plant or swap the file the server then trusts. Every
// existing ancestor must also be unable to rename or replace it.
#include "common/secure_file.hpp"
#include "test_support/temp_dir.hpp"

#ifdef _WIN32
#include "common/secure_file_acl.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using thewatcher::common::ensure_private_directory;
namespace fs = std::filesystem;

namespace
{
// Where the scratch tree lives. Every existing ancestor of a checked directory
// is checked too, so it must sit under directories with clean permissions, not
// under the build tree or a per-user temp directory whose ACLs the machine
// controls. Windows: %ProgramData% (Users can create folders there, and it passes
// the ancestor check on a stock install). POSIX: the system temp directory
// (sticky, root-owned).
fs::path scratch_base()
{
#ifdef _WIN32
    char* program_data = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&program_data, &length, "PROGRAMDATA") == 0);
    REQUIRE(program_data != nullptr);
    const fs::path base = program_data;
    std::free(program_data);
    return base;
#else
    return fs::temp_directory_path();
#endif
}

fs::path fresh_parent(const char* name)
{
    static const thewatcher::test::ScopedTempDir scratch("thewatcher-private-dir", scratch_base());
    auto dir = scratch.path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// The text of the error ensure_private_directory throws for `dir`; fails the
// test when it accepts the directory.
std::string refusal_message(const fs::path& dir)
{
    try
    {
        ensure_private_directory(dir);
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    FAIL("the unsafe directory was accepted: " << dir.string());
    return {};
}

#ifdef _WIN32
// Runs `icacls <arguments>`; the outer quotes are stripped by cmd.exe /c.
int run_icacls(const std::string& arguments)
{
    return std::system(("\"icacls " + arguments + " > NUL\"").c_str());
}

// Grants the Users group (S-1-5-32-545) write access, inherited by files and subfolders.
void grant_users_write(const fs::path& dir)
{
    REQUIRE(run_icacls("\"" + dir.string() + "\" /grant *S-1-5-32-545:(OI)(CI)W") == 0);
}
#else
void set_mode(const fs::path& dir, fs::perms mode)
{
    fs::permissions(dir, mode, fs::perm_options::replace);
}
#endif
} // namespace

SCENARIO("A missing directory is created private and then passes the check")
{
    GIVEN("a path whose last directory does not exist")
    {
        const auto dir = fresh_parent("private-dir-missing") / "TheWatcher";

        WHEN("ensure_private_directory is called")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));

            THEN("the directory exists and a second call accepts it")
            {
                REQUIRE(fs::is_directory(dir));
                REQUIRE_NOTHROW(ensure_private_directory(dir));
            }

            AND_THEN("a file can still be created in it by this user")
            {
                std::ofstream file(dir / "probe.txt");
                REQUIRE(file.is_open());
            }
        }
    }

    GIVEN("a path with several missing levels")
    {
        const auto dir = fresh_parent("private-dir-nested") / "a" / "b" / "c";

        THEN("every level is created and the result passes the check")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));
            REQUIRE(fs::is_directory(dir));
            REQUIRE_NOTHROW(ensure_private_directory(dir));
        }
    }
}

SCENARIO("An existing directory that other users can write to is refused, not repaired")
{
    GIVEN("a private directory that is then opened up to other users")
    {
        const auto dir = fresh_parent("private-dir-open") / "conf";
        ensure_private_directory(dir);
#ifdef _WIN32
        grant_users_write(dir);
#else
        set_mode(dir, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all); // 0777
#endif

        WHEN("ensure_private_directory is called")
        {
            THEN("it throws an error that names the directory and says how to fix it")
            {
                const auto message = refusal_message(dir);
                REQUIRE(message.find(dir.string()) != std::string::npos);
#ifdef _WIN32
                REQUIRE(message.find("icacls") != std::string::npos);
#else
                REQUIRE(message.find("chmod") != std::string::npos);
#endif
                REQUIRE(message.find("--config") != std::string::npos);
            }

            AND_THEN("the permissions were not rewritten behind the caller's back")
            {
                REQUIRE_THROWS(ensure_private_directory(dir));
                REQUIRE_THROWS(ensure_private_directory(dir));
            }
        }
    }
}

#ifdef _WIN32
SCENARIO("The ACL the installer sets on the config directory passes the check")
{
    GIVEN("a directory with the installer's ACL: Administrators and SYSTEM full control, nothing inherited")
    {
        const auto dir = fresh_parent("private-dir-installer") / "TheWatcher";
        fs::create_directories(dir);
        REQUIRE(run_icacls("\"" + dir.string() +
                           "\" /inheritance:r /remove:g *S-1-5-32-545 *S-1-5-11 *S-1-1-0 "
                           "/grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F") == 0);

        THEN("it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));
        }

        // Give the current user access back so the test directory can be cleaned up.
        REQUIRE(run_icacls("\"" + dir.string() + "\" /grant:r \"%USERNAME%\":(OI)(CI)F") == 0);
    }
}

namespace detail = thewatcher::common::detail;

namespace
{
std::vector<unsigned char> make_sid(WELL_KNOWN_SID_TYPE type)
{
    std::vector<unsigned char> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    REQUIRE(CreateWellKnownSid(type, nullptr, sid.data(), &size));
    return sid;
}

// An ACL built in memory, so a test does not depend on what the file system accepts.
class MemoryAcl
{
public:
    MemoryAcl()
        : buffer_(1024)
    {
        REQUIRE(InitializeAcl(acl(), static_cast<DWORD>(buffer_.size()), ACL_REVISION_DS));
    }

    PACL acl()
    {
        return reinterpret_cast<PACL>(buffer_.data());
    }

    void allow(std::vector<unsigned char>& sid, DWORD mask, BYTE flags = 0)
    {
        REQUIRE(AddAccessAllowedAceEx(acl(), ACL_REVISION_DS, flags, mask, sid.data()));
    }

    void deny(std::vector<unsigned char>& sid, DWORD mask)
    {
        REQUIRE(AddAccessDeniedAce(acl(), ACL_REVISION_DS, mask, sid.data()));
    }

    void allow_object(std::vector<unsigned char>& sid, DWORD mask)
    {
        GUID object_type{
            0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}
        };
        REQUIRE(AddAccessAllowedObjectAce(acl(), ACL_REVISION_DS, 0, mask, &object_type, nullptr, sid.data()));
    }

private:
    std::vector<unsigned char> buffer_;
};
} // namespace

SCENARIO("An ancestor that other users can rename or replace is refused (Windows)")
{
    GIVEN("a parent directory whose DACL grants Everyone FILE_DELETE_CHILD")
    {
        const auto parent = fresh_parent("private-dir-ancestor-delete-child");
        REQUIRE(run_icacls("\"" + parent.string() + "\" /grant *S-1-1-0:(DC)") == 0);

        WHEN("ensure_private_directory is called on a directory below it")
        {
            const auto message = refusal_message(parent / "conf");

            THEN("the error names the ancestor and the account")
            {
                REQUIRE(message.find(parent.string()) != std::string::npos);
                REQUIRE(message.find("Everyone") != std::string::npos);
            }
        }
    }

    GIVEN("one parent that grants Users DELETE and another that grants Users WRITE_DAC")
    {
        const auto delete_parent = fresh_parent("private-dir-ancestor-delete");
        const auto dac_parent = fresh_parent("private-dir-ancestor-write-dac");
        REQUIRE(run_icacls("\"" + delete_parent.string() + "\" /grant *S-1-5-32-545:(D)") == 0);
        REQUIRE(run_icacls("\"" + dac_parent.string() + "\" /grant *S-1-5-32-545:(WDAC)") == 0);

        THEN("both are refused, each naming its own ancestor")
        {
            REQUIRE(refusal_message(delete_parent / "conf").find(delete_parent.string()) != std::string::npos);
            REQUIRE(refusal_message(dac_parent / "conf").find(dac_parent.string()) != std::string::npos);
        }
    }

    GIVEN("a parent that grants Users add-file and add-subdirectory, like C:\\ProgramData, plus child-only entries")
    {
        const auto parent = fresh_parent("private-dir-ancestor-programdata");
        REQUIRE(run_icacls("\"" + parent.string() + "\" /grant *S-1-5-32-545:(RX) *S-1-5-32-545:(CI)(WD,AD,WEA,WA)") ==
                0);
        // Entries that only apply to children, like CREATOR OWNER and Authenticated Users on C:\.
        REQUIRE(run_icacls("\"" + parent.string() + "\" /grant *S-1-1-0:(OI)(CI)(IO)(DC,D,WDAC,WO)") == 0);

        THEN("a directory below it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(parent / "conf"));
        }
    }
}

SCENARIO("The ACL classification fails closed on entries it cannot check (Windows)")
{
    GIVEN("an ACL with an object-specific allow entry for Everyone")
    {
        auto system = make_sid(WinLocalSystemSid);
        auto everyone = make_sid(WinWorldSid);
        MemoryAcl acl;
        acl.allow(system, FILE_ALL_ACCESS);
        acl.allow_object(everyone, FILE_ALL_ACCESS);

        THEN("it is refused for the secret's directory and for an ancestor, naming the entry type")
        {
            for (const auto role : {detail::AclRole::Leaf, detail::AclRole::Ancestor})
            {
                const auto reason = detail::find_unsafe_dacl_entry(acl.acl(), system.data(), role);
                REQUIRE(reason.has_value());
                REQUIRE(reason->find("an access control entry of type") != std::string::npos);
                REQUIRE(reason->find("cannot be checked") != std::string::npos);
            }
        }
    }

    GIVEN("an ACL whose only unusual entry is a deny entry")
    {
        auto system = make_sid(WinLocalSystemSid);
        auto admins = make_sid(WinBuiltinAdministratorsSid);
        auto everyone = make_sid(WinWorldSid);
        MemoryAcl acl;
        acl.deny(everyone, FILE_ALL_ACCESS);
        acl.allow(system, FILE_ALL_ACCESS);
        acl.allow(admins, FILE_ALL_ACCESS);

        THEN("it is skipped, because a deny entry can only remove access")
        {
            for (const auto role : {detail::AclRole::Leaf, detail::AclRole::Ancestor})
                REQUIRE_FALSE(detail::find_unsafe_dacl_entry(acl.acl(), system.data(), role).has_value());
        }
    }

    GIVEN("a missing DACL")
    {
        auto system = make_sid(WinLocalSystemSid);

        THEN("it is refused for both roles")
        {
            REQUIRE(detail::find_unsafe_dacl_entry(nullptr, system.data(), detail::AclRole::Leaf).has_value());
            REQUIRE(detail::find_unsafe_dacl_entry(nullptr, system.data(), detail::AclRole::Ancestor).has_value());
        }
    }
}

SCENARIO("Ancestors are judged on the rights that replace a directory, the secret's directory on any write (Windows)")
{
    GIVEN("an ACL granting Everyone FILE_DELETE_CHILD through an inherit-only entry")
    {
        auto system = make_sid(WinLocalSystemSid);
        auto everyone = make_sid(WinWorldSid);
        MemoryAcl acl;
        acl.allow(system, FILE_ALL_ACCESS, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
        acl.allow(everyone, FILE_DELETE_CHILD, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE);

        THEN("the leaf check refuses it and the ancestor check accepts it")
        {
            REQUIRE(detail::find_unsafe_dacl_entry(acl.acl(), system.data(), detail::AclRole::Leaf).has_value());
            REQUIRE_FALSE(
                detail::find_unsafe_dacl_entry(acl.acl(), system.data(), detail::AclRole::Ancestor).has_value());
        }
    }

    GIVEN("an ACL granting Everyone FILE_ADD_FILE and FILE_ADD_SUBDIRECTORY on the directory itself")
    {
        auto system = make_sid(WinLocalSystemSid);
        auto everyone = make_sid(WinWorldSid);
        MemoryAcl acl;
        acl.allow(system, FILE_ALL_ACCESS);
        acl.allow(everyone, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);

        THEN("the ancestor check accepts it and the leaf check refuses it")
        {
            REQUIRE_FALSE(
                detail::find_unsafe_dacl_entry(acl.acl(), system.data(), detail::AclRole::Ancestor).has_value());
            REQUIRE(detail::find_unsafe_dacl_entry(acl.acl(), system.data(), detail::AclRole::Leaf).has_value());
        }
    }

    GIVEN("an ACL granting Everyone GENERIC_ALL on the directory itself")
    {
        auto system = make_sid(WinLocalSystemSid);
        auto everyone = make_sid(WinWorldSid);
        MemoryAcl acl;
        acl.allow(system, FILE_ALL_ACCESS);
        acl.allow(everyone, GENERIC_ALL);

        THEN("the ancestor check refuses it")
        {
            REQUIRE(detail::find_unsafe_dacl_entry(acl.acl(), system.data(), detail::AclRole::Ancestor).has_value());
        }
    }
}
#endif

#ifndef _WIN32
SCENARIO("An ancestor that other users can rename or replace is refused (POSIX)")
{
    GIVEN("a parent directory writable by its group, or by other users, without the sticky bit")
    {
        const auto group_parent = fresh_parent("private-dir-ancestor-group");
        const auto other_parent = fresh_parent("private-dir-ancestor-other");
        set_mode(group_parent, fs::perms::owner_all | fs::perms::group_all);  // 0770
        set_mode(other_parent, fs::perms::owner_all | fs::perms::others_all); // 0707

        WHEN("ensure_private_directory is called on a directory below it")
        {
            const auto group_message = refusal_message(group_parent / "conf");
            const auto other_message = refusal_message(other_parent / "conf");

            THEN("the error names the ancestor")
            {
                REQUIRE(group_message.find(group_parent.string()) != std::string::npos);
                REQUIRE(other_message.find(other_parent.string()) != std::string::npos);
            }
        }
    }

    GIVEN("a writable grandparent with a private parent below it")
    {
        const auto grandparent = fresh_parent("private-dir-ancestor-grandparent");
        const auto parent = grandparent / "parent";
        fs::create_directories(parent);
        set_mode(parent, fs::perms::owner_all);
        set_mode(grandparent, fs::perms::owner_all | fs::perms::others_write);

        THEN("it is refused, because every ancestor up to the root is checked")
        {
            REQUIRE(refusal_message(parent / "conf").find(grandparent.string()) != std::string::npos);
        }
    }
}

SCENARIO("A sticky world-writable ancestor is accepted, like /tmp (POSIX)")
{
    GIVEN("a parent directory with mode 1777 owned by this user")
    {
        const auto parent = fresh_parent("private-dir-ancestor-sticky");
        set_mode(parent, fs::perms::all | fs::perms::sticky_bit);

        THEN("a private directory below it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(parent / "conf"));
        }
    }

    GIVEN("the same parent directory with the sticky bit removed (mode 0777)")
    {
        const auto parent = fresh_parent("private-dir-ancestor-unsticky");
        set_mode(parent, fs::perms::all);

        THEN("it is refused")
        {
            REQUIRE_THROWS(ensure_private_directory(parent / "conf"));
        }
    }
}

SCENARIO("A default-like tree of 0755 directories is accepted (POSIX)")
{
    GIVEN("a nested tree of 0755 directories, like /usr/local/etc")
    {
        const auto etc = fresh_parent("private-dir-ancestor-default") / "etc";
        fs::create_directories(etc);
        set_mode(etc, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                          fs::perms::others_exec);

        THEN("the directory created below it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(etc / "thewatcher"));
        }
    }
}
#endif

#ifndef _WIN32
SCENARIO("A path that goes through a symbolic link is judged where the link lives (POSIX)")
{
    GIVEN("a private directory reached through a symlink that lives in a directory other users can write to")
    {
        const auto target = fresh_parent("private-dir-link-target") / "conf";
        ensure_private_directory(target);
        const auto loose = fresh_parent("private-dir-link-loose");
        set_mode(loose, fs::perms::all); // 0777, no sticky bit
        const auto link = loose / "link";
        fs::create_symlink(target, link);

        WHEN("ensure_private_directory is called with the path through the link")
        {
            const auto message = refusal_message(link);

            THEN("it is refused and the error names the link's directory and says it is not private")
            {
                REQUIRE(message.find("is not private") != std::string::npos);
                REQUIRE(message.find(link.string()) != std::string::npos);
                REQUIRE(message.find(loose.string()) != std::string::npos);
            }
        }

        WHEN("the path goes through the link and then a further component")
        {
            fs::create_directories(target / "inner");
            set_mode(target / "inner", fs::perms::owner_all);

            THEN("it is refused too")
            {
                REQUIRE_THROWS(ensure_private_directory(link / "inner"));
            }
        }
    }

    GIVEN("a link to a link, where only the first link lives in a writable directory")
    {
        const auto target = fresh_parent("private-dir-link2-target") / "conf";
        ensure_private_directory(target);
        const auto clean = fresh_parent("private-dir-link2-clean");
        const auto loose = fresh_parent("private-dir-link2-loose");
        set_mode(loose, fs::perms::all);
        fs::create_symlink(target, clean / "second");
        fs::create_symlink(clean / "second", loose / "first");

        THEN("it is refused")
        {
            REQUIRE_THROWS(ensure_private_directory(loose / "first"));
        }
    }

    GIVEN("the same link in a clean directory")
    {
        const auto target = fresh_parent("private-dir-link3-target") / "conf";
        ensure_private_directory(target);
        const auto clean = fresh_parent("private-dir-link3-clean");
        const auto link = clean / "link";
        fs::create_symlink(target, link);

        THEN("it is accepted, also with a '..' component after it")
        {
            REQUIRE_NOTHROW(ensure_private_directory(link));
            REQUIRE_NOTHROW(ensure_private_directory(link / ".." / "conf"));
        }
    }

    GIVEN("a link in a sticky world-writable directory, owned by this user")
    {
        const auto target = fresh_parent("private-dir-link4-target") / "conf";
        ensure_private_directory(target);
        const auto sticky = fresh_parent("private-dir-link4-sticky");
        set_mode(sticky, fs::perms::all | fs::perms::sticky_bit);
        fs::create_symlink(target, sticky / "link");

        THEN("it is accepted, because only its owner can rename it")
        {
            REQUIRE_NOTHROW(ensure_private_directory(sticky / "link"));
        }
    }
}
#endif

#ifdef _WIN32
SCENARIO("A path that goes through a junction is judged where the junction lives (Windows)")
{
    const auto make_junction = [](const fs::path& link, const fs::path& target) {
        return std::system(("\"mklink /J \"" + link.string() + "\" \"" + target.string() + "\" > NUL\"").c_str());
    };

    GIVEN("a private directory reached through a junction inside a directory that grants Users modify rights")
    {
        const auto target = fresh_parent("private-dir-junction-target") / "conf";
        ensure_private_directory(target);
        const auto loose = fresh_parent("private-dir-junction-loose");
        REQUIRE(run_icacls("\"" + loose.string() + "\" /grant *S-1-5-32-545:(OI)(CI)M") == 0);
        const auto link = loose / "link";
        REQUIRE(make_junction(link, target) == 0);
        // The junction's own ACL is private (/L: the link itself, not its target), so
        // only the directory it lives in is wrong.
        REQUIRE(run_icacls("\"" + link.string() +
                           "\" /L /inheritance:r /grant:r *S-1-5-32-544:F *S-1-5-18:F \"%USERNAME%\":F") == 0);

        WHEN("ensure_private_directory is called with the path through the junction")
        {
            const auto message = refusal_message(link);

            THEN("it is refused and the error says it is not private and names the junction")
            {
                REQUIRE(message.find("is not private") != std::string::npos);
                REQUIRE(message.find(link.string()) != std::string::npos);
            }
        }
    }

    GIVEN("the same junction in a clean directory")
    {
        const auto target = fresh_parent("private-dir-junction2-target") / "conf";
        ensure_private_directory(target);
        const auto clean = fresh_parent("private-dir-junction2-clean");
        REQUIRE(run_icacls("\"" + clean.string() +
                           "\" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F "
                           "\"%USERNAME%\":(OI)(CI)F") == 0);
        const auto link = clean / "link";
        REQUIRE(make_junction(link, target) == 0);

        THEN("it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(link));
        }
    }
}
#endif

// Hidden: depends on the machine. Run with "[default-paths]" to see that the
// real default parents (C:\ProgramData, /etc, /usr/local/etc) and their
// ancestors are accepted.
SCENARIO("The default config locations of this machine pass the check", "[.][default-paths]")
{
#ifdef _WIN32
    GIVEN("a new directory directly under %ProgramData%")
    {
        char* program_data = nullptr;
        std::size_t length = 0;
        REQUIRE(_dupenv_s(&program_data, &length, "PROGRAMDATA") == 0);
        REQUIRE(program_data != nullptr);
        const fs::path dir = fs::path(program_data) / "TheWatcher-ancestor-probe";
        std::free(program_data);
        fs::remove_all(dir);

        THEN("it is created private and accepted, so C:\\ProgramData and C:\\ pass as ancestors")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));
            fs::remove_all(dir);
        }
    }
#else
    GIVEN("the existing /etc and /usr/local/etc")
    {
        THEN("each is accepted, so / /usr and /usr/local pass as ancestors")
        {
            for (const char* path : {"/etc", "/usr/local/etc"})
                if (fs::is_directory(path))
                    REQUIRE_NOTHROW(ensure_private_directory(path));
        }
    }
#endif
}

#ifndef _WIN32
SCENARIO("A POSIX directory is private when only its owner can write")
{
    GIVEN("directories with different modes")
    {
        const auto parent = fresh_parent("private-dir-modes");
        const auto owner_only = parent / "owner-only";
        const auto readable = parent / "readable";
        const auto group_write = parent / "group-write";
        const auto other_write = parent / "other-write";
        for (const auto& dir : {owner_only, readable, group_write, other_write})
            fs::create_directories(dir);
        set_mode(owner_only, fs::perms::owner_all); // 0700
        set_mode(readable, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                               fs::perms::others_read | fs::perms::others_exec); // 0755
        set_mode(group_write, fs::perms::owner_all | fs::perms::group_write);    // 0720
        set_mode(other_write, fs::perms::owner_all | fs::perms::others_write);   // 0702

        THEN("0700 and 0755 are accepted, group or other write is refused")
        {
            REQUIRE_NOTHROW(ensure_private_directory(owner_only));
            REQUIRE_NOTHROW(ensure_private_directory(readable));
            REQUIRE_THROWS(ensure_private_directory(group_write));
            REQUIRE_THROWS(ensure_private_directory(other_write));
        }

        THEN("a newly created directory is 0700")
        {
            const auto created = parent / "created";
            ensure_private_directory(created);
            const auto perms = fs::status(created).permissions();
            REQUIRE((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        }
    }
}
#endif
