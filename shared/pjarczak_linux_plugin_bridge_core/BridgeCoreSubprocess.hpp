#pragma once

#include "BridgeCoreTypes.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace PJarczak::LinuxPluginBridgeCore {

class BridgeSubprocess {
public:
    BridgeSubprocess();
    ~BridgeSubprocess();

    bool start(const LaunchSpec& spec);
    bool running() const;
    void stop();

    nlohmann::json request(const std::string& method, const nlohmann::json& payload);
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    std::string m_last_error;
    std::uint64_t m_next_id{1};
};

}
