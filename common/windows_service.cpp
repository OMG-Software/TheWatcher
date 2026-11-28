#include "windows_service.hpp"

#include "SingleLog.hpp"

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace thewatcher::service
{

namespace
{

#ifdef _WIN32
    SERVICE_STATUS_HANDLE g_status_handle = nullptr;
    SERVICE_STATUS g_status{};
    const char* g_service_name = nullptr;
    const std::function<void()>* g_stop_callback = nullptr;
    const std::function<void()>* g_run_callback = nullptr;
    // Set by service_main when the worker throws, so run_windows_service can
    // report a non-zero exit status.
    bool g_worker_failed = false;

    // Closes an SC_HANDLE (service or Service Control Manager) on scope exit.
    struct ScHandleCloser
    {
        void operator()(SC_HANDLE handle) const noexcept
        {
            CloseServiceHandle(handle);
        }
    };
    using ScHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScHandleCloser>;

    void set_service_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0)
    {
        LOGF_TRACE("Setting Windows service status state=%lu exit_code=%lu wait_hint=%lu",
                   static_cast<unsigned long>(state), static_cast<unsigned long>(exit_code),
                   static_cast<unsigned long>(wait_hint));
        g_status.dwCurrentState = state;
        g_status.dwWin32ExitCode = exit_code;
        g_status.dwWaitHint = wait_hint;
        g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        SetServiceStatus(g_status_handle, &g_status);
    }

    void WINAPI service_control_handler(DWORD control)
    {
        if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        {
            LOG_INFO("Stop requested by Service Control Manager");
            set_service_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            if (g_stop_callback)
                (*g_stop_callback)();
        }
    }

    void WINAPI service_main(DWORD, LPSTR*)
    {
        g_status_handle = RegisterServiceCtrlHandlerA(g_service_name, service_control_handler);
        if (!g_status_handle)
            return;

        g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_status.dwServiceSpecificExitCode = 0;

        set_service_status(SERVICE_START_PENDING, NO_ERROR, 30000);
        set_service_status(SERVICE_RUNNING);

        try
        {
            LOG_INFO("Service started");
            if (g_run_callback)
                (*g_run_callback)();
            LOG_INFO("Service stopped");
            set_service_status(SERVICE_STOPPED);
        }
        catch (const std::exception& e)
        {
            LOGF_CRITICAL("Service failed: %s", e.what());
            g_worker_failed = true;
            set_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        }
        catch (...)
        {
            LOG_CRITICAL("Service failed with an unhandled exception");
            g_worker_failed = true;
            set_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        }
    }

    void throw_last_error(const std::string& operation)
    {
        std::ostringstream out;
        out << operation << " failed with Windows error " << GetLastError();
        throw std::runtime_error(out.str());
    }
#endif

} // namespace

// Quotes one argument so CommandLineToArgvW and the MSVC CRT split it back to
// the original value. Backslashes are literal unless they precede a double
// quote, so a run of backslashes before a quote (or before the closing quote)
// is doubled and the quote itself is escaped.
std::string quote_windows_argument(const std::string& value)
{
    LOG_FUNCTION_TRACE
    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (const char ch : value)
    {
        if (ch == '\\')
        {
            ++backslashes;
            continue;
        }
        if (ch == '"')
            quoted.append(backslashes * 2 + 1, '\\');
        else
            quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted += ch;
    }
    // Trailing backslashes would escape the closing quote, so double them.
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

std::string build_service_command_line(const std::vector<std::string>& args)
{
    LOG_FUNCTION_TRACE
    std::ostringstream command;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            command << ' ';
        command << quote_windows_argument(args[i]);
    }
    return command.str();
}

std::string current_executable_path()
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    std::string buffer(MAX_PATH, '\0');
    DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (length == 0)
        throw_last_error("GetModuleFileNameA");
    buffer.resize(length);
    return buffer;
#else
    return {};
#endif
}

int install_windows_service(const WindowsServiceDefinition& definition)
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    LOGF_INFO("Installing Windows service name=%s", definition.name.c_str());
    ScHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!scm)
        throw_last_error("OpenSCManagerA");

    ScHandle service(CreateServiceA(scm.get(), definition.name.c_str(), definition.display_name.c_str(),
                                    SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                    SERVICE_ERROR_NORMAL, definition.command_line.c_str(), nullptr, nullptr, nullptr,
                                    nullptr, nullptr));
    if (!service)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_EXISTS)
        {
            LOGF_WARNING("Service already exists: %s", definition.name.c_str());
            std::cerr << "Service already exists: " << definition.name << "\n";
            return 1;
        }
        throw std::runtime_error("CreateServiceA failed with Windows error " + std::to_string(error));
    }

    if (!definition.description.empty())
    {
        SERVICE_DESCRIPTIONA desc{};
        desc.lpDescription = const_cast<char*>(definition.description.c_str());
        ChangeServiceConfig2A(service.get(), SERVICE_CONFIG_DESCRIPTION, &desc);
    }

    LOGF_INFO("Installed service: %s", definition.name.c_str());
    std::cout << "Installed service: " << definition.name << "\n";
    std::cout << "Command: " << definition.command_line << "\n";
    return 0;
#else
    static_cast<void>(definition);
    std::cerr << "Windows service installation is only available on Windows.\n";
    return 1;
#endif
}

int uninstall_windows_service(const std::string& name)
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    LOGF_INFO("Uninstalling Windows service name=%s", name.c_str());
    ScHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm)
        throw_last_error("OpenSCManagerA");

    ScHandle service(OpenServiceA(scm.get(), name.c_str(), DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            LOGF_WARNING("Service does not exist: %s", name.c_str());
            std::cerr << "Service does not exist: " << name << "\n";
            return 1;
        }
        throw std::runtime_error("OpenServiceA failed with Windows error " + std::to_string(error));
    }

    SERVICE_STATUS status{};
    ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
    if (!DeleteService(service.get()))
    {
        DWORD error = GetLastError();
        throw std::runtime_error("DeleteService failed with Windows error " + std::to_string(error));
    }

    LOGF_INFO("Uninstalled service: %s", name.c_str());
    std::cout << "Uninstalled service: " << name << "\n";
    return 0;
#else
    static_cast<void>(name);
    std::cerr << "Windows service uninstallation is only available on Windows.\n";
    return 1;
#endif
}

int run_windows_service(const std::string& name, const std::function<void()>& run, const std::function<void()>& stop)
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    LOGF_INFO("Starting Windows service dispatcher name=%s", name.c_str());
    g_stop_callback = &stop;
    g_run_callback = &run;
    g_service_name = name.c_str();
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_worker_failed = false;

    SERVICE_TABLE_ENTRYA table[] = {
        {const_cast<char*>(name.c_str()), service_main},
        {nullptr,                         nullptr     },
    };

    if (!StartServiceCtrlDispatcherA(table))
    {
        throw_last_error("StartServiceCtrlDispatcherA");
    }
    // A worker that threw already reported ERROR_SERVICE_SPECIFIC_ERROR to the
    // SCM; also make the process exit non-zero so recovery actions fire.
    return g_worker_failed ? 1 : 0;
#else
    static_cast<void>(name);
    static_cast<void>(run);
    static_cast<void>(stop);
    std::cerr << "Windows service mode is only available on Windows.\n";
    return 1;
#endif
}

} // namespace thewatcher::service
