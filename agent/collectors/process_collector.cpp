#include "process_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(_WIN32)
// clang-format off
#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
// clang-format on
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <cerrno>

#include <pwd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#if defined(__FreeBSD__)
#include <sys/user.h>
#endif
#include <unistd.h>
#else
#error "Unsupported platform"
#endif

namespace thewatcher::agent
{

// ── Platform independent helpers (tested on every platform) ───────────────────

std::string executable_basename(std::string_view path)
{
    constexpr std::string_view deleted_suffix = " (deleted)";
    if (path.size() >= deleted_suffix.size() && path.substr(path.size() - deleted_suffix.size()) == deleted_suffix)
        path.remove_suffix(deleted_suffix.size());
    const auto pos = path.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

std::string argv0_basename(std::string_view cmdline)
{
    return executable_basename(cmdline.substr(0, cmdline.find('\0')));
}

namespace
{

    bool equal_names(std::string_view a, std::string_view b, bool case_insensitive)
    {
        if (a.size() != b.size())
            return false;
        if (!case_insensitive)
            return a == b;
        return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

} // namespace

bool process_matches_watch(std::string_view watch_name, std::string_view name, std::string_view exe_name,
                           bool case_insensitive)
{
    if (watch_name.empty())
        return false;
    return equal_names(watch_name, name, case_insensitive) ||
           (!exe_name.empty() && equal_names(watch_name, exe_name, case_insensitive));
}

bool open_process_should_retry_limited(unsigned long last_error)
{
    constexpr unsigned long error_access_denied = 5; // ERROR_ACCESS_DENIED
    return last_error == error_access_denied;
}

double cpu_percent_of_one_core(uint64_t delta_ticks, double elapsed_seconds, double ticks_per_second)
{
    if (elapsed_seconds <= 0.0 || ticks_per_second <= 0.0)
        return 0.0;
    return 100.0 * static_cast<double>(delta_ticks) / (elapsed_seconds * ticks_per_second);
}

std::vector<ProcRecord> select_reported_processes(std::vector<ProcRecord> all, int limit,
                                                  const std::vector<ProcessWatchConfig>& watches, bool case_insensitive)
{
    // Watched processes are reported under the watch's name (first watch wins).
    std::unordered_set<uint32_t> watched_pids;
    for (const auto& watch : watches)
    {
        if (!watch.enabled || watch.name.empty())
            continue;
        for (auto& proc : all)
        {
            if (watched_pids.count(proc.pid) != 0 ||
                !process_matches_watch(watch.name, proc.name, proc.exe_name, case_insensitive))
                continue;
            proc.name = watch.name;
            watched_pids.insert(proc.pid);
        }
    }

    std::stable_sort(all.begin(), all.end(), [](const ProcRecord& a, const ProcRecord& b) {
        return a.cpu_percent > b.cpu_percent;
    });

    const auto top_count = static_cast<std::size_t>(std::max(limit, 0));
    std::vector<ProcRecord> out;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (i < top_count || watched_pids.count(all[i].pid) != 0)
            out.push_back(std::move(all[i]));
    }
    return out;
}

namespace
{

#if !defined(_WIN32)

    std::string uid_to_name(uid_t uid)
    {
        char buf[1024];
        struct passwd pw{}, *result = nullptr;
        ::getpwuid_r(uid, &pw, buf, sizeof(buf), &result);
        return result ? result->pw_name : std::to_string(uid);
    }

#endif

// ── Linux ─────────────────────────────────────────────────────────────────────
#ifdef __linux__

    constexpr bool kCaseInsensitiveNames = false;

    // Owns an open DIR* and closes it on every exit path.
    struct DirCloser
    {
        void operator()(DIR* dir) const noexcept
        {
            ::closedir(dir);
        }
    };
    using DirHandle = std::unique_ptr<DIR, DirCloser>;

    double ticks_per_second()
    {
        return static_cast<double>(::sysconf(_SC_CLK_TCK));
    }

    bool parse_proc_stat(uint32_t pid, ProcRecord& rec)
    {
        std::string path = "/proc/" + std::to_string(pid) + "/stat";
        std::ifstream f(path);
        if (!f)
            return false;
        std::string line;
        if (!std::getline(f, line))
            return false;

        // comm is enclosed in parens and may contain spaces/parens
        auto comm_start = line.find('(');
        auto comm_end = line.rfind(')');
        if (comm_start == std::string::npos || comm_end == std::string::npos)
            return false;

        rec.name = line.substr(comm_start + 1, comm_end - comm_start - 1);

        std::istringstream rest(line.substr(comm_end + 2));
        char state;
        int ppid, pgrp, session, tty, tpgid;
        unsigned long flags, minflt, cminflt, majflt, cmajflt;
        unsigned long utime, stime;
        long cutime, cstime, priority, nice, num_threads;
        unsigned long long starttime;
        unsigned long vsize;
        long rss;

        rest >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt >>
            utime >> stime >> cutime >> cstime >> priority >> nice >> num_threads >> starttime >>
            starttime // itrealvalue then starttime
            >> vsize >> rss;

        rec.status = std::string(1, state);
        rec.cpu_ticks = utime + stime;
        rec.vms_bytes = vsize;
        rec.rss_bytes = static_cast<uint64_t>(rss) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        rec.num_threads = (int)num_threads;
        return true;
    }

    // comm is cut to 15 characters, so a long watch name can only match the
    // executable's own name: /proc/<pid>/exe, or argv[0] when the link is
    // unreadable (other users' processes without privileges).
    std::string read_exe_name(uint32_t pid)
    {
        const std::string base = "/proc/" + std::to_string(pid);
        char target[4096];
        const ssize_t len = ::readlink((base + "/exe").c_str(), target, sizeof(target));
        if (len > 0)
            return executable_basename(std::string_view(target, static_cast<std::size_t>(len)));

        std::ifstream cmd(base + "/cmdline", std::ios::binary);
        char buf[512];
        cmd.read(buf, sizeof(buf));
        return argv0_basename(std::string_view(buf, static_cast<std::size_t>(cmd.gcount())));
    }

    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        DirHandle dir(::opendir("/proc"));
        if (!dir)
            return out;
        constexpr std::size_t comm_max = 15; // TASK_COMM_LEN - 1
        while (const struct dirent* ent = ::readdir(dir.get()))
        {
            if (!std::isdigit(static_cast<unsigned char>(ent->d_name[0])))
                continue;
            uint32_t pid = static_cast<uint32_t>(std::stoul(ent->d_name));
            ProcRecord rec;
            rec.pid = pid;
            if (!parse_proc_stat(pid, rec))
                continue;
            if (rec.name.size() >= comm_max)
                rec.exe_name = read_exe_name(pid);

            // Get UID from /proc/{pid}/status
            std::ifstream sf("/proc/" + std::to_string(pid) + "/status");
            std::string key;
            while (sf >> key)
            {
                if (key == "Uid:")
                {
                    uid_t uid;
                    sf >> uid;
                    rec.username = uid_to_name(uid);
                    break;
                }
                sf.ignore(256, '\n');
            }
            out.push_back(std::move(rec));
        }
        return out;
    }

// ── Windows ───────────────────────────────────────────────────────────────────
#elif defined(_WIN32)

    constexpr bool kCaseInsensitiveNames = true; // Windows image names are case-insensitive

    // Owns a kernel HANDLE and closes it on every exit path. Null and
    // INVALID_HANDLE_VALUE both mean "no handle".
    class ScopedHandle
    {
    public:
        explicit ScopedHandle(HANDLE handle) noexcept
            : handle_{handle}
        {
        }
        ~ScopedHandle()
        {
            if (valid())
                ::CloseHandle(handle_);
        }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        // Closes the current handle (if any) and takes ownership of `handle`.
        void reset(HANDLE handle) noexcept
        {
            if (valid())
                ::CloseHandle(handle_);
            handle_ = handle;
        }

        bool valid() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }
        HANDLE get() const noexcept
        {
            return handle_;
        }

    private:
        HANDLE handle_;
    };

    double ticks_per_second()
    {
        return 10000000.0; // FILETIME: 100-ns intervals
    }

    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        ScopedHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap.valid())
            return out;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (!::Process32FirstW(snap.get(), &pe))
            return out;

        do
        {
            ProcRecord rec;
            rec.pid = static_cast<uint32_t>(pe.th32ProcessID);
            // Convert wide name
            int sz = ::WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
            if (sz > 1)
            {
                rec.name.resize(sz - 1);
                ::WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, rec.name.data(), sz, nullptr, nullptr);
            }

            // GetProcessMemoryInfo documents PROCESS_QUERY_[LIMITED_]INFORMATION plus
            // PROCESS_VM_READ. Protected and other users' processes refuse VM_READ, so
            // reopen those with limited access only: they stay listed with name and CPU,
            // and their memory is reported as 0.
            constexpr DWORD limited = PROCESS_QUERY_LIMITED_INFORMATION;
            ScopedHandle proc(::OpenProcess(limited | PROCESS_VM_READ, FALSE, pe.th32ProcessID));
            bool memory_readable = proc.valid();
            if (!proc.valid() && open_process_should_retry_limited(::GetLastError()))
                proc.reset(::OpenProcess(limited, FALSE, pe.th32ProcessID));
            if (proc.valid())
            {
                FILETIME ct, et, kt, ut;
                if (::GetProcessTimes(proc.get(), &ct, &et, &kt, &ut))
                {
                    auto to_u64 = [](FILETIME ft) -> uint64_t {
                        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    };
                    rec.cpu_ticks = to_u64(kt) + to_u64(ut); // in 100-ns intervals
                }
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (memory_readable &&
                    ::GetProcessMemoryInfo(proc.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                {
                    rec.rss_bytes = pmc.WorkingSetSize;
                    rec.vms_bytes = pmc.PrivateUsage;
                }
            }

            rec.status = "running";
            rec.num_threads = (int)pe.cntThreads;
            out.push_back(std::move(rec));
        } while (::Process32NextW(snap.get(), &pe));

        return out;
    }

// ── BSD ───────────────────────────────────────────────────────────────────────
#else

    constexpr bool kCaseInsensitiveNames = false;

    double ticks_per_second()
    {
        return 1000000.0; // BSD readers below report microseconds
    }

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

    // Reads the whole process table with sysctl(2). `mib` ends with the element
    // count slot when `counted` (OpenBSD and NetBSD take {.., elem_size, count}).
    // The table can grow between the size query and the read, so retry on ENOMEM.
    template <typename Rec>
    std::vector<Rec> read_kinfo(std::vector<int> mib, bool counted)
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            size_t len = 0;
            if (counted)
                mib.back() = 0;
            if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), nullptr, &len, nullptr, 0) != 0 || len == 0)
                return {};
            len += len / 8 + sizeof(Rec) * 8; // headroom for processes started meanwhile
            if (counted)
                mib.back() = static_cast<int>(len / sizeof(Rec));
            std::vector<Rec> recs(len / sizeof(Rec));
            len = recs.size() * sizeof(Rec);
            if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), recs.data(), &len, nullptr, 0) == 0)
            {
                recs.resize(len / sizeof(Rec));
                return recs;
            }
            if (errno != ENOMEM)
                return {};
        }
        return {};
    }

    uint64_t micros(uint64_t seconds, uint64_t microseconds)
    {
        return seconds * 1000000 + microseconds;
    }

#endif

#if defined(__FreeBSD__)

    // FreeBSD: struct kinfo_proc with ki_* fields, KERN_PROC_PROC lists processes
    // (not threads).
    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        const auto procs = read_kinfo<kinfo_proc>({CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}, false);
        const auto page_size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        for (const auto& kp : procs)
        {
            ProcRecord rec;
            rec.pid = static_cast<uint32_t>(kp.ki_pid);
            rec.name = kp.ki_comm;
            rec.cpu_ticks = micros(kp.ki_rusage.ru_utime.tv_sec, kp.ki_rusage.ru_utime.tv_usec) +
                            micros(kp.ki_rusage.ru_stime.tv_sec, kp.ki_rusage.ru_stime.tv_usec);
            rec.rss_bytes = static_cast<uint64_t>(kp.ki_rssize) * page_size;
            rec.vms_bytes = kp.ki_size;
            rec.num_threads = kp.ki_numthreads;
            // ki_login is the session's login name, not the owner: use the uid.
            rec.username = uid_to_name(kp.ki_uid);

            switch (kp.ki_stat)
            {
            case SSLEEP:
                rec.status = "S";
                break;
            case SRUN:
                rec.status = "R";
                break;
            case SZOMB:
                rec.status = "Z";
                break;
            case SSTOP:
                rec.status = "T";
                break;
            default:
                rec.status = "?";
                break;
            }
            out.push_back(std::move(rec));
        }
        return out;
    }

#elif defined(__OpenBSD__)

    // OpenBSD: struct kinfo_proc has p_* fields; the 6-element MIB carries the
    // element size and count (the interface kvm_getprocs(KERN_PROC_ALL) wraps).
    // Threads are not listed by KERN_PROC_ALL, so num_threads is reported as 1.
    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        const auto procs = read_kinfo<struct kinfo_proc>(
            {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0, static_cast<int>(sizeof(struct kinfo_proc)), 0}, true);
        const auto page_size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        for (const auto& kp : procs)
        {
            ProcRecord rec;
            rec.pid = static_cast<uint32_t>(kp.p_pid);
            rec.name = kp.p_comm;
            rec.cpu_ticks = micros(kp.p_uutime_sec, kp.p_uutime_usec) + micros(kp.p_ustime_sec, kp.p_ustime_usec);
            rec.rss_bytes = static_cast<uint64_t>(kp.p_vm_rssize) * page_size;
            rec.vms_bytes = (static_cast<uint64_t>(kp.p_vm_tsize) + static_cast<uint64_t>(kp.p_vm_dsize) +
                             static_cast<uint64_t>(kp.p_vm_ssize)) *
                            page_size;
            rec.num_threads = 1;
            rec.username = uid_to_name(kp.p_uid);
            // p_stat values from <sys/proc.h>: SIDL 1, SRUN 2, SSLEEP 3, SSTOP 4,
            // SZOMB 5, SDEAD 6, SONPROC 7.
            switch (kp.p_stat)
            {
            case 2:
            case 7:
                rec.status = "R";
                break;
            case 3:
                rec.status = "S";
                break;
            case 4:
                rec.status = "T";
                break;
            case 5:
                rec.status = "Z";
                break;
            default:
                rec.status = "?";
                break;
            }
            out.push_back(std::move(rec));
        }
        return out;
    }

#elif defined(__NetBSD__)

    // NetBSD: KERN_PROC2 returns struct kinfo_proc2 (what kvm_getproc2 wraps), with
    // the element size and count in the MIB.
    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        const auto procs = read_kinfo<struct kinfo_proc2>(
            {CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0, static_cast<int>(sizeof(struct kinfo_proc2)), 0}, true);
        const auto page_size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        for (const auto& kp : procs)
        {
            ProcRecord rec;
            rec.pid = static_cast<uint32_t>(kp.p_pid);
            rec.name = kp.p_comm;
            rec.cpu_ticks = micros(kp.p_uutime_sec, kp.p_uutime_usec) + micros(kp.p_ustime_sec, kp.p_ustime_usec);
            rec.rss_bytes = static_cast<uint64_t>(kp.p_vm_rssize) * page_size;
            rec.vms_bytes = static_cast<uint64_t>(kp.p_vm_vsize) * page_size;
            rec.num_threads = static_cast<int>(kp.p_nlwps);
            rec.username = uid_to_name(kp.p_uid);
            // p_stat values from <sys/proc.h>: SIDL 1, SACTIVE 2, SDYING 3,
            // SSTOP 4, SZOMB 5, SDEAD 6.
            switch (kp.p_stat)
            {
            case 2:
                rec.status = "R";
                break;
            case 4:
                rec.status = "T";
                break;
            case 5:
                rec.status = "Z";
                break;
            default:
                rec.status = "?";
                break;
            }
            out.push_back(std::move(rec));
        }
        return out;
    }

#else

    // DragonFly BSD uses a different kinfo_proc layout (kp_* fields) that this
    // project does not support yet; it reports no processes.
    std::vector<ProcRecord> get_all_procs()
    {
        return {};
    }

#endif

#endif // platform

} // namespace

// ── Common update ─────────────────────────────────────────────────────────────
void ProcessCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto now = std::chrono::steady_clock::now();
    auto procs = get_all_procs();
    const auto discovered_count = procs.size();

    // CPU % uses the previous sample; see cpu_percent_of_one_core for the unit.
    std::unordered_set<uint32_t> alive;
    for (auto& p : procs)
    {
        alive.insert(p.pid);
        auto it = prev_cpu_.find(p.pid);
        if (it != prev_cpu_.end() && p.cpu_ticks >= it->second.cpu_ticks)
        {
            const double elapsed = std::chrono::duration<double>(now - it->second.sampled_at).count();
            p.cpu_percent = cpu_percent_of_one_core(p.cpu_ticks - it->second.cpu_ticks, elapsed, ticks_per_second());
        }
        prev_cpu_[p.pid] = {p.cpu_ticks, now};
    }

    // Prune stale PIDs
    for (auto it = prev_cpu_.begin(); it != prev_cpu_.end();)
        it = alive.count(it->first) != 0 ? std::next(it) : prev_cpu_.erase(it);

    const auto reported = select_reported_processes(std::move(procs), limit_, watches_, kCaseInsensitiveNames);

    metrics.top_processes.clear();
    for (const auto& p : reported)
    {
        ProcessInfo pi;
        pi.pid = p.pid;
        pi.name = p.name;
        pi.status = p.status;
        pi.cpu_percent = p.cpu_percent;
        pi.memory_rss_bytes = p.rss_bytes;
        pi.memory_vms_bytes = p.vms_bytes;
        pi.username = p.username;
        pi.num_threads = p.num_threads;
        metrics.top_processes.push_back(std::move(pi));
    }

    LOGF_TRACE("Process collector updated discovered=%zu reported=%zu limit=%d watches=%zu", discovered_count,
               metrics.top_processes.size(), limit_, watches_.size());
}

} // namespace thewatcher::agent
