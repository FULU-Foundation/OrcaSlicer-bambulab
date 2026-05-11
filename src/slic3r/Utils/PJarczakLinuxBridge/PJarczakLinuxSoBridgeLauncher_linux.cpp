#include "PJarczakLinuxSoBridgeLauncher.hpp"
#include "PJarczakLinuxBridgeConfig.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <string>

namespace Slic3r::PJarczakLinuxBridge {

namespace {

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&build_default_launch_spec), &info) || info.dli_fname == nullptr)
        return {};
    return std::filesystem::path(info.dli_fname).parent_path();
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
    if (!std::filesystem::exists(plugin_dir / host_executable_file_name()))
        return "linux host executable missing in plugin directory";
    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path plugin_dir = module_dir();
    const std::filesystem::path host_path = plugin_dir / host_executable_file_name();

    LaunchSpec spec;
    spec.description = "linux native host";
    spec.argv = {host_path.string()};
    spec.env = {
        {"PJARCZAK_BAMBU_PLUGIN_DIR", plugin_dir.string()},
        {"PJARCZAK_BAMBU_NETWORK_SO", (plugin_dir / linux_network_library_name()).string()},
        {"PJARCZAK_BAMBU_SOURCE_SO", (plugin_dir / linux_source_library_name()).string()}
    };
    return spec;
}

}
