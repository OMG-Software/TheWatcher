#include "process_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __linux__
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <pwd.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h> // getpagesize()
#else
#error "Unsupported platform"
#endif

namespace thewatcher::agent
{

// ── Internal record shared by platforms ───────────────────────────────────────
struct ProcRecord
{
    uint32_t pid = 0;
    std::string name;
    std::string status;
    uint64_t cpu_ticks = 0; // user + system ticks (platform-specific unit)
    double cpu_percent = 0.0;
    uint64_t rss_bytes = 0;
    uint64_t vms_bytes = 0;
    std::string username;
    int num_threads = 0;
};

// ── Linux ─────────────────────────────────────────────────────────────────────
#ifdef __linux__

namespace
{

    std::string uid_to_name(uid_t uid)
    {
        char buf[1024];
        struct passwd pw{}, *result = nullptr;
        ::getpwuid_r(uid, &pw, buf, sizeof(buf), &result);
        return result ? result->pw_name : std::to_string(uid);
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

    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        DIR* dir = ::opendir("/proc");
        if (!dir)
            return out;
        struct dirent* ent;
        while ((ent = ::readdir(dir)))
        {
            if (!std::isdigit(static_cast<unsigned char>(ent->d_name[0])))
                continue;
            uint32_t pid = static_cast<uint32_t>(std::stoul(ent->d_name));
            ProcRecord rec;
            rec.pid = pid;
            if (!parse_proc_stat(pid, rec))
                continue;

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
        ::closedir(dir);
        return out;
    }

} // namespace

// ── Windows ───────────────────────────────────────────────────────────────────
#elif defined(_WIN32)

namespace
{

    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return out;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (!::Process32FirstW(snap, &pe))
        {
            ::CloseHandle(snap);
            return out;
        }

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

            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (hProc)
            {
                FILETIME ct, et, kt, ut;
                if (::GetProcessTimes(hProc, &ct, &et, &kt, &ut))
                {
                    auto to_u64 = [](FILETIME ft) -> uint64_t {
                        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    };
                    rec.cpu_ticks = to_u64(kt) + to_u64(ut); // in 100-ns intervals
                }
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (::GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                {
                    rec.rss_bytes = pmc.WorkingSetSize;
                    rec.vms_bytes = pmc.PrivateUsage;
                }
                ::CloseHandle(hProc);
            }

            rec.status = "running";
            rec.num_threads = (int)pe.cntThreads;
            out.push_back(std::move(rec));
        } while (::Process32NextW(snap, &pe));

        ::CloseHandle(snap);
        return out;
    }

} // namespace

// ── BSD ───────────────────────────────────────────────────────────────────────
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)

namespace
{

    std::vector<ProcRecord> get_all_procs()
    {
        std::vector<ProcRecord> out;
        int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
        size_t len = 0;
        ::sysctl(mib, 4, nullptr, &len, nullptr, 0);
        if (len == 0)
            return out;

        std::vector<kinfo_proc> procs(len / sizeof(kinfo_proc) + 16);
        len = procs.size() * sizeof(kinfo_proc);
        if (::sysctl(mib, 4, procs.data(), &len, nullptr, 0) != 0)
            return out;

        std::size_t count = len / sizeof(kinfo_proc);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& kp = procs[i];
            ProcRecord rec;
            rec.pid = static_cast<uint32_t>(kp.ki_pid);
            rec.name = kp.ki_comm;
            rec.cpu_ticks =
                (uint64_t)(kp.ki_rusage.ru_utime.tv_sec * 1000000 + kp.ki_rusage.ru_utime.tv_usec +
                           kp.ki_rusage.ru_stime.tv_sec * 1000000 + kp.ki_rusage.ru_stime.tv_usec); // microseconds
            rec.rss_bytes = static_cast<uint64_t>(kp.ki_rssize) * static_cast<uint64_t>(getpagesize());
            rec.vms_bytes = kp.ki_size;
            rec.num_threads = kp.ki_numthreads;
            rec.username = kp.ki_login[0] ? kp.ki_login : std::to_string(kp.ki_uid);

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

} // namespace

#endif // platform

// ── Common update ─────────────────────────────────────────────────────────────
void ProcessCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto now = std::chrono::steady_clock::now();
    auto procs = get_all_procs();
    const auto discovered_count = procs.size();

    // Compute CPU % using stored previous sample
    for (auto& p : procs)
    {
        auto it = prev_cpu_.find(p.pid);
        if (it != prev_cpu_.end())
        {
            double elapsed = std::chrono::duration<double>(now - it->second.sampled_at).count();
            if (elapsed > 0 && p.cpu_ticks >= it->second.cpu_ticks)
            {
                uint64_t delta = p.cpu_ticks - it->second.cpu_ticks;
#ifdef __linux__
                long tck = ::sysconf(_SC_CLK_TCK);
                p.cpu_percent = 100.0 * static_cast<double>(delta) / (elapsed * tck);
#elif defined(_WIN32)
                // delta in 100-ns; elapsed in seconds; per-cpu normalised
                SYSTEM_INFO si{};
                ::GetSystemInfo(&si);
                p.cpu_percent = 100.0 * static_cast<double>(delta) / (elapsed * 10000000.0 * si.dwNumberOfProcessors);
#else
                // delta in microseconds
                p.cpu_percent = 100.0 * static_cast<double>(delta) / (elapsed * 1000000.0);
#endif
                p.cpu_percent = std::clamp(p.cpu_percent, 0.0, 100.0);
            }
        }
        prev_cpu_[p.pid] = {p.cpu_ticks, now};
    }

    const auto all_procs = procs;

    // Sort by CPU% desc, keep top N, then add watched process matches that
    // would otherwise be omitted from the top process list.
    std::sort(procs.begin(), procs.end(), [](const ProcRecord& a, const ProcRecord& b) {
        return a.cpu_percent > b.cpu_percent;
    });
    if (static_cast<int>(procs.size()) > limit_)
        procs.resize(static_cast<std::size_t>(limit_));

    for (const auto& watch : watches_)
    {
        if (!watch.enabled || watch.name.empty())
            continue;
        for (const auto& proc : all_procs)
        {
            if (proc.name != watch.name)
                continue;
            const auto already_reported = std::any_of(procs.begin(), procs.end(), [&](const ProcRecord& existing) {
                return existing.pid == proc.pid;
            });
            if (!already_reported)
                procs.push_back(proc);
        }
    }

    metrics.top_processes.clear();
    for (const auto& p : procs)
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

    // Prune stale PIDs
    for (auto it = prev_cpu_.begin(); it != prev_cpu_.end();)
    {
        bool alive = std::any_of(all_procs.begin(), all_procs.end(), [&](const ProcRecord& p) {
            return p.pid == it->first;
        });
        it = alive ? std::next(it) : prev_cpu_.erase(it);
    }
    LOGF_TRACE("Process collector updated discovered=%zu reported=%zu limit=%d watches=%zu", discovered_count,
               metrics.top_processes.size(), limit_, watches_.size());
}

} // namespace thewatcher::agent
