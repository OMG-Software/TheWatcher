#include "memory_collector.hpp"

#include "common/SingleLog.hpp"

#include <cstdint>

#ifdef __linux__
#include <fstream>
#include <string>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <sysinfoapi.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/vmmeter.h>
#include <vm/vm_param.h>
#if defined(__FreeBSD__)
#include <vm/swap_pager.h> // struct xswdev
#endif
#elif defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <uvm/uvmexp.h> // struct uvmexp
#elif defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <uvm/uvm_extern.h> // struct uvmexp_sysctl
#else
#error "Unsupported platform"
#endif

namespace thewatcher::agent
{

double memory_usage_percent(uint64_t total, uint64_t available) noexcept
{
    if (total == 0)
    {
        return 0.0;
    }
    return 100.0 * static_cast<double>(saturating_sub(total, available)) / static_cast<double>(total);
}

namespace
{

    void apply_swap(MemoryMetrics& m, const SwapUsage& swap)
    {
        m.swap_total_bytes = swap.total;
        m.swap_used_bytes = swap.used;
        m.swap_free_bytes = swap.free;
    }

    void log_updated(const MemoryMetrics& m)
    {
        LOGF_TRACE("Memory collector updated total=%llu used=%llu available=%llu usage=%.2f",
                   static_cast<unsigned long long>(m.total_bytes), static_cast<unsigned long long>(m.used_bytes),
                   static_cast<unsigned long long>(m.available_bytes), m.usage_percent);
    }

} // namespace

#ifdef __linux__

void MemoryCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    std::ifstream f("/proc/meminfo");
    if (!f)
    {
        LOG_DEBUG("Memory collector could not open /proc/meminfo");
        return;
    }

    auto& m = metrics.memory;
    std::string key;
    uint64_t value;

    uint64_t mem_total = 0, mem_free = 0, mem_available = 0, buffers = 0, cached = 0, sreclaimable = 0, swap_total = 0,
             swap_free = 0;

    while (f >> key >> value)
    {
        f.ignore(256, '\n'); // skip unit and newline
        value *= 1024;       // kB → bytes
        if (key == "MemTotal:")
            mem_total = value;
        else if (key == "MemFree:")
            mem_free = value;
        else if (key == "MemAvailable:")
            mem_available = value;
        else if (key == "Buffers:")
            buffers = value;
        else if (key == "Cached:")
            cached = value;
        else if (key == "SReclaimable:")
            sreclaimable = value;
        else if (key == "SwapTotal:")
            swap_total = value;
        else if (key == "SwapFree:")
            swap_free = value;
    }

    m.total_bytes = mem_total;
    m.free_bytes = mem_free;
    m.available_bytes = mem_available;
    m.buffers_bytes = buffers;
    m.cached_bytes = cached + sreclaimable;
    // Containers and odd kernels can report a cache larger than the remainder: never wrap.
    m.used_bytes = saturating_sub(saturating_sub(saturating_sub(saturating_sub(mem_total, mem_free), buffers), cached),
                                  sreclaimable);
    m.usage_percent = memory_usage_percent(mem_total, mem_available);
    apply_swap(m, swap_usage_from_free(swap_total, swap_free));
    log_updated(m);
}

#elif defined(_WIN32)

namespace
{

    struct PageFileTotals
    {
        uint64_t total_pages = 0;
        uint64_t in_use_pages = 0;
    };

    // EnumPageFilesW callback: sums every page file. Sizes are in pages.
    BOOL CALLBACK add_page_file(LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR /*filename*/)
    {
        auto& totals = *static_cast<PageFileTotals*>(context);
        totals.total_pages += info->TotalSize;
        totals.in_use_pages += info->TotalInUse;
        return TRUE;
    }

    // Real page file size and use. ullTotalPageFile/ullAvailPageFile are the commit limit and the
    // commit headroom, not the page file, so they cannot be used for swap.
    SwapUsage read_page_file_usage()
    {
        PageFileTotals totals;
        if (!::EnumPageFilesW(&add_page_file, &totals))
        {
            return {};
        }
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        const uint64_t page_size = info.dwPageSize;
        return swap_usage_from_used(totals.total_pages * page_size, totals.in_use_pages * page_size);
    }

} // namespace

void MemoryCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    MEMORYSTATUSEX stat{};
    stat.dwLength = sizeof(stat);
    if (!::GlobalMemoryStatusEx(&stat))
    {
        LOG_DEBUG("Memory collector GlobalMemoryStatusEx failed");
        return;
    }

    auto& m = metrics.memory;
    m.total_bytes = stat.ullTotalPhys;
    m.free_bytes = stat.ullAvailPhys;
    m.available_bytes = stat.ullAvailPhys;
    m.used_bytes = saturating_sub(stat.ullTotalPhys, stat.ullAvailPhys);
    m.usage_percent = static_cast<double>(stat.dwMemoryLoad); // already 0-100
    apply_swap(m, read_page_file_usage());
    log_updated(m);
}

#elif defined(__FreeBSD__) || defined(__DragonFly__)

namespace
{

#if defined(__FreeBSD__)

    // Sums vm.swap_info.<n> (one struct xswdev per swap device, sizes in pages). The kernel returns
    // ENOENT once n runs past the last device. No devices means no swap: 0/0/0.
    SwapUsage read_swap_usage(uint64_t page_size)
    {
        int mib[16];
        size_t mib_len = sizeof(mib) / sizeof(mib[0]);
        if (::sysctlnametomib("vm.swap_info", mib, &mib_len) != 0)
        {
            return {};
        }

        uint64_t total_pages = 0;
        uint64_t used_pages = 0;
        for (int device = 0;; ++device)
        {
            mib[mib_len] = device;
            xswdev entry{};
            size_t len = sizeof(entry);
            if (::sysctl(mib, static_cast<u_int>(mib_len + 1), &entry, &len, nullptr, 0) != 0 || len != sizeof(entry) ||
                entry.xsw_version != XSWDEV_VERSION)
            {
                break;
            }
            total_pages += static_cast<uint64_t>(entry.xsw_nblks);
            used_pages += static_cast<uint64_t>(entry.xsw_used);
        }
        return swap_usage_from_used(total_pages * page_size, used_pages * page_size);
    }

#else

    // DragonFly has no vm.swap_info; swap is not collected there yet.
    SwapUsage read_swap_usage(uint64_t)
    {
        return {};
    }

#endif

} // namespace

void MemoryCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto& m = metrics.memory;

    uint64_t hw_physmem = 0;
    size_t len = sizeof(hw_physmem);
    ::sysctlbyname("hw.physmem", &hw_physmem, &len, nullptr, 0);
    m.total_bytes = hw_physmem;

    uint32_t pagesize = 4096;
    len = sizeof(pagesize);
    ::sysctlbyname("vm.stats.vm.v_page_size", &pagesize, &len, nullptr, 0);

    uint32_t active = 0, inactive = 0, free_cnt = 0, cache_cnt = 0, wire_cnt = 0;
    len = sizeof(uint32_t);
    ::sysctlbyname("vm.stats.vm.v_active_count", &active, &len, nullptr, 0);
    ::sysctlbyname("vm.stats.vm.v_inactive_count", &inactive, &len, nullptr, 0);
    ::sysctlbyname("vm.stats.vm.v_free_count", &free_cnt, &len, nullptr, 0);
    ::sysctlbyname("vm.stats.vm.v_cache_count", &cache_cnt, &len, nullptr, 0);
    ::sysctlbyname("vm.stats.vm.v_wire_count", &wire_cnt, &len, nullptr, 0);

    m.free_bytes = static_cast<uint64_t>(free_cnt) * pagesize;
    m.available_bytes = static_cast<uint64_t>(free_cnt + inactive + cache_cnt) * pagesize;
    m.cached_bytes = static_cast<uint64_t>(cache_cnt) * pagesize;
    m.used_bytes = static_cast<uint64_t>(active + wire_cnt) * pagesize;
    m.usage_percent = m.total_bytes > 0 ? 100.0 * static_cast<double>(m.used_bytes) / m.total_bytes : 0.0;

    apply_swap(m, read_swap_usage(pagesize));
    log_updated(m);
}

#elif defined(__OpenBSD__)

namespace
{

    template <typename T>
    bool read_sysctl(int first, int second, T& out)
    {
        const int mib[2] = {first, second};
        size_t len = sizeof(out);
        return ::sysctl(mib, 2, &out, &len, nullptr, 0) == 0 && len == sizeof(out);
    }

} // namespace

void MemoryCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto& m = metrics.memory;

    int64_t physmem = 0;
    uvmexp uvm{};
    if (!read_sysctl(CTL_HW, HW_PHYSMEM64, physmem) || !read_sysctl(CTL_VM, VM_UVMEXP, uvm))
    {
        LOG_DEBUG("Memory collector could not read hw.physmem64 or vm.uvmexp");
        return;
    }

    // struct uvmexp counts are in pages of `pagesize` bytes.
    const auto pages = [&uvm](int count) {
        return static_cast<uint64_t>(count > 0 ? count : 0) * uvm.pagesize;
    };

    m.total_bytes = physmem > 0 ? static_cast<uint64_t>(physmem) : 0;
    m.free_bytes = pages(uvm.free);
    m.available_bytes = pages(uvm.free) + pages(uvm.inactive);
    m.cached_bytes = pages(uvm.vnodepages) + pages(uvm.vtextpages);
    m.used_bytes = pages(uvm.active) + pages(uvm.wired);
    m.usage_percent = memory_usage_percent(m.total_bytes, m.available_bytes);
    apply_swap(m, swap_usage_from_used(pages(uvm.swpages), pages(uvm.swpginuse)));
    log_updated(m);
}

#elif defined(__NetBSD__)

void MemoryCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto& m = metrics.memory;

    uint64_t physmem = 0;
    size_t len = sizeof(physmem);
    const int hw_mib[2] = {CTL_HW, HW_PHYSMEM64};
    uvmexp_sysctl uvm{};
    size_t uvm_len = sizeof(uvm);
    const int vm_mib[2] = {CTL_VM, VM_UVMEXP2};
    if (::sysctl(hw_mib, 2, &physmem, &len, nullptr, 0) != 0 || ::sysctl(vm_mib, 2, &uvm, &uvm_len, nullptr, 0) != 0)
    {
        LOG_DEBUG("Memory collector could not read hw.physmem64 or vm.uvmexp2");
        return;
    }

    // struct uvmexp_sysctl counts are in pages of `pagesize` bytes.
    const auto pages = [&uvm](int64_t count) {
        return static_cast<uint64_t>(count > 0 ? count : 0) * uvm.pagesize;
    };

    m.total_bytes = physmem;
    m.free_bytes = pages(uvm.free);
    m.available_bytes = pages(uvm.free) + pages(uvm.inactive);
    m.cached_bytes = pages(uvm.filepages) + pages(uvm.execpages);
    m.used_bytes = pages(uvm.active) + pages(uvm.wired);
    m.usage_percent = memory_usage_percent(m.total_bytes, m.available_bytes);
    apply_swap(m, swap_usage_from_used(pages(uvm.swpages), pages(uvm.swpginuse)));
    log_updated(m);
}

#endif

} // namespace thewatcher::agent
