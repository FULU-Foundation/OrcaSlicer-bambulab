#pragma once

#include <string>
#include <vector>

namespace Slic3r::PJarczakLinuxBridge {

struct LaunchSpec {
    std::vector<std::string> argv;
    std::string description;
    std::vector<std::pair<std::string, std::string>> env;
};

std::string host_executable_name();
std::string host_pipe_hint();
std::string launch_preflight_error();
LaunchSpec build_default_launch_spec();

}
