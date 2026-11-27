#include "agent/collectors/process_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::agent;

namespace
{

ProcRecord make_proc(uint32_t pid, std::string name, double cpu, std::string exe_name = {})
{
    ProcRecord rec;
    rec.pid = pid;
    rec.name = std::move(name);
    rec.cpu_percent = cpu;
    rec.exe_name = std::move(exe_name);
    return rec;
}

bool reported(const std::vector<ProcRecord>& procs, uint32_t pid)
{
    for (const auto& p : procs)
    {
        if (p.pid == pid)
            return true;
    }
    return false;
}

} // namespace

SCENARIO("Executable names are taken from paths and command lines (issue #95)")
{
    GIVEN("executable paths from /proc/<pid>/exe and argv[0]")
    {
        THEN("the basename is extracted")
        {
            REQUIRE(executable_basename("/usr/lib/systemd/systemd-journald") == "systemd-journald");
            REQUIRE(executable_basename("C:\\Windows\\System32\\svchost.exe") == "svchost.exe");
            REQUIRE(executable_basename("plain") == "plain");
            REQUIRE(executable_basename("").empty());
        }

        THEN("a deleted-binary suffix added by the kernel is removed")
        {
            REQUIRE(executable_basename("/usr/sbin/nginx (deleted)") == "nginx");
        }
    }

    GIVEN("a NUL separated /proc/<pid>/cmdline")
    {
        const std::string cmdline("/usr/lib/systemd/systemd-journald\0--flag\0", 40);

        THEN("the basename of the first argument is returned")
        {
            REQUIRE(argv0_basename(cmdline) == "systemd-journald");
            REQUIRE(argv0_basename("").empty());
        }
    }
}

SCENARIO("A watch matches a process by name or executable basename (issue #95)")
{
    GIVEN("a watch for systemd-journald and a process whose comm is truncated to 15 characters")
    {
        WHEN("the executable basename is known")
        {
            THEN("the watch matches")
            {
                REQUIRE(process_matches_watch("systemd-journald", "systemd-journal", "systemd-journald", false));
            }
        }

        WHEN("the executable basename is unknown")
        {
            THEN("the truncated name does not match the longer watch")
            {
                REQUIRE_FALSE(process_matches_watch("systemd-journald", "systemd-journal", "", false));
            }
        }
    }

    GIVEN("a watch whose name equals comm")
    {
        THEN("it matches without an executable name")
        {
            REQUIRE(process_matches_watch("sshd", "sshd", "", false));
            REQUIRE_FALSE(process_matches_watch("sshd", "ssh", "ssh", false));
        }
    }

    GIVEN("names differing only in case")
    {
        THEN("they match only when matching is case insensitive, as on Windows")
        {
            REQUIRE_FALSE(process_matches_watch("Notepad.exe", "notepad.exe", "", false));
            REQUIRE(process_matches_watch("Notepad.exe", "notepad.exe", "", true));
        }
    }

    GIVEN("an empty watch name")
    {
        THEN("it never matches")
        {
            REQUIRE_FALSE(process_matches_watch("", "", "", false));
        }
    }
}

SCENARIO("Process CPU is reported as percent of one core on every platform (issue #95)")
{
    GIVEN("a process that used two full cores for one second at 100 ticks per second")
    {
        THEN("it reports 200 percent")
        {
            REQUIRE(cpu_percent_of_one_core(200, 1.0, 100.0) == 200.0);
        }
    }

    GIVEN("a process that used half a core for two seconds with 100-ns ticks")
    {
        THEN("it reports 50 percent")
        {
            REQUIRE(cpu_percent_of_one_core(10000000, 2.0, 10000000.0) == 50.0);
        }
    }

    GIVEN("a zero interval or a zero tick rate")
    {
        THEN("it reports 0 rather than dividing by zero")
        {
            REQUIRE(cpu_percent_of_one_core(100, 0.0, 100.0) == 0.0);
            REQUIRE(cpu_percent_of_one_core(100, 1.0, 0.0) == 0.0);
        }
    }
}

SCENARIO("Reported processes are the top N plus every watched match (issue #95, #137)")
{
    GIVEN("five processes and a limit of two")
    {
        const std::vector<ProcRecord> all{make_proc(1, "a", 5.0), make_proc(2, "b", 50.0), make_proc(3, "c", 30.0),
                                          make_proc(4, "d", 1.0), make_proc(5, "e", 0.0)};

        WHEN("there are no watches")
        {
            const auto out = select_reported_processes(all, 2, {}, false);

            THEN("the two busiest processes are reported, busiest first")
            {
                REQUIRE(out.size() == 2);
                REQUIRE(out[0].pid == 2);
                REQUIRE(out[1].pid == 3);
            }
        }

        WHEN("a watch names an idle process outside the top N")
        {
            const auto out = select_reported_processes(all, 2,
                                                       {
                                                           {"e", 1, true}
            },
                                                       false);

            THEN("the watched process is added once")
            {
                REQUIRE(out.size() == 3);
                REQUIRE(reported(out, 5));
            }
        }

        WHEN("a watch names a process already in the top N")
        {
            const auto out = select_reported_processes(all, 2,
                                                       {
                                                           {"b", 1, true}
            },
                                                       false);

            THEN("it is not duplicated")
            {
                REQUIRE(out.size() == 2);
            }
        }

        WHEN("a watch is disabled or has an empty name")
        {
            const auto out = select_reported_processes(all, 2,
                                                       {
                                                           {"e", 1, false},
                                                           {"",  1, true }
            },
                                                       false);

            THEN("it adds nothing")
            {
                REQUIRE(out.size() == 2);
            }
        }

        WHEN("the limit is zero or negative")
        {
            THEN("no top processes are reported, only watched ones")
            {
                REQUIRE(select_reported_processes(all, 0, {}, false).empty());
                REQUIRE(select_reported_processes(all, -3, {}, false).empty());
                REQUIRE(select_reported_processes(all, 0,
                                                  {
                                                      {"d", 1, true}
                },
                                                  false)
                            .size() == 1);
            }
        }

        WHEN("the limit is larger than the process count")
        {
            THEN("all processes are reported")
            {
                REQUIRE(select_reported_processes(all, 100, {}, false).size() == 5);
            }
        }
    }

    GIVEN("a process with a truncated comm and a full executable name")
    {
        const std::vector<ProcRecord> all{make_proc(10, "systemd-journal", 0.0, "systemd-journald"),
                                          make_proc(11, "busy", 90.0)};

        WHEN("a long watch name is configured and the limit excludes the process")
        {
            const auto out = select_reported_processes(all, 1,
                                                       {
                                                           {"systemd-journald", 1, true}
            },
                                                       false);

            THEN("the process is reported under the watch name so the dashboard can count it")
            {
                REQUIRE(out.size() == 2);
                REQUIRE(out[1].pid == 10);
                REQUIRE(out[1].name == "systemd-journald");
            }
        }
    }

    GIVEN("a Windows-style process whose name differs from the watch only in case")
    {
        const std::vector<ProcRecord> all{make_proc(20, "NOTEPAD.EXE", 0.0)};

        WHEN("matching is case insensitive")
        {
            const auto out = select_reported_processes(all, 0,
                                                       {
                                                           {"notepad.exe", 1, true}
            },
                                                       true);

            THEN("it is reported under the configured watch name")
            {
                REQUIRE(out.size() == 1);
                REQUIRE(out[0].name == "notepad.exe");
            }
        }
    }
}
