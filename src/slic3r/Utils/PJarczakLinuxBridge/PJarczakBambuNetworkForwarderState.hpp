#pragma once

#include "../bambu_networking.hpp"
#include "../../GUI/Printer/BambuTunnel.h"
#include "PJarczakLinuxBridgeCompat.hpp"

#include <cstdint>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r::PJarczakLinuxBridge {

struct BridgeJobState;

struct CachedSample {
    std::vector<unsigned char> buffer;
    int itrack{0};
    int size{0};
    int flags{0};
    unsigned long long decode_time{0};
};

struct BridgeAgent {
    std::int64_t remote_handle{0};
    std::string log_dir;
    std::string config_dir;
    std::string cert_dir;
    std::string cert_file;
    std::string country_code;
    std::string user_info;
    std::string user_id;
    std::string user_name;
    std::string user_avatar;
    std::string user_nickname;
    std::string login_cmd;
    std::string logout_cmd;
    std::string login_info;
    std::string selected_machine;
    std::string bambulab_host{"https://api.bambulab.com/"};
    std::string studio_info_url;
    std::string track_header;
    std::map<std::string, std::string> track_properties;
    bool started{false};
    bool server_connected{false};
    bool logged_in{false};
    bool tracking_enabled{false};
    bool multi_machine_enabled{false};

    BBL::OnMsgArrivedFn on_ssdp_msg;
    BBL::OnUserLoginFn on_user_login;
    BBL::OnPrinterConnectedFn on_printer_connected;
    BBL::OnServerConnectedFn on_server_connected;
    BBL::OnHttpErrorFn on_http_error;
    BBL::GetCountryCodeFn get_country_code;
    BBL::GetSubscribeFailureFn on_subscribe_failure;
    BBL::OnMessageFn on_message;
    BBL::OnMessageFn on_user_message;
    BBL::OnLocalConnectedFn on_local_connect;
    BBL::OnMessageFn on_local_message;
    BBL::QueueOnMainFn queue_on_main;
    BBL::OnServerErrFn on_server_error;
    std::mutex jobs_mutex;
    std::map<std::int64_t, std::shared_ptr<BridgeJobState>> jobs;
};

struct BridgeJobState {
    std::int64_t job_id{0};
    std::string kind;
    BBL::OnUpdateStatusFn on_update_status;
    BBL::WasCancelledFn was_cancelled;
    BBL::OnWaitFn on_wait;
    BBL::ProgressFn on_progress;
    BBL::CheckFn on_check;
    std::string* out_string{nullptr};
    std::atomic<bool> stop_cancel_watch{false};
    std::thread cancel_watch;
};

struct BridgeTunnel {
    std::int64_t remote_handle{0};
    Logger logger{nullptr};
    void* logger_ctx{nullptr};
    bool opened{false};
    std::string last_error;
    std::deque<CachedSample> sample_queue;
    std::vector<std::vector<unsigned char>> stream_format_buffers;
    std::string recv_message_buffer;
    std::string logger_message_utf8;
#if defined(_WIN32)
    std::wstring logger_message_wide;
#endif
};

BridgeAgent* as_agent(void* handle);
void* new_agent(const std::string& log_dir);
int delete_agent(void* handle);
BridgeTunnel* as_tunnel_handle(void* handle);

void register_remote_agent(BridgeAgent* agent);
void unregister_remote_agent(BridgeAgent* agent);
BridgeAgent* find_remote_agent(std::int64_t remote_handle);
void dispatch_agent_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload);

void register_remote_tunnel(BridgeTunnel* tunnel);
void unregister_remote_tunnel(BridgeTunnel* tunnel);
BridgeTunnel* find_remote_tunnel(std::int64_t remote_handle);
void dispatch_tunnel_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload);

std::shared_ptr<BridgeJobState> register_job_state(BridgeAgent* agent, const std::shared_ptr<BridgeJobState>& job);
std::shared_ptr<BridgeJobState> find_job_state(BridgeAgent* agent, std::int64_t job_id);
void unregister_job_state(BridgeAgent* agent, std::int64_t job_id);

}
