#include "BridgeCoreFrame.hpp"

namespace PJarczak::LinuxPluginBridgeCore {

static std::string encode_common(std::uint64_t id, const std::string& method, const nlohmann::json& payload, bool is_event)
{
    nlohmann::json root;
    root["id"] = id;
    root["method"] = method;
    root["payload"] = payload;
    root["kind"] = is_event ? "event" : "rpc";
    return root.dump() + "\n";
}

static bool decode_common(const std::string& line, std::uint64_t& id, std::string& method, nlohmann::json& payload, std::string& error)
{
    try {
        const auto root = nlohmann::json::parse(line);
        if (!root.is_object()) {
            error = "frame root is not object";
            return false;
        }
        id = root.value("id", 0ULL);
        method = root.value("method", std::string());
        payload = root.value("payload", nlohmann::json::object());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string encode_rpc_frame(const RpcFrame& frame)
{
    return encode_common(frame.id, frame.method, frame.payload, false);
}

bool decode_rpc_frame(const std::string& line, RpcFrame& frame, std::string& error)
{
    return decode_common(line, frame.id, frame.method, frame.payload, error);
}

std::string encode_bridge_event(const BridgeEvent& event)
{
    nlohmann::json payload = event.payload;
    payload["agent_handle"] = event.agent_handle;
    payload["event_kind"] = static_cast<int>(event.kind);
    return encode_common(event.id, event.name, payload, true);
}

bool decode_bridge_event(const std::string& line, BridgeEvent& event, std::string& error)
{
    if (!decode_common(line, event.id, event.name, event.payload, error))
        return false;
    event.agent_handle = event.payload.value("agent_handle", 0LL);
    event.kind = static_cast<EventKind>(event.payload.value("event_kind", 0));
    return true;
}

}
