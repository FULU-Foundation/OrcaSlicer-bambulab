#include "LinuxPluginHost.hpp"

#include "../../shared/pjarczak_linux_plugin_bridge_core/BridgeCoreJson.hpp"
#include "../../shared/pjarczak_linux_plugin_bridge_core/BridgeAuthPayload.hpp"
#include "../../src/slic3r/Utils/bambu_networking.hpp"
#include "../../src/slic3r/GUI/Printer/BambuTunnel.h"
#include "../../src/slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeCompat.hpp"
#include "../../src/slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <utility>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <unistd.h>

using namespace std::chrono_literals;

namespace Slic3r::PJarczakLinuxBridge {

namespace {

std::atomic<Slic3r::PJarczakLinuxBridge::LinuxPluginHost*> g_active_host{nullptr};

extern "C" void host_refresh_agora_url(char const* device, char const* dev_ver, char const* channel, void* context, void (*callback)(void* context, char const* url))
{
    auto* host = g_active_host.load();
    std::string url;
    if (host)
        url = host->refresh_camera_url_for_ft(device ? device : "", dev_ver ? dev_ver : "", channel ? channel : "");
    if (callback)
        callback(context, url.c_str());
}

extern "C" {
struct ft_job_result {
    int ec;
    int resp_ec;
    const char* json;
    const void* bin;
    uint32_t bin_size;
};
struct ft_job_msg {
    int kind;
    const char* json;
};
struct FT_TunnelHandle;
struct FT_JobHandle;
typedef int ft_err;
}

using fn_ft_abi_version = int (*)();
using fn_ft_free = void (*)(void *);
using fn_ft_job_result_destroy = void (*)(ft_job_result *);
using fn_ft_job_msg_destroy = void (*)(ft_job_msg *);
using fn_ft_tunnel_create = ft_err (*)(const char *url, FT_TunnelHandle **out);
using fn_ft_tunnel_release = void (*)(FT_TunnelHandle *);
using fn_ft_tunnel_sync_connect = ft_err (*)(FT_TunnelHandle *);
using fn_ft_tunnel_shutdown = ft_err (*)(FT_TunnelHandle *);
using fn_ft_job_create = ft_err (*)(const char *params_json, FT_JobHandle **out);
using fn_ft_job_release = void (*)(FT_JobHandle *);
using fn_ft_job_set_result_cb = ft_err (*)(FT_JobHandle *, void (*)(void *user, ft_job_result result), void *user);
using fn_ft_job_get_result = ft_err (*)(FT_JobHandle *, uint32_t timeout_ms, ft_job_result *out_result);
using fn_ft_tunnel_start_job = ft_err (*)(FT_TunnelHandle *, FT_JobHandle *);
using fn_ft_job_cancel = ft_err (*)(FT_JobHandle *);
using fn_ft_job_set_msg_cb = ft_err (*)(FT_JobHandle *, void (*)(void *user, ft_job_msg msg), void *user);
using fn_ft_job_try_get_msg = ft_err (*)(FT_JobHandle *, ft_job_msg *out_msg);
using fn_ft_job_get_msg = ft_err (*)(FT_JobHandle *, uint32_t timeout_ms, ft_job_msg *out_msg);

bool path_exists(const std::filesystem::path& path)
{
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

std::string env_or(const char* name, const char* fallback)
{
    if (const char* v = std::getenv(name))
        return v;
    return fallback;
}

std::mutex g_host_log_mutex;

std::string trim_for_log(const std::string& value, std::size_t limit = 8192)
{
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit) + "...<truncated>";
}

void host_log_json(const std::string& kind, const nlohmann::json& payload)
{
    try {
        nlohmann::json line;
        line["kind"] = kind;
        line["payload"] = payload;
        std::lock_guard<std::mutex> lock(g_host_log_mutex);
        std::cerr << "[PJBRIDGE] " << trim_for_log(line.dump()) << std::endl;
    } catch (...) {
    }
}


std::string windows_path_to_wsl(std::string path)
{
    if (path.empty())
        return path;

    if (path.rfind("\\\\?\\", 0) == 0)
        path.erase(0, 4);

    if (path.size() < 3)
        return path;

    const unsigned char drive = static_cast<unsigned char>(path[0]);
    const bool drive_ok =
        (drive >= 'A' && drive <= 'Z') ||
        (drive >= 'a' && drive <= 'z');

    if (!drive_ok || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
        return path;

    std::string out = "/mnt/";
    out.push_back(static_cast<char>(std::tolower(drive)));
    out.push_back('/');

    for (std::size_t i = 3; i < path.size(); ++i)
        out.push_back(path[i] == '\\' ? '/' : path[i]);

    return out;
}

std::vector<std::string> windows_paths_to_wsl(std::vector<std::string> values)
{
    for (std::string& value : values)
        value = windows_path_to_wsl(std::move(value));
    return values;
}

void translate_print_params_paths(BBL::PrintParams& p)
{
    p.filename = windows_path_to_wsl(std::move(p.filename));
    p.config_filename = windows_path_to_wsl(std::move(p.config_filename));
    p.dst_file = windows_path_to_wsl(std::move(p.dst_file));
}


thread_local std::vector<unsigned char> g_thread_request_binary;
thread_local std::vector<unsigned char> g_thread_reply_binary;

struct LoggerCallbackContext {
    LinuxPluginHost* host{nullptr};
    std::int64_t tunnel_id{0};
    void (*free_fn)(tchar const*){nullptr};
};

void logger_callback_forwarder(void* ctx, int level, tchar const* msg)
{
    auto* context = static_cast<LoggerCallbackContext*>(ctx);
    if (!context || !context->host)
        return;

    context->host->dispatch_logger_event(context->tunnel_id, level, std::string(msg ? msg : ""));
    if (context->free_fn && msg)
        context->free_fn(msg);
}


std::string host_arch_string()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

void clear_thread_reply_binary()
{
    g_thread_reply_binary.clear();
}

void copy_ft_job_result_payload(HostFtJobState& state, const ft_job_result& result)
{
    state.result_ec = result.ec;
    state.result_resp_ec = result.resp_ec;
    state.result_json.assign(result.json ? result.json : "");
    state.result_bin.clear();
    if (result.bin && result.bin_size) {
        const auto* first = static_cast<const unsigned char*>(result.bin);
        state.result_bin.assign(first, first + result.bin_size);
    }
}

std::shared_ptr<HostFtJobState> lookup_ft_job_state(std::map<std::int64_t, std::shared_ptr<HostFtJobState>>& states, std::mutex& mutex, std::int64_t id)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = states.find(id);
    if (it == states.end())
        return {};
    return it->second;
}

std::map<std::string, std::string> json_to_string_map(const nlohmann::json& j)
{
    std::map<std::string, std::string> out;
    if (!j.is_object())
        return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string())
            out[it.key()] = it.value().get<std::string>();
        else
            out[it.key()] = it.value().dump();
    }
    return out;
}

std::map<std::string, std::map<std::string, std::string>> json_to_nested_string_map(const nlohmann::json& j)
{
    std::map<std::string, std::map<std::string, std::string>> out;
    if (!j.is_object())
        return out;
    for (auto it = j.begin(); it != j.end(); ++it)
        out[it.key()] = json_to_string_map(it.value());
    return out;
}

nlohmann::json nested_string_map_to_json(const std::map<std::string, std::map<std::string, std::string>>& value)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, inner] : value)
        out[k] = inner;
    return out;
}

BBL::TaskQueryParams task_query_from_json(const nlohmann::json& j)
{
    BBL::TaskQueryParams p{};
    p.dev_id = j.value("dev_id", std::string());
    p.status = j.value("status", 0);
    p.offset = j.value("offset", 0);
    p.limit = j.value("limit", 20);
    return p;
}

BBL::PrintParams print_params_from_json(const nlohmann::json& j)
{
    BBL::PrintParams p{};
    p.dev_id = j.value("dev_id", std::string());
    p.task_name = j.value("task_name", std::string());
    p.project_name = j.value("project_name", std::string());
    p.preset_name = j.value("preset_name", std::string());
    p.filename = j.value("filename", std::string());
    p.config_filename = j.value("config_filename", std::string());
    p.plate_index = j.value("plate_index", 0);
    p.ftp_folder = j.value("ftp_folder", std::string());
    p.ftp_file = j.value("ftp_file", std::string());
    p.ftp_file_md5 = j.value("ftp_file_md5", std::string());
    p.nozzle_mapping = j.value("nozzle_mapping", std::string());
    p.ams_mapping = j.value("ams_mapping", std::string());
    p.ams_mapping2 = j.value("ams_mapping2", std::string());
    p.ams_mapping_info = j.value("ams_mapping_info", std::string());
    p.nozzles_info = j.value("nozzles_info", std::string());
    p.connection_type = j.value("connection_type", std::string());
    p.comments = j.value("comments", std::string());
    p.origin_profile_id = j.value("origin_profile_id", 0);
    p.stl_design_id = j.value("stl_design_id", 0);
    p.origin_model_id = j.value("origin_model_id", std::string());
    p.print_type = j.value("print_type", std::string());
    p.dst_file = j.value("dst_file", std::string());
    p.dev_name = j.value("dev_name", std::string());
    p.dev_ip = j.value("dev_ip", std::string());
    p.use_ssl_for_ftp = j.value("use_ssl_for_ftp", false);
    p.use_ssl_for_mqtt = j.value("use_ssl_for_mqtt", false);
    p.username = j.value("username", std::string());
    p.password = j.value("password", std::string());
    p.task_bed_leveling = j.value("task_bed_leveling", false);
    p.task_flow_cali = j.value("task_flow_cali", false);
    p.task_vibration_cali = j.value("task_vibration_cali", false);
    p.task_layer_inspect = j.value("task_layer_inspect", false);
    p.task_record_timelapse = j.value("task_record_timelapse", false);
    p.task_use_ams = j.value("task_use_ams", false);
    p.task_bed_type = j.value("task_bed_type", std::string());
    p.extra_options = j.value("extra_options", std::string());
    p.auto_bed_leveling = j.value("auto_bed_leveling", 0);
    p.auto_flow_cali = j.value("auto_flow_cali", 0);
    p.auto_offset_cali = j.value("auto_offset_cali", 0);
    p.extruder_cali_manual_mode = j.value("extruder_cali_manual_mode", -1);
    p.task_ext_change_assist = j.value("task_ext_change_assist", false);
    p.try_emmc_print = j.value("try_emmc_print", false);
    translate_print_params_paths(p);
    return p;
}

template <typename Invoke>
nlohmann::json wait_string_callback(Invoke&& invoke)
{
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::string result;
    const int ret = invoke([&](std::string value) {
        {
            std::lock_guard<std::mutex> lock(m);
            result = std::move(value);
            ready = true;
        }
        cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, 120s, [&] { return ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"result", result}};
}

template <typename Invoke>
nlohmann::json wait_string_int_callback(Invoke&& invoke)
{
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::string result;
    int status = 0;
    const int ret = invoke([&](std::string value, int st) {
        {
            std::lock_guard<std::mutex> lock(m);
            result = std::move(value);
            status = st;
            ready = true;
        }
        cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, 120s, [&] { return ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"result", result}, {"status", status}};
}

template <typename Invoke>
nlohmann::json wait_model_task_callback(Invoke&& invoke)
{
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    nlohmann::json subtask = nlohmann::json::object();
    const int ret = invoke([&](Slic3r::BBLModelTask* value) {
        {
            std::lock_guard<std::mutex> lock(m);
            subtask = model_task_to_json(value);
            ready = true;
        }
        cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, 120s, [&] { return ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"subtask", subtask}};
}

}

LinuxPluginHost::LinuxPluginHost()
{
    g_active_host.store(this);
    load_modules();
    ensure_main_dispatcher();
}

LinuxPluginHost::~LinuxPluginHost()
{
    if (g_active_host.load() == this)
        g_active_host.store(nullptr);
    stop_main_dispatcher();
}

void LinuxPluginHost::dispatch_logger_event(std::int64_t tunnel_handle, int level, const std::string& message)
{
    queue_tunnel_event(tunnel_handle, "logger", {{"level", level}, {"message", message}});
}

std::string LinuxPluginHost::refresh_camera_url_for_ft(const std::string& device, const std::string& dev_ver, const std::string& channel)
{
    auto f = net<int (*)(void*, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url");
    if (!f)
        return {};
    void* agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_agents.empty())
            agent = m_agents.begin()->second;
    }
    if (!agent)
        return {};
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::string result;
    std::string dev_arg = device + "|" + dev_ver + "|\"agora\"|" + channel;
    const int ret = f(agent, dev_arg, [&](std::string value) {
        {
            std::lock_guard<std::mutex> lock(m);
            result = std::move(value);
            ready = true;
        }
        cv.notify_one();
    });
    if (ret != 0)
        return {};
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, 20s, [&] { return ready; }))
        return {};
    return result;
}

std::string LinuxPluginHost::refresh_agora_url_ptr_string() const
{
    const auto value = reinterpret_cast<std::uintptr_t>(&host_refresh_agora_url);
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

void LinuxPluginHost::ensure_main_dispatcher()
{
    if (m_main_dispatcher.joinable())
        return;
    m_stop_main_dispatcher = false;
    m_main_dispatcher = std::thread([this] { main_dispatch_loop(); });
}

void LinuxPluginHost::stop_main_dispatcher()
{
    m_stop_main_dispatcher = true;
    m_main_tasks_cv.notify_all();
    if (m_main_dispatcher.joinable())
        m_main_dispatcher.join();
}

void LinuxPluginHost::queue_main_task(std::function<void()> fn)
{
    if (!fn)
        return;
    {
        std::lock_guard<std::mutex> lock(m_main_tasks_mutex);
        m_main_tasks.push_back(std::move(fn));
    }
    m_main_tasks_cv.notify_one();
}

void LinuxPluginHost::main_dispatch_loop()
{
    while (!m_stop_main_dispatcher.load()) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(m_main_tasks_mutex);
            m_main_tasks_cv.wait(lock, [this] { return m_stop_main_dispatcher.load() || !m_main_tasks.empty(); });
            if (m_stop_main_dispatcher.load() && m_main_tasks.empty())
                break;
            fn = std::move(m_main_tasks.front());
            m_main_tasks.pop_front();
        }
        try {
            fn();
        } catch (...) {
        }
    }
}

void LinuxPluginHost::set_thread_request_binary(std::vector<unsigned char> data)
{
    g_thread_request_binary = std::move(data);
}

bool LinuxPluginHost::consume_thread_reply_binary(std::vector<unsigned char>& out)
{
    if (g_thread_reply_binary.empty())
        return false;
    out = std::move(g_thread_reply_binary);
    g_thread_reply_binary.clear();
    return true;
}


void LinuxPluginHost::load_modules()
{
    const std::filesystem::path plugin_folder = std::filesystem::path(env_or("PJARCZAK_BAMBU_PLUGIN_DIR", "."));
    std::string manifest_reason;
    const bool have_manifest = path_exists(linux_payload_manifest_path(plugin_folder));
    const bool manifest_ok = !have_manifest || validate_linux_payload_set_against_manifest(plugin_folder, &manifest_reason);

    if (!manifest_ok) {
        m_network_status = "manifest invalid: " + manifest_reason;
        m_source_status = "manifest invalid: " + manifest_reason;
    }

    if (!m_network) {
        const auto path = env_or("PJARCZAK_BAMBU_NETWORK_SO", "libbambu_networking.so");
        std::string reason;
        if (!manifest_ok) {
            m_network_status = "manifest invalid: " + manifest_reason;
        } else if (!validate_linux_payload_file(path, &reason)) {
            m_network_status = "validate failed: " + reason;
        } else {
            m_network = dlopen(path.c_str(), RTLD_LAZY);
            if (!m_network) {
                const char* err = dlerror();
                m_network_status = err && *err ? std::string("dlopen failed: ") + err : "dlopen failed";
            } else {
                using get_version_fn = std::string (*)();
                auto gv = reinterpret_cast<get_version_fn>(dlsym(m_network, "bambu_network_get_version"));
                if (gv) {
                    std::string abi_reason;
                    const auto actual = gv();
                    m_network_actual_abi_version = actual;
                    if (!abi_version_matches_expected(actual, &abi_reason)) {
                        dlclose(m_network);
                        m_network = nullptr;
                        m_network_status = abi_reason;
                    } else {
                        m_network_status = "loaded";
                    }
                } else {
                    m_network_actual_abi_version.clear();
                    m_network_status = "loaded";
                }
            }
        }
    } else {
        m_network_status = "loaded";
    }

    if (!m_source) {
        const auto path = env_or("PJARCZAK_BAMBU_SOURCE_SO", "libBambuSource.so");
        std::string reason;
        if (!manifest_ok) {
            m_source_status = "manifest invalid: " + manifest_reason;
        } else if (!validate_linux_payload_file(path, &reason)) {
            m_source_status = "validate failed: " + reason;
        } else {
            m_source = dlopen(path.c_str(), RTLD_LAZY);
            if (!m_source) {
                const char* err = dlerror();
                m_source_status = err && *err ? std::string("dlopen failed: ") + err : "dlopen failed";
            } else {
                m_source_status = "loaded";
            }
        }
    } else {
        m_source_status = "loaded";
    }
}

void* LinuxPluginHost::resolve_network(const char* name)
{
    load_modules();
    return m_network ? dlsym(m_network, name) : nullptr;
}

void* LinuxPluginHost::resolve_source(const char* name)
{
    load_modules();
    return m_source ? dlsym(m_source, name) : nullptr;
}

bool LinuxPluginHost::has_network_symbol(const char* name)
{
    load_modules();
    return m_network && dlsym(m_network, name) != nullptr;
}

nlohmann::json LinuxPluginHost::auth_capabilities() const
{
    auto self = const_cast<LinuxPluginHost*>(this);
    return {
        {"network_loaded", self->m_network != nullptr},
        {"source_loaded", self->m_source != nullptr},
        {"bambu_network_is_user_login", self->has_network_symbol("bambu_network_is_user_login")},
        {"bambu_network_get_user_id", self->has_network_symbol("bambu_network_get_user_id")},
        {"bambu_network_get_user_name", self->has_network_symbol("bambu_network_get_user_name")},
        {"bambu_network_get_user_avatar", self->has_network_symbol("bambu_network_get_user_avatar")},
        {"bambu_network_get_user_nickanme", self->has_network_symbol("bambu_network_get_user_nickanme")},
        {"bambu_network_build_login_cmd", self->has_network_symbol("bambu_network_build_login_cmd")},
        {"bambu_network_build_logout_cmd", self->has_network_symbol("bambu_network_build_logout_cmd")},
        {"bambu_network_build_login_info", self->has_network_symbol("bambu_network_build_login_info")},
        {"bambu_network_change_user", self->has_network_symbol("bambu_network_change_user")},
        {"bambu_network_get_my_profile", self->has_network_symbol("bambu_network_get_my_profile")},
        {"bambu_network_get_my_message", self->has_network_symbol("bambu_network_get_my_message")},
        {"bambu_network_get_my_token", self->has_network_symbol("bambu_network_get_my_token")}
    };
}

nlohmann::json LinuxPluginHost::not_supported(const std::string& method) const
{
    return {
        {"ok", false},
        {"error", method + " unsupported in host"},
        {"reason", "missing_symbol_or_invalid_agent"},
        {"agent_count", m_agents.size()},
        {"auth_capabilities", auth_capabilities()}
    };
}

void LinuxPluginHost::queue_event(std::int64_t agent_handle, const std::string& name, const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(m_events_mutex);
    m_events.push_back({{"agent", agent_handle}, {"name", name}, {"payload", payload}});
}

void LinuxPluginHost::queue_tunnel_event(std::int64_t tunnel_handle, const std::string& name, const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(m_events_mutex);
    m_events.push_back({{"tunnel", tunnel_handle}, {"name", name}, {"payload", payload}});
}

nlohmann::json LinuxPluginHost::drain_events(std::size_t limit)
{
    std::lock_guard<std::mutex> lock(m_events_mutex);
    nlohmann::json arr = nlohmann::json::array();
    while (!m_events.empty() && arr.size() < limit) {
        arr.push_back(m_events.front());
        m_events.pop_front();
    }
    return {{"ok", true}, {"events", arr}};
}

std::shared_ptr<HostJobState> LinuxPluginHost::get_job(std::int64_t job_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    auto it = m_jobs.find(job_id);
    return it == m_jobs.end() ? nullptr : it->second;
}

void LinuxPluginHost::register_job(const std::shared_ptr<HostJobState>& job)
{
    if (!job)
        return;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_jobs[job->job_id] = job;
}

void LinuxPluginHost::unregister_job(std::int64_t job_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_jobs.erase(job_id);
}

void LinuxPluginHost::set_job_cancel(std::int64_t job_id, bool value)
{
    auto job = get_job(job_id);
    if (job)
        job->cancel_requested = value;
}

void LinuxPluginHost::set_job_wait_reply(std::int64_t job_id, std::int64_t request_id, bool value)
{
    auto job = get_job(job_id);
    if (!job)
        return;
    {
        std::lock_guard<std::mutex> lock(job->wait_mutex);
        job->wait_request_id = request_id;
        job->wait_reply_value = value;
        job->wait_reply_ready = true;
    }
    job->wait_cv.notify_all();
}

std::shared_ptr<HostCallbackReplyState> LinuxPluginHost::register_callback_request(std::int64_t request_id)
{
    auto state = std::make_shared<HostCallbackReplyState>();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_callback_replies[request_id] = state;
    return state;
}

void LinuxPluginHost::unregister_callback_request(std::int64_t request_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_callback_replies.erase(request_id);
}

void LinuxPluginHost::set_callback_reply(std::int64_t request_id, const std::string& value)
{
    std::shared_ptr<HostCallbackReplyState> state;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_callback_replies.find(request_id);
        if (it == m_callback_replies.end())
            return;
        state = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->string_value = value;
        state->ready = true;
    }
    state->cv.notify_all();
}

nlohmann::json LinuxPluginHost::handle(const std::string& method, const nlohmann::json& payload)
{
    using namespace BBL;

    clear_thread_reply_binary();

    if (method == "bridge.poll_events")
        return drain_events(payload.value("limit", 64U));

    if (method == "bridge.ping")
        return {{"ok", true}, {"value", "pong"}};

    if (method == "bridge.get_refresh_agora_url_ptr") {
        return {{"ok", true}, {"value", 0}, {"result", refresh_agora_url_ptr_string()}};
    }
    if (method == "bridge.handshake") {
        const std::filesystem::path plugin_folder = std::filesystem::path(env_or("PJARCZAK_BAMBU_PLUGIN_DIR", "."));
        nlohmann::json out = nlohmann::json::object();
        out["ok"] = true;
        out["protocol_version"] = 1;
        out["bridge_version"] = "PJ-LINUX-BRIDGE-0.5";
        out["network_abi_version"] = expected_network_abi_version();
        out["network_actual_abi_version"] = m_network_actual_abi_version;
        out["guest_arch"] = host_arch_string();
        out["plugin_dir"] = plugin_folder.string();
        out["network_so_present"] = path_exists(plugin_folder / linux_network_library_name());
        out["source_so_present"] = path_exists(plugin_folder / linux_source_library_name());
        out["manifest_present"] = path_exists(plugin_folder / linux_payload_manifest_file_name());
        out["network_loaded"] = m_network != nullptr;
        out["source_loaded"] = m_source != nullptr;
        out["network_status"] = m_network_status;
        out["source_status"] = m_source_status;
        out["auth_capabilities"] = auth_capabilities();
        out["agent_count"] = m_agents.size();
        return out;
    }
    if (method == "bridge.capabilities") {
        return {{"ok", true}, {"agent_count", m_agents.size()}, {"auth_capabilities", auth_capabilities()}};
    }
    if (method == "bridge.runtime_info") {
        char cwd_buf[4096] = {0};
        std::string cwd;
        if (::getcwd(cwd_buf, sizeof(cwd_buf) - 1))
            cwd = cwd_buf;
        nlohmann::json out = nlohmann::json::object();
        out["ok"] = true;
        out["cwd"] = cwd;
        out["home"] = env_or("HOME", "");
        out["plugin_dir"] = env_or("PJARCZAK_BAMBU_PLUGIN_DIR", "");
        out["network_so"] = env_or("PJARCZAK_BAMBU_NETWORK_SO", "");
        out["source_so"] = env_or("PJARCZAK_BAMBU_SOURCE_SO", "");
        out["ssl_cert_file"] = env_or("SSL_CERT_FILE", "");
        out["ssl_cert_dir"] = env_or("SSL_CERT_DIR", "");
        out["curl_ca_bundle"] = env_or("CURL_CA_BUNDLE", "");
        out["ld_library_path"] = env_or("LD_LIBRARY_PATH", "");
        out["network_loaded"] = m_network != nullptr;
        out["source_loaded"] = m_source != nullptr;
        out["network_status"] = m_network_status;
        out["source_status"] = m_source_status;
        host_log_json("bridge.runtime_info", out);
        return out;
    }

    if (method == "bridge.job_cancel") {
        set_job_cancel(payload.value("job_id", 0LL), payload.value("cancel", true));
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "bridge.job_wait_reply") {
        set_job_wait_reply(payload.value("job_id", 0LL), payload.value("request_id", 0LL), payload.value("reply", true));
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "bridge.callback_reply") {
        set_callback_reply(payload.value("request_id", 0LL), payload.value("value", std::string()));
        return {{"ok", true}, {"value", 0}};
    }

    if (method == "net.create_agent") {
        auto f = net<void* (*)(std::string)>("bambu_network_create_agent");
        if (!f) return not_supported(method);
        const std::string log_dir = windows_path_to_wsl(payload.value("log_dir", std::string()));
        void* raw = f(log_dir);
        const auto id = m_next_agent++;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_agents[id] = raw;
            m_country_codes[id] = payload.value("country_code", std::string());
        }
        return {{"ok", true}, {"value", id}};
    }
    if (method == "net.destroy_agent") {
        auto f = net<int (*)(void*)>("bambu_network_destroy_agent");
        if (!f) return not_supported(method);
        const auto id = payload.value("agent", 0LL);
        void* raw = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_agents.find(id);
            if (it == m_agents.end()) return {{"ok", false}, {"error", "agent not found"}};
            raw = it->second;
            m_agents.erase(it);
            m_country_codes.erase(id);
        }
        const int ret = f(raw);
        return {{"ok", true}, {"value", ret}};
    }

    const auto lookup_agent = [&]() -> void* {
        const auto id = payload.value("agent", 0LL);
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_agents.find(id);
        if (it == m_agents.end())
            return nullptr;
        return it->second;
    };

    const auto agent_id = payload.value("agent", 0LL);

    const auto lookup_tunnel = [&]() -> Bambu_Tunnel {
        const auto id = payload.value("tunnel", 0LL);
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_tunnels.find(id);
        return it == m_tunnels.end() ? nullptr : static_cast<Bambu_Tunnel>(it->second);
    };

    const auto lookup_ft_tunnel = [&]() -> FT_TunnelHandle* {
        const auto id = payload.value("tunnel", 0LL);
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_ft_tunnels.find(id);
        return it == m_ft_tunnels.end() ? nullptr : static_cast<FT_TunnelHandle*>(it->second);
    };

    const auto lookup_ft_job = [&]() -> FT_JobHandle* {
        const auto id = payload.value("job", 0LL);
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_ft_jobs.find(id);
        return it == m_ft_jobs.end() ? nullptr : static_cast<FT_JobHandle*>(it->second);
    };

    if (method == "net.set_config_dir") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_set_config_dir");
        auto a = lookup_agent();
        const std::string config_dir = windows_path_to_wsl(payload.value("config_dir", std::string()));
        return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, config_dir)}} : not_supported(method);
    }
    if (method == "net.set_cert_file") {
        auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_set_cert_file");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto folder = windows_path_to_wsl(payload.value("folder", std::string()));
        const auto filename = payload.value("filename", std::string());
        const int ret = f(a, folder, filename);
        nlohmann::json r{{"ok", true}, {"value", ret}, {"folder", folder}, {"filename", filename}, {"ssl_cert_file", env_or("SSL_CERT_FILE", "")}, {"curl_ca_bundle", env_or("CURL_CA_BUNDLE", "")}};
        host_log_json("net.set_cert_file", r);
        return r;
    }
    if (method == "net.set_country_code") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_set_country_code");
        auto a = lookup_agent();
        const auto code = payload.value("country_code", std::string());
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_country_codes[agent_id] = code; }
        return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, code)}} : not_supported(method);
    }
    if (method == "net.init_log") { auto f = net<int (*)(void*)>("bambu_network_init_log"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.start") { auto f = net<int (*)(void*)>("bambu_network_start"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.connect_server") { auto f = net<int (*)(void*)>("bambu_network_connect_server"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.is_server_connected") { auto f = net<bool (*)(void*)>("bambu_network_is_server_connected"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.refresh_connection") { auto f = net<int (*)(void*)>("bambu_network_refresh_connection"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.start_subscribe") { auto f = net<int (*)(void*, std::string)>("bambu_network_start_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("module", std::string()))}} : not_supported(method); }
    if (method == "net.stop_subscribe") { auto f = net<int (*)(void*, std::string)>("bambu_network_stop_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("module", std::string()))}} : not_supported(method); }
    if (method == "net.add_subscribe") { auto f = net<int (*)(void*, std::vector<std::string>)>("bambu_network_add_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("devs", std::vector<std::string>()))}} : not_supported(method); }
    if (method == "net.del_subscribe") { auto f = net<int (*)(void*, std::vector<std::string>)>("bambu_network_del_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("devs", std::vector<std::string>()))}} : not_supported(method); }
    if (method == "net.enable_multi_machine") { auto f = net<void (*)(void*, bool)>("bambu_network_enable_multi_machine"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); f(a, payload.value("enable", false)); return {{"ok", true}, {"value", 0}}; }
    if (method == "net.send_message") { auto f = net<int (*)(void*, std::string, std::string, int, int)>("bambu_network_send_message"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("msg", std::string()), payload.value("qos", 0), payload.value("flag", 0))}} : not_supported(method); }
    if (method == "net.connect_printer") { auto f = net<int (*)(void*, std::string, std::string, std::string, std::string, bool)>("bambu_network_connect_printer"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("dev_ip", std::string()), payload.value("username", std::string()), payload.value("password", std::string()), payload.value("use_ssl", false))}} : not_supported(method); }
    if (method == "net.disconnect_printer") { auto f = net<int (*)(void*)>("bambu_network_disconnect_printer"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.send_message_to_printer") { auto f = net<int (*)(void*, std::string, std::string, int, int)>("bambu_network_send_message_to_printer"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("msg", std::string()), payload.value("qos", 0), payload.value("flag", 0))}} : not_supported(method); }
    if (method == "net.update_cert") { auto f = net<int (*)(void*)>("bambu_network_update_cert"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.install_device_cert") { auto f = net<void (*)(void*, std::string, bool)>("bambu_network_install_device_cert"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); f(a, payload.value("dev_id", std::string()), payload.value("lan_only", false)); return {{"ok", true}, {"value", 0}}; }
    if (method == "net.start_discovery") { auto f = net<bool (*)(void*, bool, bool)>("bambu_network_start_discovery"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("start", false), payload.value("sending", false))}} : not_supported(method); }

    if (method == "net.set_on_ssdp_msg_fn") {
        auto f = net<int (*)(void*, OnMsgArrivedFn)>("bambu_network_set_on_ssdp_msg_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_info_json_str) { queue_event(agent_id, "on_ssdp_msg", {{"dev_info_json_str", dev_info_json_str}}); })}};
    }
    if (method == "net.set_on_user_login_fn") {
        auto f = net<int (*)(void*, OnUserLoginFn)>("bambu_network_set_on_user_login_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int online_login, bool login) { queue_event(agent_id, "on_user_login", {{"online_login", online_login}, {"login", login}}); })}};
    }
    if (method == "net.set_on_printer_connected_fn") {
        auto f = net<int (*)(void*, OnPrinterConnectedFn)>("bambu_network_set_on_printer_connected_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string topic_str) { queue_event(agent_id, "on_printer_connected", {{"topic_str", topic_str}}); })}};
    }
    if (method == "net.set_on_server_connected_fn") {
        auto f = net<int (*)(void*, OnServerConnectedFn)>("bambu_network_set_on_server_connected_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int return_code, int reason_code) { queue_event(agent_id, "on_server_connected", {{"return_code", return_code}, {"reason_code", reason_code}}); })}};
    }
    if (method == "net.set_on_http_error_fn") {
        auto f = net<int (*)(void*, OnHttpErrorFn)>("bambu_network_set_on_http_error_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](unsigned http_code, std::string http_body) { queue_event(agent_id, "on_http_error", {{"http_code", http_code}, {"http_body", http_body}}); })}};
    }
    if (method == "net.set_get_country_code_fn") {
        auto f = net<int (*)(void*, GetCountryCodeFn)>("bambu_network_set_get_country_code_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id]() {
            const auto request_id = m_next_callback_request.fetch_add(1);
            auto state = register_callback_request(request_id);
            queue_event(agent_id, "callback.get_country_code", {{"request_id", request_id}});
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->cv.wait_for(lock, 30s, [&] { return state->ready; })) {
                unregister_callback_request(request_id);
                std::lock_guard<std::mutex> s_lock(m_state_mutex);
                auto it = m_country_codes.find(agent_id);
                return it == m_country_codes.end() ? std::string() : it->second;
            }
            const auto value = state->string_value;
            lock.unlock();
            unregister_callback_request(request_id);
            return value;
        })}};
    }
    if (method == "net.set_on_subscribe_failure_fn") {
        auto f = net<int (*)(void*, GetSubscribeFailureFn)>("bambu_network_set_on_subscribe_failure_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string topic) { queue_event(agent_id, "on_subscribe_failure", {{"topic", topic}}); })}};
    }
    if (method == "net.set_on_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_user_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_user_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_user_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_local_connect_fn") {
        auto f = net<int (*)(void*, OnLocalConnectedFn)>("bambu_network_set_on_local_connect_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int status, std::string dev_id, std::string msg) { queue_event(agent_id, "on_local_connect", {{"status", status}, {"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_local_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_local_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_local_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_queue_on_main_fn") {
        auto f = net<int (*)(void*, QueueOnMainFn)>("bambu_network_set_queue_on_main_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this](std::function<void()> fn) { queue_main_task(std::move(fn)); })}};
    }
    if (method == "net.set_server_callback") {
        auto f = net<int (*)(void*, OnServerErrFn)>("bambu_network_set_server_callback");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string url, int status) { queue_event(agent_id, "on_server_error", {{"url", url}, {"status", status}}); })}};
    }

    if (method == "net.change_user") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_change_user");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto original_user_info = payload.value("user_info", std::string());
        const auto normalized_user_info = normalize_change_user_payload_string(original_user_info);
        const int ret = f(a, normalized_user_info);
        nlohmann::json r{{"ok", true}, {"value", ret}, {"user_info_original", original_user_info}, {"user_info_normalized", normalized_user_info}, {"user_info_was_normalized", normalized_user_info != original_user_info}};
        auto g1 = net<bool (*)(void*)>("bambu_network_is_user_login"); if (g1) r["logged_in"] = g1(a);
        auto g2 = net<std::string (*)(void*)>("bambu_network_get_user_id"); if (g2) r["user_id"] = g2(a);
        auto g3 = net<std::string (*)(void*)>("bambu_network_get_user_name"); if (g3) r["user_name"] = g3(a);
        auto g4 = net<std::string (*)(void*)>("bambu_network_get_user_avatar"); if (g4) r["user_avatar"] = g4(a);
        auto g5 = net<std::string (*)(void*)>("bambu_network_get_user_nickanme"); if (g5) r["user_nickname"] = g5(a);
        auto g6 = net<std::string (*)(void*)>("bambu_network_build_login_cmd"); if (g6) r["login_cmd"] = g6(a);
        auto g7 = net<std::string (*)(void*)>("bambu_network_build_logout_cmd"); if (g7) r["logout_cmd"] = g7(a);
        auto g8 = net<std::string (*)(void*)>("bambu_network_build_login_info"); if (g8) r["login_info"] = g8(a);
        auto g9 = net<std::string (*)(void*)>("bambu_network_get_bambulab_host"); if (g9) r["bambulab_host"] = g9(a);
        host_log_json("net.change_user", r);
        return r;
    }

    if (method == "net.is_user_login") { auto f = net<bool (*)(void*)>("bambu_network_is_user_login"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.user_logout") { auto f = net<int (*)(void*, bool)>("bambu_network_user_logout"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("request", false))}} : not_supported(method); }
    if (method == "net.get_user_id") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_id"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_name") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_name"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_avatar") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_avatar"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_nickname") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_nickanme"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_login_cmd") { auto f = net<std::string (*)(void*)>("bambu_network_build_login_cmd"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_logout_cmd") { auto f = net<std::string (*)(void*)>("bambu_network_build_logout_cmd"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_login_info") { auto f = net<std::string (*)(void*)>("bambu_network_build_login_info"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.ping_bind") { auto f = net<int (*)(void*, std::string)>("bambu_network_ping_bind"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("ping_code", std::string()))}} : not_supported(method); }
    if (method == "net.bind_detect") { auto f = net<int (*)(void*, std::string, std::string, detectResult&)>("bambu_network_bind_detect"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); detectResult det; const int ret = f(a, payload.value("dev_ip", std::string()), payload.value("sec_link", std::string()), det); return {{"ok", true}, {"value", ret}, {"detect", {{"result_msg", det.result_msg}, {"command", det.command}, {"dev_id", det.dev_id}, {"model_id", det.model_id}, {"dev_name", det.dev_name}, {"version", det.version}, {"bind_state", det.bind_state}, {"connect_type", det.connect_type}}}}; }
    if (method == "net.report_consent") { auto f = net<int (*)(void*, std::string)>("bambu_network_report_consent"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("expand", std::string()))}} : not_supported(method); }
    if (method == "net.bind") {
        auto f = net<int (*)(void*, std::string, std::string, std::string, std::string, bool, OnUpdateStatusFn)>("bambu_network_bind");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "bind";
        register_job(job);
        const int ret = f(a, params.value("dev_ip", std::string()), params.value("dev_id", std::string()), params.value("sec_link", std::string()), params.value("timezone", std::string()), params.value("improved", false), [this, job](int status, int code, std::string msg) {
            queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
        });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.unbind") { auto f = net<int (*)(void*, std::string)>("bambu_network_unbind"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()))}} : not_supported(method); }
    if (method == "net.get_bambulab_host") { auto f = net<std::string (*)(void*)>("bambu_network_get_bambulab_host"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_selected_machine") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_selected_machine"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.set_user_selected_machine") { auto f = net<int (*)(void*, std::string)>("bambu_network_set_user_selected_machine"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()))}} : not_supported(method); }
    if (method == "net.start_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_print";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            [this, job](int status, std::string job_info) {
                queue_event(job->agent_handle, "job.wait", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"job_info", job_info}});
                return !job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }

    if (method == "net.start_local_print_with_record") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_local_print_with_record");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_local_print_with_record";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            [this, job](int status, std::string job_info) {
                queue_event(job->agent_handle, "job.wait", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"job_info", job_info}});
                return !job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_local_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn)>("bambu_network_start_local_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_local_print";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_send_gcode_to_sdcard") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_send_gcode_to_sdcard");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_send_gcode_to_sdcard";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            nullptr);
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_sdcard_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn)>("bambu_network_start_sdcard_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_sdcard_print";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.get_studio_info_url") { auto f = net<std::string (*)(void*)>("bambu_network_get_studio_info_url"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.modify_printer_name") { auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_modify_printer_name"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("dev_name", std::string()))}} : not_supported(method); }
    if (method == "net.get_task_plate_index") { auto f = net<int (*)(void*, std::string, int*)>("bambu_network_get_task_plate_index"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int plate_index = -1; const int ret = f(a, payload.value("task_id", std::string()), &plate_index); return {{"ok", true}, {"value", ret}, {"plate_index", plate_index}}; }
    if (method == "net.get_user_info") { auto f = net<int (*)(void*, int*)>("bambu_network_get_user_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int identifier = 0; const int ret = f(a, &identifier); return {{"ok", true}, {"value", ret}, {"identifier", identifier}}; }
    if (method == "net.request_bind_ticket") { auto f = net<int (*)(void*, std::string*)>("bambu_network_request_bind_ticket"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string ticket; const int ret = f(a, &ticket); return {{"ok", true}, {"value", ret}, {"ticket", ticket}}; }
    if (method == "net.query_bind_status") { auto f = net<int (*)(void*, std::vector<std::string>, unsigned int*, std::string*)>("bambu_network_query_bind_status"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("query_list", std::vector<std::string>()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_printer_firmware") { auto f = net<int (*)(void*, std::string, unsigned*, std::string*)>("bambu_network_get_printer_firmware"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned http_code = 0; std::string http_body; const int ret = f(a, payload.value("dev_id", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_my_profile") { auto f = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_profile"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("token", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.request_setting_id") { auto f = net<std::string (*)(void*, std::string, std::map<std::string, std::string>*, unsigned int*)>("bambu_network_request_setting_id"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto values = json_to_string_map(payload.value("values", nlohmann::json::object())); unsigned int http_code = 0; std::string setting_id = f(a, payload.value("name", std::string()), &values, &http_code); return {{"ok", true}, {"value", 0}, {"setting_id", setting_id}, {"http_code", http_code}}; }
    if (method == "net.get_user_presets") { auto f = net<int (*)(void*, std::map<std::string, std::map<std::string, std::string>>*)>("bambu_network_get_user_presets"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::map<std::string, std::map<std::string, std::string>> user_presets; const int ret = f(a, &user_presets); return {{"ok", true}, {"value", ret}, {"user_presets", nested_string_map_to_json(user_presets)}}; }
    if (method == "net.get_setting_list") { auto f = net<int (*)(void*, std::string, ProgressFn, WasCancelledFn)>("bambu_network_get_setting_list"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); const auto job_id = payload.value("client_job_id", 0LL); const auto params = payload.value("params", nlohmann::json::object()); auto job = std::make_shared<HostJobState>(); job->job_id = job_id; job->agent_handle = agent_id; job->kind = "get_setting_list"; register_job(job); const int ret = f(a, params.value("bundle_version", std::string()), [this, job](int progress) { queue_event(job->agent_handle, "job.progress", {{"job_id", job->job_id}, {"kind", job->kind}, {"progress", progress}}); }, [job]() { return job->cancel_requested.load(); }); unregister_job(job_id); return {{"ok", true}, {"value", ret}, {"job_id", job_id}}; }
    if (method == "net.get_setting_list2") { auto f = net<int (*)(void*, std::string, CheckFn, ProgressFn, WasCancelledFn)>("bambu_network_get_setting_list2"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); const auto job_id = payload.value("client_job_id", 0LL); const auto params = payload.value("params", nlohmann::json::object()); auto job = std::make_shared<HostJobState>(); job->job_id = job_id; job->agent_handle = agent_id; job->kind = "get_setting_list2"; register_job(job); const int ret = f(a, params.value("bundle_version", std::string()), [this, job](std::map<std::string, std::string> info) { const auto request_id = m_next_wait_request.fetch_add(1); { std::lock_guard<std::mutex> lock(job->wait_mutex); job->wait_request_id = request_id; job->wait_reply_ready = false; job->wait_reply_value = true; } queue_event(job->agent_handle, "job.check", {{"job_id", job->job_id}, {"kind", job->kind}, {"request_id", request_id}, {"info", info}}); std::unique_lock<std::mutex> lock(job->wait_mutex); job->wait_cv.wait(lock, [&] { return job->wait_reply_ready && job->wait_request_id == request_id; }); return job->wait_reply_value; }, [this, job](int progress) { queue_event(job->agent_handle, "job.progress", {{"job_id", job->job_id}, {"kind", job->kind}, {"progress", progress}}); }, [job]() { return job->cancel_requested.load(); }); unregister_job(job_id); return {{"ok", true}, {"value", ret}, {"job_id", job_id}}; }
    if (method == "net.put_setting") { auto f = net<int (*)(void*, std::string, std::string, std::map<std::string, std::string>*, unsigned int*)>("bambu_network_put_setting"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto values = json_to_string_map(payload.value("values", nlohmann::json::object())); unsigned int http_code = 0; const int ret = f(a, payload.value("setting_id", std::string()), payload.value("name", std::string()), &values, &http_code); return {{"ok", true}, {"value", ret}, {"http_code", http_code}}; }
    if (method == "net.delete_setting") { auto f = net<int (*)(void*, std::string)>("bambu_network_delete_setting"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("setting_id", std::string()))}} : not_supported(method); }
    if (method == "net.set_extra_http_header") { auto f = net<int (*)(void*, std::map<std::string, std::string>)>("bambu_network_set_extra_http_header"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto headers = json_to_string_map(payload.value("headers", nlohmann::json::object())); const int ret = f(a, headers); nlohmann::json r{{"ok", true}, {"value", ret}, {"headers", headers}}; host_log_json("net.set_extra_http_header", r); return r; }
    if (method == "net.get_my_message") { auto f = net<int (*)(void*, int, int, int, unsigned int*, std::string*)>("bambu_network_get_my_message"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("type", 0), payload.value("after", 0), payload.value("limit", 20), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.check_user_task_report") { auto f = net<int (*)(void*, int*, bool*)>("bambu_network_check_user_task_report"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int task_id = 0; bool printable = false; const int ret = f(a, &task_id, &printable); return {{"ok", true}, {"value", ret}, {"task_id", task_id}, {"printable", printable}}; }
    if (method == "net.get_user_print_info") { auto f = net<int (*)(void*, unsigned int*, std::string*)>("bambu_network_get_user_print_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_user_tasks") { auto f = net<int (*)(void*, TaskQueryParams, std::string*)>("bambu_network_get_user_tasks"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto params = task_query_from_json(payload.value("params", nlohmann::json::object())); std::string http_body; const int ret = f(a, params, &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.get_subtask_info") { auto f = net<int (*)(void*, std::string, std::string*, unsigned int*, std::string*)>("bambu_network_get_subtask_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string task_json; unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("subtask_id", std::string()), &task_json, &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"task_json", task_json}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_slice_info") { auto f = net<int (*)(void*, std::string, std::string, int, std::string*)>("bambu_network_get_slice_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string slice_json; const int ret = f(a, payload.value("project_id", std::string()), payload.value("profile_id", std::string()), payload.value("plate_index", 0), &slice_json); return {{"ok", true}, {"value", ret}, {"slice_json", slice_json}}; }
    if (method == "net.get_camera_url") { auto f = net<int (*)(void*, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("dev_id", std::string()), cb); }); }
    if (method == "net.get_camera_url_for_golive") { auto f = net<int (*)(void*, std::string, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url_for_golive"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("dev_id", std::string()), payload.value("sdev_id", std::string()), cb); }); }
    if (method == "net.get_design_staffpick") { auto f = net<int (*)(void*, int, int, std::function<void(std::string)>)>("bambu_network_get_design_staffpick"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("offset", 0), payload.value("limit", 0), cb); }); }
    if (method == "net.get_model_publish_url") { auto f = net<int (*)(void*, std::string*)>("bambu_network_get_model_publish_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_model_mall_home_url") { auto f = net<int (*)(void*, std::string*)>("bambu_network_get_model_mall_home_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_model_mall_detail_url") { auto f = net<int (*)(void*, std::string*, std::string)>("bambu_network_get_model_mall_detail_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url, payload.value("id", std::string())); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_subtask") { auto f = net<int (*)(void*, Slic3r::BBLModelTask*, OnGetSubTaskFn)>("bambu_network_get_subtask"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); Slic3r::BBLModelTask task{}; if (payload.contains("task") && payload["task"].is_object()) json_to_model_task(payload["task"], task); return wait_model_task_callback([&](auto cb) { return f(a, &task, cb); }); }
    if (method == "net.put_model_mall_rating") { auto f = net<int (*)(void*, int, int, std::string, std::vector<std::string>, unsigned int&, std::string&)>("bambu_network_put_model_mall_rating"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_error; const int ret = f(a, payload.value("rating_id", 0), payload.value("score", 0), payload.value("content", std::string()), windows_paths_to_wsl(payload.value("images", std::vector<std::string>())), http_code, http_error); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_oss_config") { auto f = net<int (*)(void*, std::string&, std::string, unsigned int&, std::string&)>("bambu_network_get_oss_config"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string config; unsigned int http_code = 0; std::string http_error; const int ret = f(a, config, payload.value("country_code", std::string()), http_code, http_error); return {{"ok", true}, {"value", ret}, {"config", config}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.put_rating_picture_oss") { auto f = net<int (*)(void*, std::string&, std::string&, std::string, int, unsigned int&, std::string&)>("bambu_network_put_rating_picture_oss"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string config = payload.value("config", std::string()); std::string pic_oss_path = windows_path_to_wsl(payload.value("pic_oss_path", std::string())); unsigned int http_code = 0; std::string http_error; const int ret = f(a, config, pic_oss_path, payload.value("model_id", std::string()), payload.value("profile_id", 0), http_code, http_error); return {{"ok", true}, {"value", ret}, {"config", config}, {"pic_oss_path", pic_oss_path}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_model_mall_rating") { auto f = net<int (*)(void*, int, std::string&, unsigned int&, std::string&)>("bambu_network_get_model_mall_rating"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string rating_result; unsigned int http_code = 0; std::string http_error; const int ret = f(a, payload.value("job_id", 0), rating_result, http_code, http_error); return {{"ok", true}, {"value", ret}, {"rating_result", rating_result}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_mw_user_preference") { auto f = net<int (*)(void*, std::function<void(std::string)>)>("bambu_network_get_mw_user_preference"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, cb); }); }
    if (method == "net.get_mw_user_4ulist") { auto f = net<int (*)(void*, int, int, std::function<void(std::string)>)>("bambu_network_get_mw_user_4ulist"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("seed", 0), payload.value("limit", 0), cb); }); }
    if (method == "net.get_hms_snapshot") { auto f = net<int (*)(void*, std::string, std::string, std::function<void(std::string, int)>)>("bambu_network_get_hms_snapshot"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_int_callback([&](auto cb) { return f(a, payload.value("dev_id", std::string()), windows_path_to_wsl(payload.value("file_name", std::string())), cb); }); }

    if (method == "net.get_my_token") { auto f = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_token"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("ticket", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }

    if (method == "net.track_enable") { auto f = net<int (*)(void*, bool)>("bambu_network_track_enable"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("enable", false))}} : not_supported(method); }
    if (method == "net.track_remove_files") { auto f = net<int (*)(void*)>("bambu_network_track_remove_files"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.track_event") { auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_track_event"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("evt_key", std::string()), payload.value("content", std::string()))}} : not_supported(method); }
    if (method == "net.track_header") { auto f = net<int (*)(void*, std::string)>("bambu_network_track_header"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("header", std::string()))}} : not_supported(method); }
    if (method == "net.track_update_property") { auto f = net<int (*)(void*, std::string, std::string, std::string)>("bambu_network_track_update_property"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("name", std::string()), payload.value("value", std::string()), payload.value("type", std::string()))}} : not_supported(method); }
    if (method == "net.track_get_property") { auto f = net<int (*)(void*, std::string, std::string&, std::string)>("bambu_network_track_get_property"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string value; const int ret = f(a, payload.value("name", std::string()), value, payload.value("type", std::string())); return {{"ok", true}, {"value", ret}, {"property_value", value}}; }

    if (method == "ft.capabilities") {
        return {
            {"ok", true},
            {"ft_abi_version", has_network_symbol("ft_abi_version")},
            {"ft_tunnel_create", has_network_symbol("ft_tunnel_create")},
            {"ft_tunnel_sync_connect", has_network_symbol("ft_tunnel_sync_connect")},
            {"ft_tunnel_release", has_network_symbol("ft_tunnel_release")},
            {"ft_tunnel_shutdown", has_network_symbol("ft_tunnel_shutdown")},
            {"ft_job_create", has_network_symbol("ft_job_create")},
            {"ft_job_release", has_network_symbol("ft_job_release")},
            {"ft_job_set_result_cb", has_network_symbol("ft_job_set_result_cb")},
            {"ft_job_get_result", has_network_symbol("ft_job_get_result")},
            {"ft_tunnel_start_job", has_network_symbol("ft_tunnel_start_job")},
            {"ft_job_cancel", has_network_symbol("ft_job_cancel")},
            {"ft_job_set_msg_cb", has_network_symbol("ft_job_set_msg_cb")},
            {"ft_job_try_get_msg", has_network_symbol("ft_job_try_get_msg")},
            {"ft_job_get_msg", has_network_symbol("ft_job_get_msg")}
        };
    }
    if (method == "ft.tunnel_create") {
        auto f = net<fn_ft_tunnel_create>("ft_tunnel_create");
        if (!f) return not_supported(method);
        FT_TunnelHandle* tunnel = nullptr;
        const int ret = static_cast<int>(f(payload.value("url", std::string()).c_str(), &tunnel));
        if (ret != 0 || !tunnel)
            return {{"ok", true}, {"value", ret}, {"tunnel", 0}};
        const auto id = m_next_ft_tunnel++;
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_ft_tunnels[id] = tunnel; }
        return {{"ok", true}, {"value", ret}, {"tunnel", id}};
    }
    if (method == "ft.tunnel_sync_connect") {
        auto f = net<fn_ft_tunnel_sync_connect>("ft_tunnel_sync_connect");
        auto t = lookup_ft_tunnel();
        return f && t ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t))}} : not_supported(method);
    }
    if (method == "ft.tunnel_shutdown") {
        auto f = net<fn_ft_tunnel_shutdown>("ft_tunnel_shutdown");
        auto t = lookup_ft_tunnel();
        return f && t ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t))}} : not_supported(method);
    }
    if (method == "ft.tunnel_release") {
        auto f = net<fn_ft_tunnel_release>("ft_tunnel_release");
        if (!f) return not_supported(method);
        const auto id = payload.value("tunnel", 0LL);
        FT_TunnelHandle* tunnel = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_ft_tunnels.find(id);
            if (it != m_ft_tunnels.end()) {
                tunnel = static_cast<FT_TunnelHandle*>(it->second);
                m_ft_tunnels.erase(it);
            }
        }
        if (!tunnel) return {{"ok", false}, {"error", "tunnel not found"}};
        f(tunnel);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "ft.job_create") {
        auto f = net<fn_ft_job_create>("ft_job_create");
        auto set_result_cb = net<fn_ft_job_set_result_cb>("ft_job_set_result_cb");
        auto set_msg_cb = net<fn_ft_job_set_msg_cb>("ft_job_set_msg_cb");
        if (!f) return not_supported(method);
        FT_JobHandle* job = nullptr;
        const int ret = static_cast<int>(f(payload.value("params_json", std::string()).c_str(), &job));
        if (ret != 0 || !job)
            return {{"ok", true}, {"value", ret}, {"job", 0}};
        const auto id = m_next_ft_job++;
        auto state = std::make_shared<HostFtJobState>();
        state->handle = job;
        if (set_result_cb) {
            set_result_cb(job, [](void* user, ft_job_result result) {
                auto* state = static_cast<HostFtJobState*>(user);
                if (!state)
                    return;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    copy_ft_job_result_payload(*state, result);
                    state->result_ready = true;
                }
                state->cv.notify_all();
            }, state.get());
        }
        if (set_msg_cb) {
            set_msg_cb(job, [](void* user, ft_job_msg msg) {
                auto* state = static_cast<HostFtJobState*>(user);
                if (!state)
                    return;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->messages.emplace_back(msg.kind, std::string(msg.json ? msg.json : ""));
                }
                state->cv.notify_all();
            }, state.get());
        }
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_ft_jobs[id] = job;
            m_ft_job_states[id] = state;
        }
        return {{"ok", true}, {"value", ret}, {"job", id}};
    }
    if (method == "ft.job_start") {
        auto f = net<fn_ft_tunnel_start_job>("ft_tunnel_start_job");
        auto t = lookup_ft_tunnel();
        auto j = lookup_ft_job();
        return f && t && j ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t, j))}} : not_supported(method);
    }
    if (method == "ft.job_cancel") {
        auto f = net<fn_ft_job_cancel>("ft_job_cancel");
        auto j = lookup_ft_job();
        return f && j ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(j))}} : not_supported(method);
    }
    if (method == "ft.job_get_result") {
        auto f = net<fn_ft_job_get_result>("ft_job_get_result");
        auto free_result = net<fn_ft_job_result_destroy>("ft_job_result_destroy");
        auto free_mem = net<fn_ft_free>("ft_free");
        const auto job_id = payload.value("job", 0LL);
        auto j = lookup_ft_job();
        auto state = lookup_ft_job_state(m_ft_job_states, m_state_mutex, job_id);
        if (!j) return not_supported(method);
        const auto timeout_ms = payload.value("timeout_ms", 0U);
        if (state) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->result_ready) {
                if (timeout_ms == 0)
                    state->cv.wait(lock, [&state] { return state->result_ready; });
                else
                    state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state] { return state->result_ready; });
            }
            if (state->result_ready) {
                nlohmann::json out{{"ok", true}, {"value", 0}, {"ec", state->result_ec}, {"resp_ec", state->result_resp_ec}, {"json", state->result_json}};
                if (!state->result_bin.empty()) {
                    g_thread_reply_binary = state->result_bin;
                    out["binary_size"] = state->result_bin.size();
                    out["__binary_pending"] = true;
                } else {
                    clear_thread_reply_binary();
                    out["binary_size"] = 0;
                }
                return out;
            }
            clear_thread_reply_binary();
            return {{"ok", true}, {"value", -4}, {"binary_size", 0}};
        }
        if (!f) return not_supported(method);
        ft_job_result result{};
        const int ret = static_cast<int>(f(j, timeout_ms, &result));
        nlohmann::json out{{"ok", true}, {"value", ret}};
        if (ret == 0) {
            out["ec"] = result.ec;
            out["resp_ec"] = result.resp_ec;
            out["json"] = std::string(result.json ? result.json : "");
            if (result.bin && result.bin_size) {
                g_thread_reply_binary.assign(static_cast<const unsigned char*>(result.bin), static_cast<const unsigned char*>(result.bin) + result.bin_size);
                out["binary_size"] = result.bin_size;
                out["__binary_pending"] = true;
            } else {
                clear_thread_reply_binary();
                out["binary_size"] = 0;
            }
            if (free_result) free_result(&result);
            else if (free_mem) {
                if (result.json) free_mem((void*) result.json);
                if (result.bin) free_mem((void*) result.bin);
            }
        }
        return out;
    }
    if (method == "ft.job_try_get_msg" || method == "ft.job_get_msg") {
        auto f_try = net<fn_ft_job_try_get_msg>("ft_job_try_get_msg");
        auto f_get = net<fn_ft_job_get_msg>("ft_job_get_msg");
        auto free_msg = net<fn_ft_job_msg_destroy>("ft_job_msg_destroy");
        auto free_mem = net<fn_ft_free>("ft_free");
        const auto job_id = payload.value("job", 0LL);
        auto j = lookup_ft_job();
        auto state = lookup_ft_job_state(m_ft_job_states, m_state_mutex, job_id);
        if (!j) return not_supported(method);
        if (state) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (method == "ft.job_get_msg" && state->messages.empty() && !state->result_ready) {
                const auto timeout_ms = payload.value("timeout_ms", 0U);
                if (timeout_ms == 0)
                    state->cv.wait(lock, [&state] { return !state->messages.empty() || state->result_ready; });
                else
                    state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state] { return !state->messages.empty() || state->result_ready; });
            }
            if (!state->messages.empty()) {
                auto msg = std::move(state->messages.front());
                state->messages.pop_front();
                return {{"ok", true}, {"value", 0}, {"kind", msg.first}, {"json", msg.second}};
            }
            if (state->result_ready)
                return {{"ok", true}, {"value", -2}};
        }
        ft_job_msg msg{};
        int ret = -1;
        if (method == "ft.job_try_get_msg") {
            if (!f_try) return not_supported(method);
            ret = static_cast<int>(f_try(j, &msg));
        } else {
            if (!f_get) return not_supported(method);
            ret = static_cast<int>(f_get(j, payload.value("timeout_ms", 0U), &msg));
        }
        nlohmann::json out{{"ok", true}, {"value", ret}};
        if (ret == 0) {
            out["kind"] = msg.kind;
            out["json"] = std::string(msg.json ? msg.json : "");
            if (free_msg) free_msg(&msg);
            else if (free_mem && msg.json) free_mem((void*) msg.json);
        }
        return out;
    }
    if (method == "ft.job_release") {
        auto f = net<fn_ft_job_release>("ft_job_release");
        if (!f) return not_supported(method);
        const auto id = payload.value("job", 0LL);
        FT_JobHandle* job = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_ft_jobs.find(id);
            if (it != m_ft_jobs.end()) {
                job = static_cast<FT_JobHandle*>(it->second);
                m_ft_jobs.erase(it);
            }
            m_ft_job_states.erase(id);
        }
        if (!job) return {{"ok", false}, {"error", "job not found"}};
        f(job);
        return {{"ok", true}, {"value", 0}};
    }

    if (method == "src.init") { auto f = src<int (*)()>("Bambu_Init"); return f ? nlohmann::json{{"ok", true}, {"value", f()}} : nlohmann::json{{"ok", true}, {"value", 0}}; }
    if (method == "src.deinit") { auto f = src<void (*)()>("Bambu_Deinit"); if (f) f(); return {{"ok", true}, {"value", 0}}; }
    if (method == "src.get_last_error_msg") { auto f = src<const char* (*)()>("Bambu_GetLastErrorMsg"); return f ? nlohmann::json{{"ok", true}, {"message", std::string(f() ? f() : "")}} : nlohmann::json{{"ok", true}, {"message", std::string()}}; }
    if (method == "src.free_log_msg") { return {{"ok", true}, {"value", 0}}; }
    if (method == "src.create") {
        auto f = src<int (*)(Bambu_Tunnel*, const char*)>("Bambu_Create");
        if (!f) return not_supported(method);
        Bambu_Tunnel tunnel = nullptr;
        const std::string path = windows_path_to_wsl(payload.value("path", std::string()));
        const int ret = f(&tunnel, path.c_str());
        if (ret != 0)
            return {{"ok", true}, {"value", ret}, {"tunnel", 0}};
        const auto id = m_next_tunnel++;
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_tunnels[id] = tunnel; }
        return {{"ok", true}, {"value", ret}, {"tunnel", id}};
    }
    if (method == "src.open") { auto f = src<int (*)(Bambu_Tunnel)>("Bambu_Open"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t)}} : not_supported(method); }
    if (method == "src.start_stream") { auto f = src<int (*)(Bambu_Tunnel, bool)>("Bambu_StartStream"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t, payload.value("video", false))}} : not_supported(method); }
    if (method == "src.start_stream_ex") { auto f = src<int (*)(Bambu_Tunnel, int)>("Bambu_StartStreamEx"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t, payload.value("type", 0))}} : not_supported(method); }
    if (method == "src.get_stream_count") { auto f = src<int (*)(Bambu_Tunnel)>("Bambu_GetStreamCount"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t)}} : not_supported(method); }
    if (method == "src.get_stream_info") { auto f = src<int (*)(Bambu_Tunnel, int, Bambu_StreamInfo*)>("Bambu_GetStreamInfo"); auto t = lookup_tunnel(); if (!f || !t) return not_supported(method); Bambu_StreamInfo info{}; const int ret = f(t, payload.value("index", 0), &info); nlohmann::json out{{"ok", true}, {"value", ret}}; if (ret == 0) { nlohmann::json ji{{"type", info.type}, {"sub_type", info.sub_type}, {"format_type", info.format_type}, {"format_size", info.format_size}, {"max_frame_size", info.max_frame_size}, {"format_buffer", info.format_buffer && info.format_size > 0 ? std::string(reinterpret_cast<const char*>(info.format_buffer), info.format_size) : std::string()}}; if (info.type == VIDE) ji.update({{"width", info.format.video.width}, {"height", info.format.video.height}, {"frame_rate", info.format.video.frame_rate}}); else ji.update({{"sample_rate", info.format.audio.sample_rate}, {"channel_count", info.format.audio.channel_count}, {"sample_size", info.format.audio.sample_size}}); out["info"] = ji; } return out; }
    if (method == "src.get_duration") { auto f = src<unsigned long (*)(Bambu_Tunnel)>("Bambu_GetDuration"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t)}} : not_supported(method); }
    if (method == "src.seek") { auto f = src<int (*)(Bambu_Tunnel, unsigned long)>("Bambu_Seek"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t, payload.value("time", 0UL))}} : not_supported(method); }
    if (method == "src.send_message") {
        auto f = src<int (*)(Bambu_Tunnel, int, const char*, int)>("Bambu_SendMessage");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        std::string fallback = payload.value("data", std::string());
        const char* data_ptr = fallback.c_str();
        int data_len = static_cast<int>(fallback.size());

        if (payload.value("__binary_request", false)) {
            data_ptr = reinterpret_cast<const char*>(g_thread_request_binary.data());
            data_len = static_cast<int>(g_thread_request_binary.size());
        }

        const int ret = f(t, payload.value("ctrl", 0), data_ptr, data_len);
        g_thread_request_binary.clear();
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.recv_message") {
        auto f = src<int (*)(Bambu_Tunnel, int*, char*, int*)>("Bambu_RecvMessage");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        int ctrl = 0;
        int len = payload.value("buffer_size", 65536);
        std::vector<unsigned char> buffer(static_cast<std::size_t>(len > 0 ? len : 0), 0);
        char* buffer_ptr = buffer.empty() ? nullptr : reinterpret_cast<char*>(buffer.data());

        const int ret = f(t, &ctrl, buffer_ptr, &len);
        nlohmann::json out{{"ok", true}, {"value", ret}, {"ctrl", ctrl}};

        if (ret == 0 && len >= 0) {
            out["message_len"] = len;
            if (len > 0) {
                g_thread_reply_binary.assign(buffer.begin(), buffer.begin() + static_cast<std::size_t>(len));
                out["__binary_pending"] = true;
            }
        } else {
            out["required_len"] = len;
        }

        return out;
    }
    if (method == "src.read_sample") {
        auto f = src<int (*)(Bambu_Tunnel, Bambu_Sample*)>("Bambu_ReadSample");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        Bambu_Sample sample{};
        const int ret = f(t, &sample);
        nlohmann::json j{{"ok", true}, {"value", ret}};
        if (ret == 0) {
            j["sample"] = {{"itrack", sample.itrack}, {"size", sample.size}, {"flags", sample.flags}, {"decode_time", sample.decode_time}};
            if (sample.buffer && sample.size > 0) {
                g_thread_reply_binary.assign(sample.buffer, sample.buffer + static_cast<std::size_t>(sample.size));
                j["__binary_pending"] = true;
            }
        }
        return j;
    }
    if (method == "src.close") { auto f = src<void (*)(Bambu_Tunnel)>("Bambu_Close"); auto t = lookup_tunnel(); if (!f || !t) return not_supported(method); f(t); return {{"ok", true}, {"value", 0}}; }
    if (method == "src.destroy") {
        auto f = src<void (*)(Bambu_Tunnel)>("Bambu_Destroy");
        const auto id = payload.value("tunnel", 0LL);
        Bambu_Tunnel t = nullptr;
        LoggerCallbackContext* logger_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_tunnels.find(id);
            if (it != m_tunnels.end()) {
                t = static_cast<Bambu_Tunnel>(it->second);
                m_tunnels.erase(it);
            }
            auto logger_it = m_logger_contexts.find(id);
            if (logger_it != m_logger_contexts.end()) {
                logger_ctx = static_cast<LoggerCallbackContext*>(logger_it->second);
                m_logger_contexts.erase(logger_it);
            }
        }
        delete logger_ctx;
        if (!f || !t)
            return not_supported(method);
        f(t);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "src.set_logger") {
        auto f = src<void (*)(Bambu_Tunnel, Logger, void*)>("Bambu_SetLogger");
        auto free_f = src<void (*)(tchar const*)>("Bambu_FreeLogMsg");
        auto t = lookup_tunnel();
        const auto tunnel_id = payload.value("tunnel", 0LL);
        if (!f || !t)
            return not_supported(method);

        auto* logger_ctx = new LoggerCallbackContext{this, tunnel_id, free_f};
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_logger_contexts.find(tunnel_id);
            if (it != m_logger_contexts.end()) {
                delete static_cast<LoggerCallbackContext*>(it->second);
                it->second = logger_ctx;
            } else {
                m_logger_contexts.emplace(tunnel_id, logger_ctx);
            }
        }

        f(t, logger_callback_forwarder, logger_ctx);
        return {{"ok", true}, {"value", 0}};
    }

    return not_supported(method);
}

}
