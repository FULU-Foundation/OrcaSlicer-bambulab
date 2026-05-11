#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace PJarczak::LinuxPluginBridgeCore {

enum class TransportKind : std::uint8_t {
    StdioJsonLine = 0,
    NamedPipeJsonLine = 1,
    UnixSocketJsonLine = 2
};

enum class EventKind : std::uint16_t {
    Unknown = 0,
    UserLogin = 1,
    PrinterConnected = 2,
    ServerConnected = 3,
    HttpError = 4,
    SubscribeFailure = 5,
    Message = 6,
    UserMessage = 7,
    LocalConnect = 8,
    LocalMessage = 9,
    ServerError = 10,
    JobProgress = 11,
    JobWait = 12,
    Log = 13
};

struct LaunchSpec {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    TransportKind transport{TransportKind::StdioJsonLine};
};

struct RpcFrame {
    std::uint64_t id{0};
    std::string method;
    nlohmann::json payload;
};

struct BridgeEvent {
    std::uint64_t id{0};
    std::int64_t agent_handle{0};
    EventKind kind{EventKind::Unknown};
    std::string name;
    nlohmann::json payload;
};

struct MethodManifestEntry {
    std::string symbol;
    std::string exported_name;
    std::string area;
    std::string status;
    std::string stability;
    std::string notes;
};

}
