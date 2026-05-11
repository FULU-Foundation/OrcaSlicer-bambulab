#include "PJarczakLinuxSoBridgeEventPump.hpp"
#include "PJarczakLinuxSoBridgeRpcClient.hpp"
#include "PJarczakBambuNetworkForwarderState.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace Slic3r::PJarczakLinuxBridge {
namespace {
std::mutex g_pump_mutex;
std::thread g_pump_thread;
std::atomic<bool> g_stop{false};

bool has_events(const nlohmann::json& j)
{
    return j.value("ok", false) && j.contains("events") && j["events"].is_array() && !j["events"].empty();
}
}

EventPump& EventPump::instance()
{
    static EventPump pump;
    return pump;
}

EventPump::~EventPump()
{
    stop();
}

void EventPump::ensure_started()
{
    std::lock_guard<std::mutex> lock(g_pump_mutex);
    if (m_running)
        return;
    g_stop = false;
    m_running = true;
    g_pump_thread = std::thread([this] { run(); });
}

void EventPump::stop()
{
    std::lock_guard<std::mutex> lock(g_pump_mutex);
    if (!m_running)
        return;
    g_stop = true;
    if (g_pump_thread.joinable())
        g_pump_thread.join();
    m_running = false;
}

void EventPump::run()
{
    using namespace std::chrono_literals;
    while (!g_stop.load()) {
        auto& rpc = RpcClient::instance();
        if (!rpc.is_started()) {
            std::this_thread::sleep_for(250ms);
            continue;
        }

        const auto j = rpc.invoke_json("bridge.poll_events", {{"limit", 64}});
        if (!has_events(j)) {
            std::this_thread::sleep_for(80ms);
            continue;
        }

        for (const auto& ev : j["events"]) {
            const auto payload = ev.contains("payload") ? ev["payload"] : nlohmann::json::object();
            if (ev.contains("agent"))
                dispatch_agent_event(ev.value("agent", 0LL), ev.value("name", std::string()), payload);
            else if (ev.contains("tunnel"))
                dispatch_tunnel_event(ev.value("tunnel", 0LL), ev.value("name", std::string()), payload);
        }
    }
}

}
