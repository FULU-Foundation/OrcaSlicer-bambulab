#include "PJarczakLinuxSoBridgeLauncher.hpp"
#include "PJarczakLinuxBridgeConfig.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Slic3r::PJarczakLinuxBridge {

namespace {

std::filesystem::path module_dir()
{
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&build_default_launch_spec), &module))
        return {};

    std::wstring path(32768, L'\0');
    const DWORD size = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0)
        return {};
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::string trim_ascii(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string normalize_native_text(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());

    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '\r') {
            if (i + 1 < value.size() && value[i + 1] == '\n')
                continue;
            out.push_back('\n');
        } else {
            out.push_back(ch);
        }
    }
    return trim_ascii(out);
}

std::string decode_text_auto(const std::vector<char>& bytes)
{
    if (bytes.empty())
        return {};

    auto from_utf16le = [&](size_t offset) {
        const size_t wchar_count = (bytes.size() - offset) / 2;
        std::wstring ws(wchar_count, L'\0');
        if (wchar_count > 0)
            std::memcpy(ws.data(), bytes.data() + offset, wchar_count * sizeof(wchar_t));
        return normalize_native_text(narrow(ws));
    };

    if (bytes.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xFFu && b1 == 0xFEu)
            return from_utf16le(2);
        if (b0 == 0xFEu && b1 == 0xFFu) {
            std::wstring ws;
            ws.reserve((bytes.size() - 2) / 2);
            for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
                wchar_t ch = static_cast<wchar_t>((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]));
                ws.push_back(ch);
            }
            return normalize_native_text(narrow(ws));
        }
    }

    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEFu &&
        static_cast<unsigned char>(bytes[1]) == 0xBBu &&
        static_cast<unsigned char>(bytes[2]) == 0xBFu)
        return normalize_native_text(std::string(bytes.begin() + 3, bytes.end()));

    for (size_t i = 1; i < std::min<size_t>(bytes.size(), 64); i += 2) {
        if (bytes[i] == '\0')
            return from_utf16le(0);
    }

    return normalize_native_text(std::string(bytes.begin(), bytes.end()));
}

std::wstring quote_windows_arg(const std::wstring& value)
{
    if (value.empty())
        return L"\"\"";

    const bool needs_quotes = value.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes)
        return value;

    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes != 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes != 0)
        out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::string run_and_capture(const std::wstring& exe_path, const std::vector<std::wstring>& args, DWORD* exit_code = nullptr)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        if (exit_code)
            *exit_code = static_cast<DWORD>(-1);
        return {};
    }

    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};

    std::wstring command_line = quote_windows_arg(exe_path);
    for (const std::wstring& arg : args) {
        command_line.push_back(L' ');
        command_line += quote_windows_arg(arg);
    }

    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL ok = ::CreateProcessW(exe_path.c_str(),
                                     mutable_command.data(),
                                     nullptr,
                                     nullptr,
                                     TRUE,
                                     CREATE_NO_WINDOW,
                                     nullptr,
                                     nullptr,
                                     &si,
                                     &pi);

    ::CloseHandle(write_pipe);
    write_pipe = nullptr;

    if (!ok) {
        ::CloseHandle(read_pipe);
        if (exit_code)
            *exit_code = static_cast<DWORD>(-1);
        return {};
    }

    std::vector<char> bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0)
            break;
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read);
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD process_exit = static_cast<DWORD>(-1);
    ::GetExitCodeProcess(pi.hProcess, &process_exit);

    ::CloseHandle(read_pipe);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    if (exit_code)
        *exit_code = process_exit;
    return decode_text_auto(bytes);
}

DWORD run_powershell_wait(const std::filesystem::path& script_path,
                          const std::filesystem::path& package_dir,
                          const std::filesystem::path& plugin_dir,
                          const std::string& distro)
{
    std::wstring command =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script_path.wstring() +
        L"\" -PackageDir \"" + package_dir.wstring() +
        L"\" -PluginDir \"" + plugin_dir.wstring() +
        L"\" -DistroName \"" + widen(distro) + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return static_cast<DWORD>(-1);

    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = static_cast<DWORD>(-1);
    ::GetExitCodeProcess(pi.hProcess, &exit_code);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    return exit_code;
}

std::string to_wsl_path(const std::filesystem::path& p)
{
    const std::wstring ws = p.wstring();
    if (ws.size() >= 2 && ws[1] == L':') {
        std::string tail = narrow(ws.substr(2));
        std::replace(tail.begin(), tail.end(), '\\', '/');
        if (!tail.empty() && tail.front() == '/')
            tail.erase(tail.begin());
        std::string out = "/mnt/";
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ws[0]))));
        out.push_back('/');
        out += tail;
        return out;
    }

    std::string out = narrow(ws);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::string required_env(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? trim_ascii(std::string(value)) : std::string();
}

std::string read_text_file_trimmed(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEFu &&
        static_cast<unsigned char>(value[1]) == 0xBBu &&
        static_cast<unsigned char>(value[2]) == 0xBFu)
        value.erase(0, 3);

    return trim_ascii(value);
}

std::string configured_distro_name(const std::filesystem::path& plugin_dir)
{
    const auto env_value = required_env("PJARCZAK_WSL_DISTRO");
    if (!env_value.empty())
        return env_value;
    return read_text_file_trimmed(plugin_dir / windows_wsl_distro_file_name());
}

std::filesystem::path configured_plugin_cache_dir(const std::filesystem::path& plugin_dir)
{
    const auto env_value = required_env("PJARCZAK_BAMBU_WINDOWS_PLUGIN_CACHE_DIR");
    if (!env_value.empty())
        return std::filesystem::path(env_value);

    const auto appdata = required_env("APPDATA");
    if (appdata.empty())
        return {};

    const auto subdir_file = plugin_dir / windows_plugin_cache_subdir_file_name();
    const auto configured_subdir = read_text_file_trimmed(subdir_file);
    if (!configured_subdir.empty())
        return std::filesystem::path(appdata) / std::filesystem::path(configured_subdir);

    return std::filesystem::path(appdata) / "OrcaSlicer" / "ota" / "plugins";
}

std::string wsl_exe_path()
{
    std::wstring path(32768, L'\0');
    const UINT size = ::GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (size == 0 || size >= path.size())
        return "wsl.exe";
    path.resize(size);
    return narrow((std::filesystem::path(path) / L"wsl.exe").wstring());
}

std::string legacy_windows_wsl_bootstrap_script_file_name()
{
    return "pjarczak-wsl-run-host.sh";
}

std::filesystem::path resolve_bootstrap_script_path(const std::filesystem::path& plugin_dir)
{
    const auto primary = plugin_dir / windows_wsl_bootstrap_script_file_name();
    if (std::filesystem::exists(primary))
        return primary;

    const auto legacy = plugin_dir / legacy_windows_wsl_bootstrap_script_file_name();
    if (std::filesystem::exists(legacy))
        return legacy;

    return {};
}

std::string first_missing_runtime_file(const std::filesystem::path& plugin_dir)
{
    const std::array<std::string, 10> required_files = {{
        host_executable_file_name(),
        std::string("pjarczak_bambu_linux_host_abi1"),
        std::string("pjarczak_bambu_linux_host_abi0"),
        windows_wsl_import_script_file_name(),
        windows_wsl_validate_script_file_name(),
        windows_wsl_distro_file_name(),
        windows_wsl_rootfs_file_name(),
        windows_plugin_cache_subdir_file_name(),
        std::string("ca-certificates.crt"),
        std::string("slicer_base64.cer")
    }};

    for (const std::string& name : required_files) {
        if (!std::filesystem::exists(plugin_dir / std::filesystem::path(name)))
            return name;
    }

    if (resolve_bootstrap_script_path(plugin_dir).empty())
        return windows_wsl_bootstrap_script_file_name();

    return {};
}

bool probe_wsl_ready(const std::string& distro, std::string* reason)
{
    const std::string wsl = wsl_exe_path();
    if (!std::filesystem::exists(std::filesystem::u8path(wsl))) {
        if (reason)
            *reason = "wsl.exe not found in Windows system directory";
        return false;
    }

    const std::wstring wsl_w = widen(wsl);

    DWORD status_code = 0;
    const std::string status_out = run_and_capture(wsl_w, {L"--status"}, &status_code);
    if (status_code != 0) {
        if (reason)
            *reason = trim_ascii(status_out.empty() ? "wsl --status failed" : status_out);
        return false;
    }

    if (distro.empty()) {
        if (reason)
            reason->clear();
        return true;
    }

    DWORD probe_code = 0;
    const std::string probe_out = run_and_capture(wsl_w, {L"-d", widen(distro), L"--user", L"root", L"--", L"sh", L"-lc", L"true"}, &probe_code);
    if (probe_code == 0) {
        if (reason)
            reason->clear();
        return true;
    }

    std::string message = trim_ascii(probe_out);
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (lowered.find("there is no distribution with the supplied name") != std::string::npos ||
        lowered.find("wsl_e_distribution_not_found") != std::string::npos ||
        (lowered.find("distribution") != std::string::npos &&
         lowered.find("not") != std::string::npos &&
         lowered.find("found") != std::string::npos)) {
        if (reason)
            *reason = "WSL distro '" + distro + "' is not installed";
        return false;
    }

    if (reason)
        *reason = message.empty() ? ("Failed to start WSL distro '" + distro + "'") : message;
    return false;
}

bool ensure_runtime_interactive(const std::filesystem::path& plugin_dir, const std::string& distro)
{
    const std::filesystem::path script = plugin_dir / windows_wsl_import_script_file_name();
    if (!std::filesystem::exists(script))
        return false;

    std::string reason;
    if (probe_wsl_ready(distro, &reason))
        return true;

    const std::wstring title = widen("OrcaSlicer Linux Bridge");
    std::wstring message =
        L"WSL2 runtime is not ready for this bridge.\n\n"
        L"Target host app: OrcaSlicer\n"
        L"Distro: " + widen(distro) + L"\n\n"
        L"Reason:\n" + widen(reason.empty() ? std::string("unknown") : reason) + L"\n\n"
        L"Install or repair it now?\n"
        L"This may ask for Administrator approval.";
    const int choice = ::MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_ICONQUESTION | MB_YESNO | MB_SYSTEMMODAL);
    if (choice != IDYES)
        return false;

    const DWORD exit_code = run_powershell_wait(script, plugin_dir, plugin_dir, distro);
    if (exit_code == 0)
        return probe_wsl_ready(distro, nullptr);

    if (exit_code == 1641 || exit_code == 3010) {
        ::MessageBoxW(nullptr,
                      L"WSL installation requested a Windows restart.\n\nRestart Windows and launch OrcaSlicer again.",
                      title.c_str(),
                      MB_ICONWARNING | MB_OK | MB_SYSTEMMODAL);
        return false;
    }

    std::wstring error =
        L"WSL2 runtime setup failed.\n\nExit code: " + std::to_wstring(exit_code) +
        L"\n\nRun install_runtime.ps1 manually from the plugin directory for details.";
    ::MessageBoxW(nullptr, error.c_str(), title.c_str(), MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
    return false;
}

LaunchSpec error_launch_spec(const std::string& message)
{
    LaunchSpec spec;
    spec.description = "windows bridge preflight error";
    spec.argv = {"cmd.exe", "/C", "echo " + message + " 1>&2 && exit /b 127"};
    return spec;
}

}

std::string host_executable_name()
{
    return host_executable_file_name();
}

std::string host_pipe_hint()
{
    return "stdio";
}

std::string launch_preflight_error()
{
    const std::filesystem::path plugin_dir = module_dir();
    if (plugin_dir.empty())
        return "bridge launcher could not resolve plugin directory";

    if (!std::filesystem::exists(std::filesystem::u8path(wsl_exe_path())))
        return "wsl.exe not found in Windows system directory";

    const auto missing_file = first_missing_runtime_file(plugin_dir);
    if (!missing_file.empty())
        return "required Windows WSL runtime file missing: " + missing_file;

    const auto distro = configured_distro_name(plugin_dir);
    if (distro.empty())
        return "PJARCZAK_WSL_DISTRO is not set and pjarczak_wsl_distro.txt is missing or empty";

    const auto plugin_cache_dir = configured_plugin_cache_dir(plugin_dir);
    if (plugin_cache_dir.empty())
        return "Windows plugin cache dir is not configured";

    std::string reason;
    if (!probe_wsl_ready(distro, &reason))
        return reason.empty() ? "WSL2 runtime is not ready" : reason;

    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path plugin_dir = module_dir();
    if (plugin_dir.empty())
        return error_launch_spec("bridge launcher could not resolve plugin directory");

    const auto missing_file = first_missing_runtime_file(plugin_dir);
    if (!missing_file.empty())
        return error_launch_spec("required Windows WSL runtime file missing: " + missing_file);

    const std::string distro = configured_distro_name(plugin_dir);
    if (distro.empty())
        return error_launch_spec("PJARCZAK_WSL_DISTRO is not set and pjarczak_wsl_distro.txt is missing or empty");

    const auto plugin_cache_dir = configured_plugin_cache_dir(plugin_dir);
    if (plugin_cache_dir.empty())
        return error_launch_spec("Windows plugin cache dir is not configured");

    std::string reason;
    if (!probe_wsl_ready(distro, &reason) && !ensure_runtime_interactive(plugin_dir, distro)) {
        std::string retry_reason;
        if (!probe_wsl_ready(distro, &retry_reason))
            return error_launch_spec(retry_reason.empty() ? "WSL2 runtime is not ready" : retry_reason);
    }

    const auto bootstrap_path = resolve_bootstrap_script_path(plugin_dir);
    const std::string plugin_dir_wsl = to_wsl_path(plugin_dir);
    const std::string plugin_cache_wsl = plugin_cache_dir.empty() ? std::string() : to_wsl_path(plugin_cache_dir);
    const std::string bootstrap_wsl = bootstrap_path.empty() ? std::string() : to_wsl_path(bootstrap_path);

    LaunchSpec spec;
    spec.description = "windows via explicit WSL2 distro with linux-local runtime bootstrap";
    spec.argv = {
        wsl_exe_path(),
        "-d", distro,
        "--user", "root",
        "--cd", "/",
        "sh", bootstrap_wsl, plugin_dir_wsl, plugin_cache_wsl
    };
    return spec;
}

}
