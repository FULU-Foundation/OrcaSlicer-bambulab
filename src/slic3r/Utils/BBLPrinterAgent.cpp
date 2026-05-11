#include "BBLPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "NetworkAgentFactory.hpp"

#include <boost/log/trivial.hpp>

#include <thread>

namespace Slic3r {

BBLPrinterAgent::BBLPrinterAgent() = default;

BBLPrinterAgent::~BBLPrinterAgent() = default;

int BBLPrinterAgent::invoke_print_request_untracked(LastPrintRequestType type,
                                                    PrintParams params,
                                                    OnUpdateStatusFn update_fn,
                                                    WasCancelledFn cancel_fn,
                                                    OnWaitFn wait_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    if (!agent) {
        return -1;
    }

    switch (type) {
    case LastPrintRequestType::start_print: {
        auto func = plugin.get_start_print();
        if (!func) {
            return -1;
        }
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    case LastPrintRequestType::start_local_print_with_record: {
        auto func = plugin.get_start_local_print_with_record();
        if (!func) {
            return -1;
        }
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_local_print_with_record_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    case LastPrintRequestType::start_local_print: {
        auto func = plugin.get_start_local_print();
        if (!func) {
            return -1;
        }
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_local_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn);
        }
        return func(agent, params, update_fn, cancel_fn);
    }
    case LastPrintRequestType::start_sdcard_print: {
        auto func = plugin.get_start_sdcard_print();
        if (!func) {
            return -1;
        }
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_sdcard_print_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn);
        }
        return func(agent, params, update_fn, cancel_fn);
    }
    default:
        return -1;
    }
}

void BBLPrinterAgent::remember_last_print_request(LastPrintRequestType type,
                                                 const PrintParams& params,
                                                 OnUpdateStatusFn update_fn,
                                                 WasCancelledFn cancel_fn,
                                                 OnWaitFn wait_fn)
{
    std::lock_guard<std::mutex> lock(m_last_print_request_mutex);
    m_last_print_request.type = type;
    m_last_print_request.params = params;
    m_last_print_request.update_fn = update_fn;
    m_last_print_request.cancel_fn = cancel_fn;
    m_last_print_request.wait_fn = wait_fn;
    m_last_print_request.retry_count = 0;
}

bool BBLPrinterAgent::retry_last_print_request(const std::string& dev_id)
{
    LastPrintRequestType type = LastPrintRequestType::none;
    PrintParams params;
    OnUpdateStatusFn update_fn = nullptr;
    WasCancelledFn cancel_fn = nullptr;
    OnWaitFn wait_fn = nullptr;
    int retry_count = 0;

    {
        std::lock_guard<std::mutex> lock(m_last_print_request_mutex);
        if (m_last_print_request.type == LastPrintRequestType::none) {
            return false;
        }
        if (m_last_print_request.retry_count >= 3) {
            return false;
        }
        if (!dev_id.empty() && !m_last_print_request.params.dev_id.empty() && m_last_print_request.params.dev_id != dev_id) {
            return false;
        }

        type = m_last_print_request.type;
        params = m_last_print_request.params;
        update_fn = m_last_print_request.update_fn;
        cancel_fn = m_last_print_request.cancel_fn;
        wait_fn = m_last_print_request.wait_fn;

        m_last_print_request.retry_count++;
        retry_count = m_last_print_request.retry_count;
    }

    if (!update_fn) {
        update_fn = [](int, int, std::string) {};
    }
    if (!cancel_fn) {
        cancel_fn = []() { return false; };
    }
    if (!wait_fn) {
        wait_fn = [](int, std::string) { return true; };
    }

    std::thread([this, type, params, update_fn, cancel_fn, wait_fn, retry_count]() mutable {
        BOOST_LOG_TRIVIAL(info)
            << "auto retry last print request attempt=" << retry_count
            << ", dev_id=" << params.dev_id;

        const int result = invoke_print_request_untracked(type, params, update_fn, cancel_fn, wait_fn);

        BOOST_LOG_TRIVIAL(info)
            << "auto retry last print request result=" << result
            << ", attempt=" << retry_count
            << ", dev_id=" << params.dev_id;
    }).detach();

    return true;
}

void BBLPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    m_cloud_agent = cloud;
    // BBL DLL manages tokens internally, so this is just for interface compliance
}

// ============================================================================
// Communication
// ============================================================================

int BBLPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_send_message_legacy>(func);
            return legacy_func(agent, dev_id, json_str, qos);
        }
        return func(agent, dev_id, json_str, qos, flag);
    }
    return -1;
}

int BBLPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_connect_printer();
    if (func && agent) {
        return func(agent, dev_id, dev_ip, username, password, use_ssl);
    }
    return -1;
}

int BBLPrinterAgent::disconnect_printer()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_disconnect_printer();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

int BBLPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_send_message_to_printer();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_send_message_to_printer_legacy>(func);
            return legacy_func(agent, dev_id, json_str, qos);
        }
        return func(agent, dev_id, json_str, qos, flag);
    }
    return -1;
}

// ============================================================================
// Certificates
// ============================================================================

int BBLPrinterAgent::check_cert()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_check_cert();
    if (func && agent) {
        return func(agent);
    }
    return -1;
}

void BBLPrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_install_device_cert();
    if (func && agent) {
        func(agent, dev_id, lan_only);
    }
}

// ============================================================================
// Discovery
// ============================================================================

bool BBLPrinterAgent::start_discovery(bool start, bool sending)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_discovery();
    if (func && agent) {
        return func(agent, start, sending);
    }
    return false;
}

// ============================================================================
// Binding
// ============================================================================

int BBLPrinterAgent::ping_bind(std::string ping_code)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_ping_bind();
    if (func && agent) {
        return func(agent, ping_code);
    }
    return -1;
}

int BBLPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_bind_detect();
    if (func && agent) {
        return func(agent, dev_ip, sec_link, detect);
    }
    return -1;
}

int BBLPrinterAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_bind();
    if (func && agent) {
        return func(agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
    }
    return -1;
}

int BBLPrinterAgent::unbind(std::string dev_id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_unbind();
    if (func && agent) {
        return func(agent, dev_id);
    }
    return -1;
}

int BBLPrinterAgent::request_bind_ticket(std::string* ticket)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_request_bind_ticket();
    if (func && agent) {
        return func(agent, ticket);
    }
    return -1;
}

int BBLPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_server_callback();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string BBLPrinterAgent::get_user_selected_machine()
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_get_user_selected_machine();
    if (func && agent) {
        return func(agent);
    }
    return "";
}

int BBLPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_user_selected_machine();
    if (func && agent) {
        return func(agent, dev_id);
    }
    return -1;
}

// ============================================================================
// Agent Information
// ============================================================================
AgentInfo BBLPrinterAgent::get_agent_info_static()
{
    return AgentInfo{BBL_PRINTER_AGENT_ID, "Bambu Lab", "", "Bambu Lab printer agent"};
}

// ============================================================================
// Print Job Operations
// ============================================================================

int BBLPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    const int result = invoke_print_request_untracked(LastPrintRequestType::start_print, params, update_fn, cancel_fn, wait_fn);
    if (result == 0) {
        remember_last_print_request(LastPrintRequestType::start_print, params, update_fn, cancel_fn, wait_fn);
    }
    return result;
}

int BBLPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    const int result = invoke_print_request_untracked(LastPrintRequestType::start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    if (result == 0) {
        remember_last_print_request(LastPrintRequestType::start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    }
    return result;
}

int BBLPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_start_send_gcode_to_sdcard();
    if (func && agent) {
        if (plugin.use_legacy_network()) {
            auto legacy_func = reinterpret_cast<func_start_send_gcode_to_sdcard_legacy>(func);
            auto legacy_params = BBLNetworkPlugin::as_legacy(params);
            return legacy_func(agent, legacy_params, update_fn, cancel_fn, wait_fn);
        }
        return func(agent, params, update_fn, cancel_fn, wait_fn);
    }
    return -1;
}

int BBLPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    const int result = invoke_print_request_untracked(LastPrintRequestType::start_local_print, params, update_fn, cancel_fn, nullptr);
    if (result == 0) {
        remember_last_print_request(LastPrintRequestType::start_local_print, params, update_fn, cancel_fn, nullptr);
    }
    return result;
}

int BBLPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    const int result = invoke_print_request_untracked(LastPrintRequestType::start_sdcard_print, params, update_fn, cancel_fn, nullptr);
    if (result == 0) {
        remember_last_print_request(LastPrintRequestType::start_sdcard_print, params, update_fn, cancel_fn, nullptr);
    }
    return result;
}

// ============================================================================
// Callbacks
// ============================================================================

int BBLPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_ssdp_msg_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_printer_connected_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_subscribe_failure_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_user_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_local_connect_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_on_local_message_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

int BBLPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    auto& plugin = BBLNetworkPlugin::instance();
    auto agent = plugin.get_agent();
    auto func = plugin.get_set_queue_on_main_fn();
    if (func && agent) {
        return func(agent, fn);
    }
    return -1;
}

// ============================================================================
// Filament Operations
// ============================================================================

FilamentSyncMode BBLPrinterAgent::get_filament_sync_mode() const
{
    // BBL uses MQTT subscription for real-time filament updates
    return FilamentSyncMode::subscription;
}

} // namespace Slic3r
