#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <functional>

namespace Slic3r::PJarczakLinuxBridge {

struct HostJobState {
    std::int64_t job_id{0};
    std::int64_t agent_handle{0};
    std::string kind;
    std::atomic<bool> cancel_requested{false};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::int64_t wait_request_id{0};
    bool wait_reply_ready{false};
    bool wait_reply_value{true};
};

struct HostCallbackReplyState {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{false};
    std::string string_value;
};


struct HostFtJobState {
    void* handle{nullptr};
    std::mutex mutex;
    std::condition_variable cv;
    bool result_ready{false};
    int result_ec{0};
    int result_resp_ec{0};
    std::string result_json;
    std::vector<unsigned char> result_bin;
    std::deque<std::pair<int, std::string>> messages;
};

class LinuxPluginHost {
public:
    LinuxPluginHost();
    ~LinuxPluginHost();
    nlohmann::json handle(const std::string& method, const nlohmann::json& payload);
    static void set_thread_request_binary(std::vector<unsigned char> data);
    static bool consume_thread_reply_binary(std::vector<unsigned char>& out);
    void dispatch_logger_event(std::int64_t tunnel_handle, int level, const std::string& message);
    std::string refresh_camera_url_for_ft(const std::string& device, const std::string& dev_ver, const std::string& channel);
    std::string refresh_agora_url_ptr_string() const;

private:
    void load_modules();
    void* resolve_network(const char* name);
    void* resolve_source(const char* name);
    bool has_network_symbol(const char* name);
    nlohmann::json auth_capabilities() const;
    nlohmann::json not_supported(const std::string& method) const;
    void queue_event(std::int64_t agent_handle, const std::string& name, const nlohmann::json& payload);
    void queue_tunnel_event(std::int64_t tunnel_handle, const std::string& name, const nlohmann::json& payload);
    nlohmann::json drain_events(std::size_t limit);
    std::shared_ptr<HostJobState> get_job(std::int64_t job_id);
    void register_job(const std::shared_ptr<HostJobState>& job);
    void unregister_job(std::int64_t job_id);
    void set_job_cancel(std::int64_t job_id, bool value);
    void set_job_wait_reply(std::int64_t job_id, std::int64_t request_id, bool value);
    std::shared_ptr<HostCallbackReplyState> register_callback_request(std::int64_t request_id);
    void unregister_callback_request(std::int64_t request_id);
    void set_callback_reply(std::int64_t request_id, const std::string& value);
    void queue_main_task(std::function<void()> fn);
    void ensure_main_dispatcher();
    void stop_main_dispatcher();
    void main_dispatch_loop();

    template <typename T>
    T net(const char* name)
    {
        return reinterpret_cast<T>(resolve_network(name));
    }

    template <typename T>
    T src(const char* name)
    {
        return reinterpret_cast<T>(resolve_source(name));
    }

    void* m_network{nullptr};
    void* m_source{nullptr};
    std::int64_t m_next_agent{1};
    std::int64_t m_next_tunnel{1};
    std::int64_t m_next_ft_tunnel{1};
    std::int64_t m_next_ft_job{1};
    std::map<std::int64_t, void*> m_agents;
    std::map<std::int64_t, void*> m_tunnels;
    std::map<std::int64_t, void*> m_ft_tunnels;
    std::map<std::int64_t, void*> m_ft_jobs;
    std::map<std::int64_t, std::shared_ptr<HostFtJobState>> m_ft_job_states;
    std::map<std::int64_t, std::string> m_country_codes;
    std::mutex m_state_mutex;
    std::mutex m_events_mutex;
    std::deque<nlohmann::json> m_events;
    std::map<std::int64_t, std::shared_ptr<HostJobState>> m_jobs;
    std::map<std::int64_t, std::shared_ptr<HostCallbackReplyState>> m_callback_replies;
    std::atomic<std::int64_t> m_next_wait_request{1};
    std::atomic<std::int64_t> m_next_callback_request{1};
    std::map<std::int64_t, void*> m_logger_contexts;
    std::mutex m_main_tasks_mutex;
    std::condition_variable m_main_tasks_cv;
    std::deque<std::function<void()>> m_main_tasks;
    std::thread m_main_dispatcher;
    std::atomic<bool> m_stop_main_dispatcher{false};
    std::string m_network_status{"not_loaded"};
    std::string m_network_actual_abi_version;
    std::string m_source_status{"not_loaded"};
};

}
