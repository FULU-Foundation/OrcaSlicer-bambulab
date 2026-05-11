#include "PJarczakBambuNetworkForwarderState.hpp"
#include "PJarczakLinuxBridgeCompat.hpp"
#include "PJarczakLinuxBridgeConfig.hpp"
#include "PJarczakLinuxSoBridgeEventPump.hpp"
#include "PJarczakLinuxSoBridgeRpcClient.hpp"
#include "../../../../shared/pjarczak_linux_plugin_bridge_core/BridgeCoreJson.hpp"

#include "../../GUI/Printer/BambuTunnel.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

#if defined(_WIN32)
#define PJBRIDGE_EXPORT extern "C" __declspec(dllexport)
#else
#define PJBRIDGE_EXPORT extern "C"
#endif

namespace Slic3r::PJarczakLinuxBridge {

static std::string g_last_error;
static std::string bridge_reported_version()
{
    const auto j = RpcClient::instance().invoke_json("bridge.handshake", nlohmann::json::object());
    if (!j.value("ok", false)) {
        if (j.contains("error"))
            g_last_error = j["error"].get<std::string>();
        return {};
    }
    if (!j.value("network_loaded", false) || !j.value("source_loaded", false)) {
        g_last_error = "bridge host failed to load linux payload: network=" +
            j.value("network_status", std::string("unknown")) +
            ", source=" + j.value("source_status", std::string("unknown"));
        return {};
    }
    const auto actual = j.value("network_actual_abi_version", std::string());
    if (!actual.empty())
        return actual;
    const auto reported = j.value("network_abi_version", std::string());
    if (!reported.empty())
        return reported;
    return expected_network_abi_version();
}

static int invalid_handle()
{
    g_last_error = "invalid handle";
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

static BridgeAgent* require_agent(void* handle)
{
    return as_agent(handle);
}

static BridgeTunnel* require_tunnel(Bambu_Tunnel tunnel)
{
    return as_tunnel_handle(tunnel);
}

static nlohmann::json ok_or_error(const nlohmann::json& j)
{
    if (!j.value("ok", false) && j.contains("error"))
        g_last_error = j["error"].get<std::string>();
    return j;
}

static std::int64_t agent_id(BridgeAgent* a)
{
    return a ? a->remote_handle : 0;
}

static void fill_user_cache(BridgeAgent* a, const nlohmann::json& j)
{
    if (!a)
        return;
    if (j.contains("user_id")) a->user_id = j["user_id"].get<std::string>();
    if (j.contains("user_name")) a->user_name = j["user_name"].get<std::string>();
    if (j.contains("user_avatar")) a->user_avatar = j["user_avatar"].get<std::string>();
    if (j.contains("user_nickname")) a->user_nickname = j["user_nickname"].get<std::string>();
    if (j.contains("login_cmd")) a->login_cmd = j["login_cmd"].get<std::string>();
    if (j.contains("logout_cmd")) a->logout_cmd = j["logout_cmd"].get<std::string>();
    if (j.contains("login_info")) a->login_info = j["login_info"].get<std::string>();
    if (j.contains("bambulab_host")) a->bambulab_host = j["bambulab_host"].get<std::string>();
}

static void ensure_event_pump()
{
    EventPump::instance().ensure_started();
}

static int register_remote_callback(const char* method, BridgeAgent* a)
{
    if (!a)
        return invalid_handle();
    ensure_event_pump();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}}));
    return j.value("value", j.value("ret", 0));
}

static std::atomic<std::int64_t> g_next_job_id{1};

static std::int64_t next_job_id()
{
    return g_next_job_id.fetch_add(1);
}

static void start_cancel_watch(const std::shared_ptr<BridgeJobState>& job)
{
    if (!job || !job->was_cancelled)
        return;
    job->cancel_watch = std::thread([job]() {
        using namespace std::chrono_literals;
        while (!job->stop_cancel_watch.load()) {
            bool cancel_now = false;
            try {
                cancel_now = job->was_cancelled();
            } catch (...) {
                cancel_now = false;
            }
            if (cancel_now) {
                RpcClient::instance().invoke_void("bridge.job_cancel", {{"job_id", job->job_id}, {"cancel", true}});
                break;
            }
            std::this_thread::sleep_for(120ms);
        }
    });
}

static int invoke_job_update_only(const char* method, const char* kind, BridgeAgent* a, const nlohmann::json& payload, BBL::OnUpdateStatusFn update)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_update_status = std::move(update);
    register_job_state(a, job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static int invoke_job_with_wait(const char* method, const char* kind, BridgeAgent* a, const nlohmann::json& params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel, BBL::OnWaitFn wait, std::string* out)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_update_status = std::move(update);
    job->was_cancelled = std::move(cancel);
    job->on_wait = std::move(wait);
    job->out_string = out;
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", params}}));
    if (out && j.contains("out"))
        *out = j.value("out", std::string());
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static std::map<std::string, std::string> clone_string_map(const std::map<std::string, std::string>* in)
{
    return in ? *in : std::map<std::string, std::string>();
}

static int invoke_string_callback(const char* method, BridgeAgent* a, const nlohmann::json& payload, const std::function<void(std::string)>& fn)
{
    if (!a)
        return invalid_handle();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, payload));
    const int ret = j.value("value", j.value("ret", 0));
    if (ret == 0 && fn && j.contains("result"))
        fn(j.value("result", std::string()));
    return ret;
}

static int invoke_string_int_callback(const char* method, BridgeAgent* a, const nlohmann::json& payload, const std::function<void(std::string, int)>& fn)
{
    if (!a)
        return invalid_handle();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, payload));
    const int ret = j.value("value", j.value("ret", 0));
    if (ret == 0 && fn)
        fn(j.value("result", std::string()), j.value("status", 0));
    return ret;
}

static nlohmann::json nested_string_map_to_json(const std::map<std::string, std::map<std::string, std::string>>& value)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, inner] : value)
        out[k] = inner;
    return out;
}

static void json_to_nested_string_map(const nlohmann::json& j, std::map<std::string, std::map<std::string, std::string>>& out)
{
    out.clear();
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        std::map<std::string, std::string> inner;
        if (it.value().is_object()) {
            for (auto it2 = it.value().begin(); it2 != it.value().end(); ++it2)
                inner[it2.key()] = it2.value().is_string() ? it2.value().get<std::string>() : it2.value().dump();
        }
        out[it.key()] = std::move(inner);
    }
}

static int invoke_progress_job(const char* method, const char* kind, BridgeAgent* a, const nlohmann::json& payload, BBL::ProgressFn progress, BBL::WasCancelledFn cancel)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_progress = std::move(progress);
    job->was_cancelled = std::move(cancel);
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static int invoke_progress_check_job(const char* method, const char* kind, BridgeAgent* a, const nlohmann::json& payload, BBL::CheckFn check, BBL::ProgressFn progress, BBL::WasCancelledFn cancel)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<BridgeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_check = std::move(check);
    job->on_progress = std::move(progress);
    job->was_cancelled = std::move(cancel);
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

}

using namespace Slic3r::PJarczakLinuxBridge;
using namespace BBL;
using Slic3r::BBLModelTask;
using Slic3r::OnGetSubTaskFn;

PJBRIDGE_EXPORT bool bambu_network_check_debug_consistent(bool) { return true; }
PJBRIDGE_EXPORT std::string bambu_network_get_version() { return bridge_reported_version(); }

PJBRIDGE_EXPORT void* bambu_network_create_agent(std::string log_dir)
{
    auto* a = as_agent(new_agent(log_dir));
    auto& rpc = RpcClient::instance();
    const auto j = ok_or_error(rpc.invoke_json("net.create_agent", {{"log_dir", log_dir}}));
    if (!j.value("ok", false)) {
        delete_agent(a);
        return nullptr;
    }
    a->remote_handle = j.value("value", 0LL);
    if (a->remote_handle == 0) {
        g_last_error = "bridge host returned invalid remote agent handle";
        delete_agent(a);
        return nullptr;
    }
    register_remote_agent(a);
    ensure_event_pump();
    return a;
}

PJBRIDGE_EXPORT int bambu_network_destroy_agent(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    unregister_remote_agent(a);
    if (a->remote_handle)
        ok_or_error(RpcClient::instance().invoke_json("net.destroy_agent", {{"agent", a->remote_handle}}));
    return delete_agent(agent);
}

PJBRIDGE_EXPORT int bambu_network_init_log(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    return RpcClient::instance().invoke_int("net.init_log", {{"agent", agent_id(a)}});
}

PJBRIDGE_EXPORT int bambu_network_set_config_dir(void* agent, std::string config_dir)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    a->config_dir = std::move(config_dir);
    return RpcClient::instance().invoke_int("net.set_config_dir", {{"agent", agent_id(a)}, {"config_dir", a->config_dir}});
}

PJBRIDGE_EXPORT int bambu_network_set_cert_file(void* agent, std::string folder, std::string filename)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    a->cert_dir = std::move(folder);
    a->cert_file = std::move(filename);
    return RpcClient::instance().invoke_int("net.set_cert_file", {{"agent", agent_id(a)}, {"folder", a->cert_dir}, {"filename", a->cert_file}});
}

PJBRIDGE_EXPORT int bambu_network_set_country_code(void* agent, std::string country_code)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    a->country_code = std::move(country_code);
    return RpcClient::instance().invoke_int("net.set_country_code", {{"agent", agent_id(a)}, {"country_code", a->country_code}});
}

PJBRIDGE_EXPORT int bambu_network_start(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    a->started = true;
    return RpcClient::instance().invoke_int("net.start", {{"agent", agent_id(a)}});
}

PJBRIDGE_EXPORT int bambu_network_set_on_ssdp_msg_fn(void* agent, OnMsgArrivedFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_ssdp_msg = std::move(fn); return register_remote_callback("net.set_on_ssdp_msg_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_user_login_fn(void* agent, OnUserLoginFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_user_login = std::move(fn); return register_remote_callback("net.set_on_user_login_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_printer_connected_fn(void* agent, OnPrinterConnectedFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_printer_connected = std::move(fn); return register_remote_callback("net.set_on_printer_connected_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_server_connected_fn(void* agent, OnServerConnectedFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_server_connected = std::move(fn); return register_remote_callback("net.set_on_server_connected_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_http_error_fn(void* agent, OnHttpErrorFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_http_error = std::move(fn); return register_remote_callback("net.set_on_http_error_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_get_country_code_fn(void* agent, GetCountryCodeFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->get_country_code = std::move(fn); return register_remote_callback("net.set_get_country_code_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_subscribe_failure_fn(void* agent, GetSubscribeFailureFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_subscribe_failure = std::move(fn); return register_remote_callback("net.set_on_subscribe_failure_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_message_fn(void* agent, OnMessageFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_message = std::move(fn); return register_remote_callback("net.set_on_message_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_user_message_fn(void* agent, OnMessageFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_user_message = std::move(fn); return register_remote_callback("net.set_on_user_message_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_local_connect_fn(void* agent, OnLocalConnectedFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_local_connect = std::move(fn); return register_remote_callback("net.set_on_local_connect_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_on_local_message_fn(void* agent, OnMessageFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_local_message = std::move(fn); return register_remote_callback("net.set_on_local_message_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_queue_on_main_fn(void* agent, QueueOnMainFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->queue_on_main = std::move(fn); return register_remote_callback("net.set_queue_on_main_fn", a); }
PJBRIDGE_EXPORT int bambu_network_set_server_callback(void* agent, OnServerErrFn fn) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->on_server_error = std::move(fn); return register_remote_callback("net.set_server_callback", a); }

PJBRIDGE_EXPORT int bambu_network_connect_server(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const int ret = RpcClient::instance().invoke_int("net.connect_server", {{"agent", agent_id(a)}});
    a->server_connected = ret == 0;
    return ret;
}

PJBRIDGE_EXPORT bool bambu_network_is_server_connected(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return false;
    const bool ret = RpcClient::instance().invoke_bool("net.is_server_connected", {{"agent", agent_id(a)}});
    a->server_connected = ret;
    return ret;
}

PJBRIDGE_EXPORT int bambu_network_refresh_connection(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.refresh_connection", {{"agent", agent_id(a)}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_start_subscribe(void* agent, std::string module) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.start_subscribe", {{"agent", agent_id(a)}, {"module", module}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_stop_subscribe(void* agent, std::string module) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.stop_subscribe", {{"agent", agent_id(a)}, {"module", module}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_add_subscribe(void* agent, std::vector<std::string> devs) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.add_subscribe", {{"agent", agent_id(a)}, {"devs", devs}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_del_subscribe(void* agent, std::vector<std::string> devs) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.del_subscribe", {{"agent", agent_id(a)}, {"devs", devs}}) : invalid_handle(); }
PJBRIDGE_EXPORT void bambu_network_enable_multi_machine(void* agent, bool enable) { auto* a = require_agent(agent); if (a) { a->multi_machine_enabled = enable; RpcClient::instance().invoke_void("net.enable_multi_machine", {{"agent", agent_id(a)}, {"enable", enable}}); } }

PJBRIDGE_EXPORT int bambu_network_send_message(void* agent, std::string dev_id, std::string msg, int qos, int flag) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.send_message", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"msg", msg}, {"qos", qos}, {"flag", flag}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_connect_printer(void* agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.connect_printer", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"dev_ip", dev_ip}, {"username", username}, {"password", password}, {"use_ssl", use_ssl}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_disconnect_printer(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.disconnect_printer", {{"agent", agent_id(a)}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_send_message_to_printer(void* agent, std::string dev_id, std::string msg, int qos, int flag) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.send_message_to_printer", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"msg", msg}, {"qos", qos}, {"flag", flag}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_update_cert(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.update_cert", {{"agent", agent_id(a)}}) : invalid_handle(); }
PJBRIDGE_EXPORT void bambu_network_install_device_cert(void* agent, std::string dev_id, bool lan_only) { auto* a = require_agent(agent); if (a) RpcClient::instance().invoke_void("net.install_device_cert", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"lan_only", lan_only}}); }
PJBRIDGE_EXPORT bool bambu_network_start_discovery(void* agent, bool start, bool sending) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_bool("net.start_discovery", {{"agent", agent_id(a)}, {"start", start}, {"sending", sending}}) : false; }

PJBRIDGE_EXPORT int bambu_network_change_user(void* agent, std::string user_info)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    a->user_info = std::move(user_info);
    const auto j = ok_or_error(RpcClient::instance().invoke_json("net.change_user", {{"agent", agent_id(a)}, {"user_info", a->user_info}}));
    a->logged_in = j.value("logged_in", !a->user_info.empty());
    fill_user_cache(a, j);
    return j.value("value", 0);
}

PJBRIDGE_EXPORT bool bambu_network_is_user_login(void* agent) { auto* a = require_agent(agent); if (!a) return false; a->logged_in = RpcClient::instance().invoke_bool("net.is_user_login", {{"agent", agent_id(a)}}); return a->logged_in; }
PJBRIDGE_EXPORT int bambu_network_user_logout(void* agent, bool request) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->logged_in = false; return RpcClient::instance().invoke_int("net.user_logout", {{"agent", agent_id(a)}, {"request", request}}); }
PJBRIDGE_EXPORT std::string bambu_network_get_user_id(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->user_id = RpcClient::instance().invoke_string("net.get_user_id", {{"agent", agent_id(a)}}); return a->user_id; }
PJBRIDGE_EXPORT std::string bambu_network_get_user_name(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->user_name = RpcClient::instance().invoke_string("net.get_user_name", {{"agent", agent_id(a)}}); return a->user_name; }
PJBRIDGE_EXPORT std::string bambu_network_get_user_avatar(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->user_avatar = RpcClient::instance().invoke_string("net.get_user_avatar", {{"agent", agent_id(a)}}); return a->user_avatar; }
PJBRIDGE_EXPORT std::string bambu_network_get_user_nickanme(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->user_nickname = RpcClient::instance().invoke_string("net.get_user_nickname", {{"agent", agent_id(a)}}); return a->user_nickname; }
PJBRIDGE_EXPORT std::string bambu_network_build_login_cmd(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->login_cmd = RpcClient::instance().invoke_string("net.build_login_cmd", {{"agent", agent_id(a)}}); return a->login_cmd; }
PJBRIDGE_EXPORT std::string bambu_network_build_logout_cmd(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->logout_cmd = RpcClient::instance().invoke_string("net.build_logout_cmd", {{"agent", agent_id(a)}}); return a->logout_cmd; }
PJBRIDGE_EXPORT std::string bambu_network_build_login_info(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->login_info = RpcClient::instance().invoke_string("net.build_login_info", {{"agent", agent_id(a)}}); return a->login_info; }
PJBRIDGE_EXPORT int bambu_network_ping_bind(void* agent, std::string ping_code) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.ping_bind", {{"agent", agent_id(a)}, {"ping_code", ping_code}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_bind_detect(void* agent, std::string dev_ip, std::string sec_link, detectResult& out)
{
    auto* a = require_agent(agent); if (!a) return invalid_handle();
    const auto j = ok_or_error(RpcClient::instance().invoke_json("net.bind_detect", {{"agent", agent_id(a)}, {"dev_ip", dev_ip}, {"sec_link", sec_link}}));
    if (j.contains("detect")) {
        const auto& d = j["detect"];
        out.result_msg = d.value("result_msg", std::string());
        out.command = d.value("command", std::string());
        out.dev_id = d.value("dev_id", std::string());
        out.model_id = d.value("model_id", std::string());
        out.dev_name = d.value("dev_name", std::string());
        out.version = d.value("version", std::string());
        out.bind_state = d.value("bind_state", std::string());
        out.connect_type = d.value("connect_type", std::string());
    }
    return j.value("value", 0);
}
PJBRIDGE_EXPORT int bambu_network_report_consent(void* agent, std::string expand) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.report_consent", {{"agent", agent_id(a)}, {"expand", expand}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_bind(void* agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update) { auto* a = require_agent(agent); return invoke_job_update_only("net.bind", "bind", a, {{"dev_ip", dev_ip}, {"dev_id", dev_id}, {"sec_link", sec_link}, {"timezone", timezone}, {"improved", improved}}, std::move(update)); }
PJBRIDGE_EXPORT int bambu_network_unbind(void* agent, std::string dev_id) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.unbind", {{"agent", agent_id(a)}, {"dev_id", dev_id}}) : invalid_handle(); }
PJBRIDGE_EXPORT std::string bambu_network_get_bambulab_host(void* agent)
{
    auto* a = require_agent(agent);
    if (!a)
        return {};
    const auto value = RpcClient::instance().invoke_string("net.get_bambulab_host", {{"agent", agent_id(a)}});
    if (!value.empty())
        a->bambulab_host = value;
    if (a->bambulab_host.empty())
        a->bambulab_host = "https://bambulab.com";
    return a->bambulab_host;
}
PJBRIDGE_EXPORT std::string bambu_network_get_user_selected_machine(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->selected_machine = RpcClient::instance().invoke_string("net.get_user_selected_machine", {{"agent", agent_id(a)}}); return a->selected_machine; }
PJBRIDGE_EXPORT int bambu_network_set_user_selected_machine(void* agent, std::string dev_id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->selected_machine = std::move(dev_id); return RpcClient::instance().invoke_int("net.set_user_selected_machine", {{"agent", agent_id(a)}, {"dev_id", a->selected_machine}}); }

PJBRIDGE_EXPORT int bambu_network_start_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_print", "print", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), std::move(wait), nullptr); }
PJBRIDGE_EXPORT int bambu_network_start_local_print_with_record(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_local_print_with_record", "local_print_with_record", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), std::move(wait), nullptr); }
PJBRIDGE_EXPORT int bambu_network_start_send_gcode_to_sdcard(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_send_gcode_to_sdcard", "send_gcode_to_sdcard", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), std::move(wait), nullptr); }
PJBRIDGE_EXPORT int bambu_network_start_local_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_local_print", "local_print", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), OnWaitFn(), nullptr); }
PJBRIDGE_EXPORT int bambu_network_start_sdcard_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_sdcard_print", "sdcard_print", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), OnWaitFn(), nullptr); }
PJBRIDGE_EXPORT int bambu_network_get_user_presets(void* agent, std::map<std::string, std::map<std::string, std::string>>* user_presets) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_presets", {{"agent", agent_id(a)}})); if (user_presets && j.contains("user_presets")) json_to_nested_string_map(j["user_presets"], *user_presets); return j.value("value", 0); }
PJBRIDGE_EXPORT std::string bambu_network_request_setting_id(void* agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) { auto* a = require_agent(agent); if (!a) return {}; const auto values = clone_string_map(values_map); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.request_setting_id", {{"agent", agent_id(a)}, {"name", name}, {"values", values}})); if (http_code) *http_code = j.value("http_code", 0u); return j.value("setting_id", std::string()); }
PJBRIDGE_EXPORT int bambu_network_put_setting(void* agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto values = clone_string_map(values_map); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_setting", {{"agent", agent_id(a)}, {"setting_id", setting_id}, {"name", name}, {"values", values}})); if (http_code) *http_code = j.value("http_code", 0u); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_setting_list(void* agent, std::string bundle_version, ProgressFn progress, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_progress_job("net.get_setting_list", "get_setting_list", a, {{"bundle_version", bundle_version}}, std::move(progress), std::move(cancel)); }
PJBRIDGE_EXPORT int bambu_network_get_setting_list2(void* agent, std::string bundle_version, CheckFn check, ProgressFn progress, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_progress_check_job("net.get_setting_list2", "get_setting_list2", a, {{"bundle_version", bundle_version}}, std::move(check), std::move(progress), std::move(cancel)); }
PJBRIDGE_EXPORT int bambu_network_delete_setting(void* agent, std::string setting_id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.delete_setting", {{"agent", agent_id(a)}, {"setting_id", setting_id}}); }
PJBRIDGE_EXPORT std::string bambu_network_get_studio_info_url(void* agent) { auto* a = require_agent(agent); if (!a) return {}; a->studio_info_url = RpcClient::instance().invoke_string("net.get_studio_info_url", {{"agent", agent_id(a)}}); return a->studio_info_url; }
PJBRIDGE_EXPORT int bambu_network_set_extra_http_header(void* agent, std::map<std::string, std::string> headers) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.set_extra_http_header", {{"agent", agent_id(a)}, {"headers", headers}}); }
PJBRIDGE_EXPORT int bambu_network_get_my_message(void* agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_message", {{"agent", agent_id(a)}, {"type", type}, {"after", after}, {"limit", limit}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_check_user_task_report(void* agent, int* task_id, bool* printable) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.check_user_task_report", {{"agent", agent_id(a)}})); if (task_id) *task_id = j.value("task_id", 0); if (printable) *printable = j.value("printable", false); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_user_print_info(void* agent, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_print_info", {{"agent", agent_id(a)}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_user_tasks(void* agent, TaskQueryParams params, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_tasks", {{"agent", agent_id(a)}, {"params", {{"dev_id", params.dev_id}, {"status", params.status}, {"offset", params.offset}, {"limit", params.limit}}}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_printer_firmware(void* agent, std::string dev_id, unsigned* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_printer_firmware", {{"agent", agent_id(a)}, {"dev_id", dev_id}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_task_plate_index(void* agent, std::string task_id, int* plate_index) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_task_plate_index", {{"agent", agent_id(a)}, {"task_id", task_id}})); if (plate_index) *plate_index = j.value("plate_index", -1); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_user_info(void* agent, int* identifier) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_info", {{"agent", agent_id(a)}})); if (identifier) *identifier = j.value("identifier", 0); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_request_bind_ticket(void* agent, std::string* ticket) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.request_bind_ticket", {{"agent", agent_id(a)}})); if (ticket) *ticket = j.value("ticket", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_subtask_info(void* agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_subtask_info", {{"agent", agent_id(a)}, {"subtask_id", subtask_id}})); if (task_json) *task_json = j.value("task_json", std::string()); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_slice_info(void* agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_slice_info", {{"agent", agent_id(a)}, {"project_id", project_id}, {"profile_id", profile_id}, {"plate_index", plate_index}})); if (slice_json) *slice_json = j.value("slice_json", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_query_bind_status(void* agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.query_bind_status", {{"agent", agent_id(a)}, {"query_list", query_list}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_modify_printer_name(void* agent, std::string dev_id, std::string dev_name) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.modify_printer_name", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"dev_name", dev_name}}); }
PJBRIDGE_EXPORT int bambu_network_get_camera_url(void* agent, std::string dev_id, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_camera_url", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}}, callback); }
PJBRIDGE_EXPORT int bambu_network_get_camera_url_for_golive(void* agent, std::string dev_id, std::string sdev_id, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_camera_url_for_golive", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"sdev_id", sdev_id}}, callback); }
PJBRIDGE_EXPORT int bambu_network_get_design_staffpick(void* agent, int offset, int limit, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_design_staffpick", a, {{"agent", agent_id(a)}, {"offset", offset}, {"limit", limit}}, callback); }
PJBRIDGE_EXPORT int bambu_network_start_publish(void* agent, PublishParams params, OnUpdateStatusFn update, WasCancelledFn cancel, std::string* out) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_publish", "publish", a, JsonBridge::to_json(params), std::move(update), std::move(cancel), OnWaitFn(), out); }
PJBRIDGE_EXPORT int bambu_network_get_model_publish_url(void* agent, std::string* url) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_publish_url", {{"agent", agent_id(a)}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_model_mall_home_url(void* agent, std::string* url) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_home_url", {{"agent", agent_id(a)}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_model_mall_detail_url(void* agent, std::string* url, std::string id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_detail_url", {{"agent", agent_id(a)}, {"id", id}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_subtask(void* agent, BBLModelTask* task, OnGetSubTaskFn callback) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_subtask", {{"agent", agent_id(a)}, {"task", model_task_to_json(task)}})); const int ret = j.value("value", 0); if (ret == 0 && callback && j.contains("subtask") && j["subtask"].is_object()) { BBLModelTask local_task{}; json_to_model_task(j["subtask"], local_task); if (task) *task = local_task; callback(&local_task); } return ret; }
PJBRIDGE_EXPORT int bambu_network_get_my_profile(void* agent, std::string token, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_profile", {{"agent", agent_id(a)}, {"token", token}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_my_token(void* agent, std::string ticket, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_token", {{"agent", agent_id(a)}, {"ticket", ticket}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }

PJBRIDGE_EXPORT int bambu_network_track_enable(void* agent, bool enable) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->tracking_enabled = enable; return RpcClient::instance().invoke_int("net.track_enable", {{"agent", agent_id(a)}, {"enable", enable}}); }
PJBRIDGE_EXPORT int bambu_network_track_remove_files(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.track_remove_files", {{"agent", agent_id(a)}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_track_event(void* agent, std::string evt_key, std::string content) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.track_event", {{"agent", agent_id(a)}, {"evt_key", evt_key}, {"content", content}}) : invalid_handle(); }
PJBRIDGE_EXPORT int bambu_network_track_header(void* agent, std::string header) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->track_header = header; return RpcClient::instance().invoke_int("net.track_header", {{"agent", agent_id(a)}, {"header", header}}); }
PJBRIDGE_EXPORT int bambu_network_track_update_property(void* agent, std::string name, std::string value, std::string type) { auto* a = require_agent(agent); if (!a) return invalid_handle(); a->track_properties[name] = value; return RpcClient::instance().invoke_int("net.track_update_property", {{"agent", agent_id(a)}, {"name", name}, {"value", value}, {"type", type}}); }
PJBRIDGE_EXPORT int bambu_network_track_get_property(void* agent, std::string name, std::string& value, std::string type) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.track_get_property", {{"agent", agent_id(a)}, {"name", name}, {"type", type}})); if (j.contains("property_value")) value = j.value("property_value", std::string()); else { auto it = a->track_properties.find(name); if (it != a->track_properties.end()) value = it->second; } return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_put_model_mall_rating(void* agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_model_mall_rating", {{"agent", agent_id(a)}, {"rating_id", rating_id}, {"score", score}, {"content", content}, {"images", images}})); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_oss_config(void* agent, std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_oss_config", {{"agent", agent_id(a)}, {"country_code", country_code}})); config = j.value("config", std::string()); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_put_rating_picture_oss(void* agent, std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_rating_picture_oss", {{"agent", agent_id(a)}, {"config", config}, {"pic_oss_path", pic_oss_path}, {"model_id", model_id}, {"profile_id", profile_id}})); config = j.value("config", config); pic_oss_path = j.value("pic_oss_path", pic_oss_path); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_model_mall_rating(void* agent, int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_rating", {{"agent", agent_id(a)}, {"job_id", job_id}})); rating_result = j.value("rating_result", std::string()); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
PJBRIDGE_EXPORT int bambu_network_get_mw_user_preference(void* agent, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_mw_user_preference", a, {{"agent", agent_id(a)}}, callback); }
PJBRIDGE_EXPORT int bambu_network_get_mw_user_4ulist(void* agent, int seed, int limit, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_mw_user_4ulist", a, {{"agent", agent_id(a)}, {"seed", seed}, {"limit", limit}}, callback); }
PJBRIDGE_EXPORT int bambu_network_get_hms_snapshot(void* agent, std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) { auto* a = require_agent(agent); return invoke_string_int_callback("net.get_hms_snapshot", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"file_name", file_name}}, callback); }
PJBRIDGE_EXPORT const char* bambu_network_get_last_error_msg() { return g_last_error.c_str(); }

PJBRIDGE_EXPORT int Bambu_Create(Bambu_Tunnel* tunnel, char const* path)
{
    if (!tunnel) return -1;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("src.create", {{"path", std::string(path ? path : "")}}));
    const int ret = j.value("value", -1);
    const auto remote = j.value("tunnel", 0LL);
    if (ret != 0 || remote == 0) {
        *tunnel = nullptr;
        return ret;
    }
    auto* t = new BridgeTunnel();
    t->remote_handle = remote;
    register_remote_tunnel(t);
    *tunnel = reinterpret_cast<Bambu_Tunnel>(t);
    return ret;
}

PJBRIDGE_EXPORT void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void* context)
{
    auto* t = require_tunnel(tunnel);
    if (!t) return;
    t->logger = logger;
    t->logger_ctx = context;
    RpcClient::instance().invoke_void("src.set_logger", {{"tunnel", t->remote_handle}});
}

PJBRIDGE_EXPORT int Bambu_Open(Bambu_Tunnel tunnel) { auto* t = require_tunnel(tunnel); if (!t) return -1; const int ret = RpcClient::instance().invoke_int("src.open", {{"tunnel", t->remote_handle}}); t->opened = ret == 0; return ret; }
PJBRIDGE_EXPORT int Bambu_StartStream(Bambu_Tunnel tunnel, bool video) { auto* t = require_tunnel(tunnel); return t ? RpcClient::instance().invoke_int("src.start_stream", {{"tunnel", t->remote_handle}, {"video", video}}) : -1; }
PJBRIDGE_EXPORT int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type) { auto* t = require_tunnel(tunnel); return t ? RpcClient::instance().invoke_int("src.start_stream_ex", {{"tunnel", t->remote_handle}, {"type", type}}) : -1; }
PJBRIDGE_EXPORT int Bambu_GetStreamCount(Bambu_Tunnel tunnel) { auto* t = require_tunnel(tunnel); return t ? RpcClient::instance().invoke_int("src.get_stream_count", {{"tunnel", t->remote_handle}}) : -1; }
PJBRIDGE_EXPORT int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info) { auto* t = require_tunnel(tunnel); if (!t || !info) return -1; const auto j = ok_or_error(RpcClient::instance().invoke_json("src.get_stream_info", {{"tunnel", t->remote_handle}, {"index", index}})); const int ret = j.value("value", -1); if (ret != 0 || !j.contains("info")) return ret; const auto& s = j["info"]; info->type = static_cast<Bambu_StreamType>(s.value("type", 0)); info->sub_type = s.value("sub_type", 0); info->format_type = s.value("format_type", 0); info->format_size = s.value("format_size", 0); info->max_frame_size = s.value("max_frame_size", 0); if (info->type == VIDE) { info->format.video.width = s.value("width", 0); info->format.video.height = s.value("height", 0); info->format.video.frame_rate = s.value("frame_rate", 0); } else { info->format.audio.sample_rate = s.value("sample_rate", 0); info->format.audio.channel_count = s.value("channel_count", 0); info->format.audio.sample_size = s.value("sample_size", 0); } const auto buf = s.value("format_buffer", std::string()); if (static_cast<int>(t->stream_format_buffers.size()) <= index) t->stream_format_buffers.resize(index + 1); t->stream_format_buffers[index].assign(buf.begin(), buf.end()); info->format_buffer = t->stream_format_buffers[index].empty() ? nullptr : t->stream_format_buffers[index].data(); return ret; }
PJBRIDGE_EXPORT unsigned long Bambu_GetDuration(Bambu_Tunnel tunnel) { auto* t = require_tunnel(tunnel); if (!t) return 0; const auto j = ok_or_error(RpcClient::instance().invoke_json("src.get_duration", {{"tunnel", t->remote_handle}})); return j.value("value", 0UL); }
PJBRIDGE_EXPORT int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long time) { auto* t = require_tunnel(tunnel); return t ? RpcClient::instance().invoke_int("src.seek", {{"tunnel", t->remote_handle}, {"time", time}}) : -1; }

PJBRIDGE_EXPORT int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    auto* t = require_tunnel(tunnel);
    if (!t || !sample)
        return -1;

    const auto reply = RpcClient::instance().invoke_binary("src.read_sample", {{"tunnel", t->remote_handle}});
    const auto j = ok_or_error(reply.payload);
    const int ret = j.value("value", -1);
    if (ret != 0 || !j.contains("sample"))
        return ret;

    const auto& s = j["sample"];
    CachedSample cached;
    cached.buffer = reply.binary;
    cached.itrack = s.value("itrack", 0);
    cached.size = s.value("size", 0);
    cached.flags = s.value("flags", 0);
    cached.decode_time = s.value("decode_time", 0ULL);
    t->sample_queue.push_back(std::move(cached));

    auto& front = t->sample_queue.back();
    sample->itrack = front.itrack;
    sample->size = front.size;
    sample->flags = front.flags;
    sample->decode_time = front.decode_time;
    sample->buffer = front.buffer.empty() ? nullptr : front.buffer.data();
    return ret;
}

PJBRIDGE_EXPORT int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const* data, int len)
{
    auto* t = require_tunnel(tunnel);
    if (!t)
        return -1;

    std::vector<unsigned char> binary;
    if (data && len > 0)
        binary.assign(reinterpret_cast<const unsigned char*>(data), reinterpret_cast<const unsigned char*>(data) + static_cast<std::size_t>(len));

    const auto reply = RpcClient::instance().invoke_binary("src.send_message", {{"tunnel", t->remote_handle}, {"ctrl", ctrl}}, binary);
    return reply.payload.value("value", -1);
}

PJBRIDGE_EXPORT int Bambu_RecvMessage(Bambu_Tunnel tunnel, int* ctrl, char* data, int* len)
{
    auto* t = require_tunnel(tunnel);
    if (!t || !len)
        return -1;

    const int caller_buffer_size = *len;
    const auto reply = RpcClient::instance().invoke_binary("src.recv_message", {{"tunnel", t->remote_handle}, {"buffer_size", caller_buffer_size}});
    const auto j = ok_or_error(reply.payload);
    const int ret = j.value("value", -1);
    if (ctrl)
        *ctrl = j.value("ctrl", 0);

    if (ret != 0) {
        if (j.contains("required_len"))
            *len = j.value("required_len", caller_buffer_size);
        return ret;
    }

    t->recv_message_buffer.assign(reply.binary.begin(), reply.binary.end());
    const int needed = j.contains("message_len") ? j.value("message_len", static_cast<int>(t->recv_message_buffer.size()))
                                                  : static_cast<int>(t->recv_message_buffer.size());
    if (!data || caller_buffer_size < needed) {
        *len = needed;
        return -1;
    }

    if (needed > 0)
        std::memcpy(data, t->recv_message_buffer.data(), static_cast<std::size_t>(needed));
    *len = needed;
    return ret;
}

PJBRIDGE_EXPORT void Bambu_Close(Bambu_Tunnel tunnel) { auto* t = require_tunnel(tunnel); if (t) RpcClient::instance().invoke_void("src.close", {{"tunnel", t->remote_handle}}); }
PJBRIDGE_EXPORT void Bambu_Destroy(Bambu_Tunnel tunnel) { auto* t = require_tunnel(tunnel); if (!t) return; unregister_remote_tunnel(t); RpcClient::instance().invoke_void("src.destroy", {{"tunnel", t->remote_handle}}); delete t; }
PJBRIDGE_EXPORT int Bambu_Init() { return RpcClient::instance().invoke_int("src.init"); }
PJBRIDGE_EXPORT void Bambu_Deinit() { RpcClient::instance().invoke_void("src.deinit"); }
PJBRIDGE_EXPORT char const* Bambu_GetLastErrorMsg() { const auto j = RpcClient::instance().invoke_json("src.get_last_error_msg"); if (j.value("ok", false) && j.contains("message")) g_last_error = j.value("message", std::string()); return g_last_error.c_str(); }
PJBRIDGE_EXPORT void Bambu_FreeLogMsg(tchar const* msg) { (void)msg; }

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
