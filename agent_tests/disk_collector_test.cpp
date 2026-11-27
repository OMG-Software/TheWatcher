#include "agent/collectors/disk_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace thewatcher;
using namespace thewatcher::agent;

SCENARIO("DiskCollector reports at least one mount with sane metrics")
{
    GIVEN("a fresh DiskCollector on the host")
    {
        DiskCollector collector;
        SystemMetrics metrics;

        WHEN("the collector runs once")
        {
            collector.update(metrics);

            THEN("at least one disk is reported")
            {
                REQUIRE_FALSE(metrics.disks.empty());
            }

            AND_THEN("each reported disk has a mount point and bounded usage")
            {
                for (const auto& disk : metrics.disks)
                {
                    REQUIRE_FALSE(disk.mount_point.empty());
                    REQUIRE(disk.total_bytes > 0);
                    REQUIRE(disk.usage_percent >= 0.0);
                    REQUIRE(disk.usage_percent <= 100.0);
                }
            }
        }
    }
}

#ifdef _WIN32

SCENARIO("On Windows, DiskCollector enumerates every fixed drive the system exposes")
{
    GIVEN("the set of fixed drives reported by GetLogicalDrives + GetDriveTypeA")
    {
        std::set<std::string> expected_mount_points;
        const DWORD mask = ::GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if ((mask & (1u << i)) == 0)
                continue;
            char root[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
            if (::GetDriveTypeA(root) != DRIVE_FIXED)
                continue;
            std::error_code ec;
            (void)std::filesystem::space(root, ec);
            if (ec)
                continue;
            expected_mount_points.insert(root);
        }

        WHEN("the disk collector runs")
        {
            DiskCollector collector;
            SystemMetrics metrics;
            collector.update(metrics);

            std::set<std::string> actual_mount_points;
            for (const auto& disk : metrics.disks)
                actual_mount_points.insert(disk.mount_point);

            THEN("the collector reports the same fixed-drive set")
            {
                REQUIRE(actual_mount_points == expected_mount_points);
            }
        }
    }
}

#endif // _WIN32

SCENARIO("Disk usage leaves root-reserved space out of free and usage, as df does (issue #158)")
{
    GIVEN("a 100-unit volume with 50 units free, of which 45 are available to normal users")
    {
        std::filesystem::space_info space{};
        space.capacity = 100;
        space.free = 50;
        space.available = 45;
        DiskMetrics disk;

        WHEN("the space info is applied")
        {
            apply_space_info(disk, space);

            THEN("free is what normal users can write and usage is used / (used + available)")
            {
                REQUIRE(disk.total_bytes == 100);
                REQUIRE(disk.free_bytes == 45);
                REQUIRE(disk.used_bytes == 50);
                REQUIRE(disk.usage_percent > 52.6);
                REQUIRE(disk.usage_percent < 52.7); // 50 / 95
            }
        }
    }

    GIVEN("a volume that is full for normal users but still has root-reserved blocks")
    {
        std::filesystem::space_info space{};
        space.capacity = 1000;
        space.free = 50;
        space.available = 0;
        DiskMetrics disk;

        WHEN("the space info is applied")
        {
            apply_space_info(disk, space);

            THEN("usage is 100% and nothing is reported free")
            {
                REQUIRE(disk.free_bytes == 0);
                REQUIRE(disk.usage_percent == 100.0);
            }
        }
    }

    GIVEN("an empty or unreadable volume")
    {
        std::filesystem::space_info space{};
        DiskMetrics disk;

        WHEN("the space info is applied")
        {
            apply_space_info(disk, space);

            THEN("usage is 0% instead of a division by zero")
            {
                REQUIRE(disk.usage_percent == 0.0);
            }
        }
    }

    GIVEN("a filesystem that reports free above capacity")
    {
        std::filesystem::space_info space{};
        space.capacity = 100;
        space.free = 120;
        space.available = 110;
        DiskMetrics disk;

        WHEN("the space info is applied")
        {
            apply_space_info(disk, space);

            THEN("free and used are clamped to capacity instead of underflowing")
            {
                REQUIRE(disk.free_bytes == 100);
                REQUIRE(disk.used_bytes == 0);
                REQUIRE(disk.usage_percent == 0.0);
            }
        }
    }

    GIVEN("a filesystem that reports available above free")
    {
        std::filesystem::space_info space{};
        space.capacity = 100;
        space.free = 50;
        space.available = 80;
        DiskMetrics disk;

        WHEN("the space info is applied")
        {
            apply_space_info(disk, space);

            THEN("available is clamped to free")
            {
                REQUIRE(disk.free_bytes == 50);
                REQUIRE(disk.used_bytes == 50);
                REQUIRE(disk.usage_percent == 50.0);
            }
        }
    }
}

SCENARIO("Mount table fields are unescaped as the kernel escapes them (issue #96)")
{
    GIVEN("a /proc/self/mounts field where the kernel escaped a space, tab, newline and backslash")
    {
        WHEN("it is unescaped")
        {
            THEN("each octal sequence becomes its character")
            {
                REQUIRE(unescape_mount_field(R"(/mnt/My\040Disk)") == "/mnt/My Disk");
                REQUIRE(unescape_mount_field(R"(a\011b\012c\134d)") == "a\tb\nc\\d");
            }
        }
    }

    GIVEN("fields without a complete octal escape")
    {
        THEN("they are returned unchanged")
        {
            REQUIRE(unescape_mount_field("/plain/path") == "/plain/path");
            REQUIRE(unescape_mount_field(R"(trailing\04)") == R"(trailing\04)");
            REQUIRE(unescape_mount_field(R"(bad\9zz)") == R"(bad\9zz)");
            REQUIRE(unescape_mount_field("").empty());
        }
    }
}

SCENARIO("A mount table is parsed into entries (issue #96)")
{
    GIVEN("mount table text with an escaped mount point and a short line")
    {
        const std::string text = "/dev/sda1 / ext4 rw,relatime 0 0\n"
                                 "/dev/sdb1 /mnt/My\\040Disk ext4 rw 0 0\n"
                                 "garbage\n"
                                 "tmpfs /run tmpfs rw 0 0\n";

        WHEN("it is parsed")
        {
            const auto entries = parse_mount_table(text);

            THEN("well-formed lines become entries with decoded paths and bad lines are skipped")
            {
                REQUIRE(entries.size() == 3);
                REQUIRE(entries[0].device == "/dev/sda1");
                REQUIRE(entries[0].mount_point == "/");
                REQUIRE(entries[0].filesystem == "ext4");
                REQUIRE(entries[1].mount_point == "/mnt/My Disk");
                REQUIRE(entries[2].filesystem == "tmpfs");
            }
        }
    }
}

SCENARIO("Pseudo, loop, snap and duplicate mounts are not reported (issue #96)")
{
    GIVEN("an Ubuntu-like mount table with snaps, pseudo filesystems and a bind mount")
    {
        const auto entries = parse_mount_table("/dev/mapper/vg-root / ext4 rw 0 0\n"
                                               "proc /proc proc rw 0 0\n"
                                               "tmpfs /run tmpfs rw 0 0\n"
                                               "/dev/loop3 /snap/core/1 squashfs ro 0 0\n"
                                               "/dev/loop4 /snap/lxd/2 ext4 ro 0 0\n"
                                               "/dev/sda2 /boot ext4 rw 0 0\n"
                                               "/dev/mapper/vg-root /srv/bind ext4 rw 0 0\n"
                                               "/dev/sdc1 /media/cd squashfs ro 0 0\n");

        WHEN("the mounts are filtered with a block device required")
        {
            const auto kept = filter_mounts(entries, true);

            THEN("only the first mount of each real block device remains")
            {
                REQUIRE(kept.size() == 2);
                REQUIRE(kept[0].mount_point == "/");
                REQUIRE(kept[1].mount_point == "/boot");
            }
        }
    }

    GIVEN("BSD-style mounts where ZFS datasets are not under /dev")
    {
        const auto entries = parse_mount_table("zroot/ROOT/default / zfs rw 0 0\n"
                                               "devfs /dev devfs rw 0 0\n"
                                               "/dev/gpt/efiboot0 /boot/efi msdosfs rw 0 0\n"
                                               "/usr/jails/base /jail/base nullfs rw 0 0\n"
                                               "fdescfs /dev/fd fdescfs rw 0 0\n");

        WHEN("the mounts are filtered without requiring a /dev prefix")
        {
            const auto kept = filter_mounts(entries, false);

            THEN("real filesystems stay and pseudo filesystems are dropped")
            {
                REQUIRE(kept.size() == 2);
                REQUIRE(kept[0].device == "zroot/ROOT/default");
                REQUIRE(kept[1].mount_point == "/boot/efi");
            }
        }
    }
}

SCENARIO("A mount's device maps to its /proc/diskstats name (issue #96)")
{
    GIVEN("a device-mapper name table read from /sys/block/dm-N/dm/name")
    {
        const std::unordered_map<std::string, std::string> dm_names{
            {"vg-root", "dm-0"},
            {"vg-home", "dm-1"}
        };

        WHEN("the device is a /dev/mapper alias")
        {
            THEN("it maps to the dm-N diskstats name")
            {
                REQUIRE(diskstats_name_for_device("/dev/mapper/vg-root", dm_names) == "dm-0");
                REQUIRE(diskstats_name_for_device("/dev/mapper/vg-home", dm_names) == "dm-1");
            }
        }

        WHEN("the device is a plain block device or already a dm node")
        {
            THEN("its basename is used")
            {
                REQUIRE(diskstats_name_for_device("/dev/sda1", dm_names) == "sda1");
                REQUIRE(diskstats_name_for_device("/dev/dm-0", dm_names) == "dm-0");
                REQUIRE(diskstats_name_for_device("/dev/nvme0n1p2", {}) == "nvme0n1p2");
            }
        }
    }
}

SCENARIO("Mounts are filtered and de-duplicated on their resolved device (issue #96)")
{
    // Stands in for realpath(): udev aliases resolve to the kernel node.
    const auto resolve = [](const std::string& device) -> std::string {
        static const std::unordered_map<std::string, std::string> links{
            {"/dev/disk/by-uuid/aaaa",    "/dev/dm-0" },
            {"/dev/mapper/vg-root",       "/dev/dm-0" },
            {"/dev/disk/by-id/snap-core", "/dev/loop7"},
            {"/dev/disk/by-label/data",   "/dev/sdb1" },
        };
        const auto it = links.find(device);
        return it != links.end() ? it->second : device;
    };

    GIVEN("one volume mounted twice through two different alias names")
    {
        const auto entries = parse_mount_table("/dev/mapper/vg-root / ext4 rw 0 0\n"
                                               "/dev/disk/by-uuid/aaaa /srv/again ext4 rw 0 0\n"
                                               "/dev/dm-0 /srv/third ext4 rw 0 0\n");

        WHEN("the mounts are filtered with a resolver")
        {
            const auto kept = filter_mounts(entries, true, resolve);

            THEN("only the first mount remains and it keeps the device string from the mount table")
            {
                REQUIRE(kept.size() == 1);
                REQUIRE(kept[0].mount_point == "/");
                REQUIRE(kept[0].device == "/dev/mapper/vg-root");
                REQUIRE(kept[0].canonical_device == "/dev/dm-0");
            }
        }
    }

    GIVEN("a by-id alias that resolves to a loop device and a by-label alias that resolves to a disk")
    {
        const auto entries = parse_mount_table("/dev/disk/by-id/snap-core /snap/core/1 ext4 ro 0 0\n"
                                               "/dev/disk/by-label/data /data ext4 rw 0 0\n");

        WHEN("the mounts are filtered with a resolver")
        {
            const auto kept = filter_mounts(entries, true, resolve);

            THEN("the loop-backed mount is excluded and the disk is kept")
            {
                REQUIRE(kept.size() == 1);
                REQUIRE(kept[0].mount_point == "/data");
                REQUIRE(kept[0].canonical_device == "/dev/sdb1");
            }
        }
    }

    GIVEN("no resolver")
    {
        WHEN("a device is filtered")
        {
            const auto kept = filter_mounts(parse_mount_table("/dev/sda1 / ext4 rw 0 0\n"), true);

            THEN("its canonical device is the device itself")
            {
                REQUIRE(kept.size() == 1);
                REQUIRE(kept[0].canonical_device == "/dev/sda1");
            }
        }
    }
}
