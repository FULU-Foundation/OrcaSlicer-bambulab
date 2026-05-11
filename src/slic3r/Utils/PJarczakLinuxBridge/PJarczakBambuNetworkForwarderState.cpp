#include "PJarczakBambuNetworkForwarderState.hpp"
#include "PJarczakLinuxSoBridgeRpcClient.hpp"

#include <functional>
#if defined(_WIN32)
#include <codecvt>
#include <locale>
#endif

namespace Slic3r::PJarczakLinuxBridge {
namespace {
std::mutex g_remote_agents_mutex;
std::map<std::int64_t, BridgeAgent*> g_remote_agents;
std::mutex g_remote_tunnels_mutex;
std::map<std::int64_t, BridgeTunnel*> g_remote_tunnels;

void run_or_queue(BridgeAgent* agent, std::function<void()> fn)
{
    if (!agent)
        return;
    if (agent->queue_on_main)
        agent->queue_on_main(std::move(fn));
    else
        fn();
}

#if defined(_WIN32)
std::wstring utf8_to_wstring(const std::string& s)
{
    try {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        return conv.from_bytes(s);
    } catch (...) {
        return std::wstring(s.begin(), s.end());
    }
}
#endif
}

BridgeAgent* as_agent(void* handle)
{
    return reinterpret_cast<BridgeAgent*>(handle);
}

void* new_agent(const std::string& log_dir)
{
    auto* agent = new BridgeAgent();
    agent->log_dir = log_dir;
    return agent;
}

int delete_agent(void* handle)
{
    auto* agent = as_agent(handle);
    unregister_remote_agent(agent);
    delete agent;
    return 0;
}

BridgeTunnel* as_tunnel_handle(void* handle)
{
    return reinterpret_cast<BridgeTunnel*>(handle);
}

void register_remote_agent(BridgeAgent* agent)
{
    if (!agent || !agent->remote_handle)
        return;
    std::lock_guard<std::mutex> lock(g_remote_agents_mutex);
    g_remote_agents[agent->remote_handle] = agent;
}

void unregister_remote_agent(BridgeAgent* agent)
{
    if (!agent || !agent->remote_handle)
        return;
    std::lock_guard<std::mutex> lock(g_remote_agents_mutex);
    auto it = g_remote_agents.find(agent->remote_handle);
    if (it != g_remote_agents.end() && it->second == agent)
        g_remote_agents.erase(it);
}

BridgeAgent* find_remote_agent(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> lock(g_remote_agents_mutex);
    auto it = g_remote_agents.find(remote_handle);
    return it == g_remote_agents.end() ? nullptr : it->second;
}

void register_remote_tunnel(BridgeTunnel* tunnel)
{
    if (!tunnel || !tunnel->remote_handle)
        return;
    std::lock_guard<std::mutex> lock(g_remote_tunnels_mutex);
    g_remote_tunnels[tunnel->remote_handle] = tunnel;
}

void unregister_remote_tunnel(BridgeTunnel* tunnel)
{
    if (!tunnel || !tunnel->remote_handle)
        return;
    std::lock_guard<std::mutex> lock(g_remote_tunnels_mutex);
    auto it = g_remote_tunnels.find(tunnel->remote_handle);
    if (it != g_remote_tunnels.end() && it->second == tunnel)
        g_remote_tunnels.erase(it);
}

BridgeTunnel* find_remote_tunnel(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> lock(g_remote_tunnels_mutex);
    auto it = g_remote_tunnels.find(remote_handle);
    return it == g_remote_tunnels.end() ? nullptr : it->second;
}

void dispatch_agent_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    BridgeAgent* agent = find_remote_agent(remote_handle);
    if (!agent)
        return;

    if (name == "on_ssdp_msg") {
        if (agent->on_ssdp_msg) {
            const auto msg = payload.value("dev_info_json_str", std::string());
            run_or_queue(agent, [agent, msg] { agent->on_ssdp_msg(msg); });
        }
        return;
    }
    if (name == "on_user_login") {
        const int online_login = payload.value("online_login", 0);
        const bool login = payload.value("login", false);
        agent->logged_in = login;
        if (agent->on_user_login)
            run_or_queue(agent, [agent, online_login, login] { agent->on_user_login(online_login, login); });
        return;
    }
    if (name == "on_printer_connected") {
        const auto topic = payload.value("topic_str", std::string());
        if (agent->on_printer_connected)
            run_or_queue(agent, [agent, topic] { agent->on_printer_connected(topic); });
        return;
    }
    if (name == "on_server_connected") {
        const int return_code = payload.value("return_code", 0);
        const int reason_code = payload.value("reason_code", 0);
        agent->server_connected = return_code == 0;
        if (agent->on_server_connected)
            run_or_queue(agent, [agent, return_code, reason_code] { agent->on_server_connected(return_code, reason_code); });
        return;
    }
    if (name == "on_http_error") {
        const unsigned http_code = payload.value("http_code", 0u);
        const auto body = payload.value("http_body", std::string());
        if (agent->on_http_error)
            run_or_queue(agent, [agent, http_code, body] { agent->on_http_error(http_code, body); });
        return;
    }
    if (name == "on_subscribe_failure") {
        const auto topic = payload.value("topic", std::string());
        if (agent->on_subscribe_failure)
            run_or_queue(agent, [agent, topic] { agent->on_subscribe_failure(topic); });
        return;
    }
    if (name == "callback.get_country_code") {
        std::string value;
        if (agent->get_country_code)
            value = agent->get_country_code();
        else
            value = agent->country_code;
        RpcClient::instance().invoke_void("bridge.callback_reply", {{"request_id", payload.value("request_id", 0LL)}, {"value", value}});
        return;
    }
    if (name == "on_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (agent->on_message)
            run_or_queue(agent, [agent, dev_id, msg] { agent->on_message(dev_id, msg); });
        return;
    }
    if (name == "on_user_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (agent->on_user_message)
            run_or_queue(agent, [agent, dev_id, msg] { agent->on_user_message(dev_id, msg); });
        return;
    }
    if (name == "on_local_connect") {
        const int status = payload.value("status", 0);
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (agent->on_local_connect)
            run_or_queue(agent, [agent, status, dev_id, msg] { agent->on_local_connect(status, dev_id, msg); });
        return;
    }
    if (name == "on_local_message") {
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (agent->on_local_message)
            run_or_queue(agent, [agent, dev_id, msg] { agent->on_local_message(dev_id, msg); });
        return;
    }
    if (name == "on_server_error") {
        const auto url = payload.value("url", std::string());
        const int status = payload.value("status", 0);
        if (agent->on_server_error)
            run_or_queue(agent, [agent, url, status] { agent->on_server_error(url, status); });
        return;
    }
    if (name == "job.update_status") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->on_update_status) {
            const int status = payload.value("status", 0);
            const int code = payload.value("code", 0);
            const auto msg = payload.value("msg", std::string());
            run_or_queue(agent, [job, status, code, msg] { job->on_update_status(status, code, msg); });
        }
        return;
    }
    if (name == "job.progress") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->on_progress) {
            const int progress = payload.value("progress", 0);
            run_or_queue(agent, [job, progress] { job->on_progress(progress); });
        }
        return;
    }
    if (name == "job.check") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        bool reply = true;
        if (job && job->on_check && payload.contains("info") && payload["info"].is_object()) {
            std::map<std::string, std::string> info;
            for (auto it = payload["info"].begin(); it != payload["info"].end(); ++it)
                info[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            reply = job->on_check(info);
        }
        RpcClient::instance().invoke_void("bridge.job_wait_reply", {{"job_id", payload.value("job_id", 0LL)}, {"request_id", payload.value("request_id", 0LL)}, {"reply", reply}});
        return;
    }
    if (name == "job.wait") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        bool reply = true;
        if (job && job->on_wait) {
            const int status = payload.value("status", 0);
            const auto info = payload.value("job_info", std::string());
            reply = job->on_wait(status, info);
        }
        RpcClient::instance().invoke_void("bridge.job_wait_reply", {{"job_id", payload.value("job_id", 0LL)}, {"request_id", payload.value("request_id", 0LL)}, {"reply", reply}});
        return;
    }
    if (name == "job.complete") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->out_string && payload.contains("out"))
            *job->out_string = payload.value("out", std::string());
        return;
    }
}

void dispatch_tunnel_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    BridgeTunnel* tunnel = find_remote_tunnel(remote_handle);
    if (!tunnel)
        return;
    if (name == "logger" && tunnel->logger) {
        const int level = payload.value("level", 0);
        const auto message = payload.value("message", std::string());
        tunnel->logger_message_utf8 = message;
#if defined(_WIN32)
        tunnel->logger_message_wide = utf8_to_wstring(message);
        tunnel->logger(tunnel->logger_ctx, level, tunnel->logger_message_wide.c_str());
#else
        tunnel->logger(tunnel->logger_ctx, level, tunnel->logger_message_utf8.c_str());
#endif
    }
}

std::shared_ptr<BridgeJobState> register_job_state(BridgeAgent* agent, const std::shared_ptr<BridgeJobState>& job)
{
    if (!agent || !job)
        return nullptr;
    std::lock_guard<std::mutex> lock(agent->jobs_mutex);
    agent->jobs[job->job_id] = job;
    return job;
}

std::shared_ptr<BridgeJobState> find_job_state(BridgeAgent* agent, std::int64_t job_id)
{
    if (!agent)
        return nullptr;
    std::lock_guard<std::mutex> lock(agent->jobs_mutex);
    auto it = agent->jobs.find(job_id);
    return it == agent->jobs.end() ? nullptr : it->second;
}

void unregister_job_state(BridgeAgent* agent, std::int64_t job_id)
{
    if (!agent)
        return;
    std::shared_ptr<BridgeJobState> job;
    {
        std::lock_guard<std::mutex> lock(agent->jobs_mutex);
        auto it = agent->jobs.find(job_id);
        if (it != agent->jobs.end()) {
            job = it->second;
            agent->jobs.erase(it);
        }
    }
    if (job) {
        job->stop_cancel_watch = true;
        if (job->cancel_watch.joinable())
            job->cancel_watch.join();
    }
}

}
