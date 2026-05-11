#include "BridgeCoreMethodManifest.hpp"

namespace PJarczak::LinuxPluginBridgeCore {

const std::vector<MethodManifestEntry>& method_manifest()
{
    static const std::vector<MethodManifestEntry> entries = {
MethodManifestEntry{"bambu_network_check_debug_consistent", "bambu_network_check_debug_consistent", "network", "implemented", "stable", "local bridge always reports debug consistency"},
MethodManifestEntry{"bambu_network_get_version", "bambu_network_get_version", "network", "implemented", "stable", "returns bridge version"},
MethodManifestEntry{"bambu_network_create_agent", "bambu_network_create_agent", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_destroy_agent", "bambu_network_destroy_agent", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_init_log", "bambu_network_init_log", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_config_dir", "bambu_network_set_config_dir", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_cert_file", "bambu_network_set_cert_file", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_country_code", "bambu_network_set_country_code", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_start", "bambu_network_start", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_on_ssdp_msg_fn", "bambu_network_set_on_ssdp_msg_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_user_login_fn", "bambu_network_set_on_user_login_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_printer_connected_fn", "bambu_network_set_on_printer_connected_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_server_connected_fn", "bambu_network_set_on_server_connected_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_http_error_fn", "bambu_network_set_on_http_error_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_get_country_code_fn", "bambu_network_set_get_country_code_fn", "callbacks", "implemented", "bridge-wired", "callback bridged through request/reply RPC to local getter"},
MethodManifestEntry{"bambu_network_set_on_subscribe_failure_fn", "bambu_network_set_on_subscribe_failure_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_message_fn", "bambu_network_set_on_message_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_user_message_fn", "bambu_network_set_on_user_message_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_local_connect_fn", "bambu_network_set_on_local_connect_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_on_local_message_fn", "bambu_network_set_on_local_message_fn", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_set_queue_on_main_fn", "bambu_network_set_queue_on_main_fn", "callbacks", "implemented", "bridge-wired", "client-side queue_on_main dispatch retained and host callback path registered"},
MethodManifestEntry{"bambu_network_connect_server", "bambu_network_connect_server", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_is_server_connected", "bambu_network_is_server_connected", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_refresh_connection", "bambu_network_refresh_connection", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_start_subscribe", "bambu_network_start_subscribe", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_stop_subscribe", "bambu_network_stop_subscribe", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_add_subscribe", "bambu_network_add_subscribe", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_del_subscribe", "bambu_network_del_subscribe", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_enable_multi_machine", "bambu_network_enable_multi_machine", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_send_message", "bambu_network_send_message", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_connect_printer", "bambu_network_connect_printer", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_disconnect_printer", "bambu_network_disconnect_printer", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_send_message_to_printer", "bambu_network_send_message_to_printer", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_update_cert", "bambu_network_update_cert", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_install_device_cert", "bambu_network_install_device_cert", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_start_discovery", "bambu_network_start_discovery", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_change_user", "bambu_network_change_user", "network", "implemented", "stable", "rpc passthrough plus cache refresh"},
MethodManifestEntry{"bambu_network_is_user_login", "bambu_network_is_user_login", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_user_logout", "bambu_network_user_logout", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_user_id", "bambu_network_get_user_id", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_user_name", "bambu_network_get_user_name", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_user_avatar", "bambu_network_get_user_avatar", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_user_nickanme", "bambu_network_get_user_nickanme", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_build_login_cmd", "bambu_network_build_login_cmd", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_build_logout_cmd", "bambu_network_build_logout_cmd", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_build_login_info", "bambu_network_build_login_info", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_ping_bind", "bambu_network_ping_bind", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_bind_detect", "bambu_network_bind_detect", "jobs", "implemented", "experimental", "detectResult struct mapping not done"},
MethodManifestEntry{"bambu_network_report_consent", "bambu_network_report_consent", "jobs", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_server_callback", "bambu_network_set_server_callback", "callbacks", "implemented", "bridge-wired", "host callback fanout wired through event queue and local dispatch"},
MethodManifestEntry{"bambu_network_bind", "bambu_network_bind", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_unbind", "bambu_network_unbind", "jobs", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_bambulab_host", "bambu_network_get_bambulab_host", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_user_selected_machine", "bambu_network_get_user_selected_machine", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_set_user_selected_machine", "bambu_network_set_user_selected_machine", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_start_print", "bambu_network_start_print", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_start_local_print_with_record", "bambu_network_start_local_print_with_record", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_start_send_gcode_to_sdcard", "bambu_network_start_send_gcode_to_sdcard", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_start_local_print", "bambu_network_start_local_print", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_start_sdcard_print", "bambu_network_start_sdcard_print", "jobs", "implemented", "experimental", "phase5: bridged through host job runtime with callback and cancel plumbing"},
MethodManifestEntry{"bambu_network_get_user_presets", "bambu_network_get_user_presets", "network", "implemented", "stable", "nested map marshaled via json"},
MethodManifestEntry{"bambu_network_request_setting_id", "bambu_network_request_setting_id", "network", "implemented", "stable", "map and http code bridged via rpc"},
MethodManifestEntry{"bambu_network_put_setting", "bambu_network_put_setting", "network", "implemented", "stable", "map and http code bridged via rpc"},
MethodManifestEntry{"bambu_network_get_setting_list", "bambu_network_get_setting_list", "jobs", "implemented", "experimental", "progress callback not bridged"},
MethodManifestEntry{"bambu_network_get_setting_list2", "bambu_network_get_setting_list2", "jobs", "implemented", "experimental", "check, progress and cancel callbacks not bridged"},
MethodManifestEntry{"bambu_network_delete_setting", "bambu_network_delete_setting", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_studio_info_url", "bambu_network_get_studio_info_url", "network", "implemented", "stable", "string return bridged"},
MethodManifestEntry{"bambu_network_set_extra_http_header", "bambu_network_set_extra_http_header", "network", "implemented", "stable", "map marshaling bridged"},
MethodManifestEntry{"bambu_network_get_my_message", "bambu_network_get_my_message", "network", "implemented", "stable", "http code/body out params bridged"},
MethodManifestEntry{"bambu_network_check_user_task_report", "bambu_network_check_user_task_report", "network", "implemented", "stable", "task_id and printable out params bridged"},
MethodManifestEntry{"bambu_network_get_user_print_info", "bambu_network_get_user_print_info", "network", "implemented", "stable", "http code/body out params bridged"},
MethodManifestEntry{"bambu_network_get_user_tasks", "bambu_network_get_user_tasks", "network", "implemented", "stable", "TaskQueryParams and body bridged"},
MethodManifestEntry{"bambu_network_get_printer_firmware", "bambu_network_get_printer_firmware", "network", "implemented", "stable", "http code/body out params bridged"},
MethodManifestEntry{"bambu_network_get_task_plate_index", "bambu_network_get_task_plate_index", "network", "implemented", "stable", "plate index out param bridged"},
MethodManifestEntry{"bambu_network_get_user_info", "bambu_network_get_user_info", "network", "implemented", "stable", "identifier out param bridged"},
MethodManifestEntry{"bambu_network_request_bind_ticket", "bambu_network_request_bind_ticket", "network", "implemented", "stable", "ticket out param bridged"},
MethodManifestEntry{"bambu_network_get_subtask_info", "bambu_network_get_subtask_info", "network", "implemented", "stable", "task_json/http outputs bridged"},
MethodManifestEntry{"bambu_network_get_slice_info", "bambu_network_get_slice_info", "network", "implemented", "stable", "slice_json out param bridged"},
MethodManifestEntry{"bambu_network_query_bind_status", "bambu_network_query_bind_status", "network", "implemented", "stable", "vector and http outputs bridged"},
MethodManifestEntry{"bambu_network_modify_printer_name", "bambu_network_modify_printer_name", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_get_camera_url", "bambu_network_get_camera_url", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"bambu_network_get_camera_url_for_golive", "bambu_network_get_camera_url_for_golive", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"bambu_network_get_design_staffpick", "bambu_network_get_design_staffpick", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"bambu_network_start_publish", "bambu_network_start_publish", "jobs", "implemented", "stable", "job callback bridge with cancel support"},
MethodManifestEntry{"bambu_network_get_model_publish_url", "bambu_network_get_model_publish_url", "network", "implemented", "stable", "url out param bridged"},
MethodManifestEntry{"bambu_network_get_model_mall_home_url", "bambu_network_get_model_mall_home_url", "network", "implemented", "stable", "url out param bridged"},
MethodManifestEntry{"bambu_network_get_model_mall_detail_url", "bambu_network_get_model_mall_detail_url", "network", "implemented", "stable", "url out param bridged"},
MethodManifestEntry{"bambu_network_get_subtask", "bambu_network_get_subtask", "callbacks", "implemented", "experimental", "custom task object callback not bridged"},
MethodManifestEntry{"bambu_network_get_my_profile", "bambu_network_get_my_profile", "network", "implemented", "stable", "http code/body out params bridged"},
MethodManifestEntry{"bambu_network_get_my_token", "bambu_network_get_my_token", "network", "implemented", "stable", "http code/body out params bridged"},
MethodManifestEntry{"bambu_network_track_enable", "bambu_network_track_enable", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_track_remove_files", "bambu_network_track_remove_files", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_track_event", "bambu_network_track_event", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_track_header", "bambu_network_track_header", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_track_update_property", "bambu_network_track_update_property", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_track_get_property", "bambu_network_track_get_property", "network", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"bambu_network_put_model_mall_rating", "bambu_network_put_model_mall_rating", "network", "implemented", "stable", "vector and out params bridged via rpc"},
MethodManifestEntry{"bambu_network_get_oss_config", "bambu_network_get_oss_config", "network", "implemented", "stable", "out params bridged via rpc"},
MethodManifestEntry{"bambu_network_put_rating_picture_oss", "bambu_network_put_rating_picture_oss", "network", "implemented", "stable", "in-out strings and out params bridged via rpc"},
MethodManifestEntry{"bambu_network_get_model_mall_rating", "bambu_network_get_model_mall_rating", "network", "implemented", "stable", "out params bridged via rpc"},
MethodManifestEntry{"bambu_network_get_mw_user_preference", "bambu_network_get_mw_user_preference", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"bambu_network_get_mw_user_4ulist", "bambu_network_get_mw_user_4ulist", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"bambu_network_get_hms_snapshot", "bambu_network_get_hms_snapshot", "callbacks", "implemented", "stable", "one-shot callback bridged with wait"},
MethodManifestEntry{"Bambu_Create", "Bambu_Create", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_SetLogger", "Bambu_SetLogger", "source", "implemented", "stable", "logger messages copied in host and forwarded through tunnel events"},
MethodManifestEntry{"Bambu_Open", "Bambu_Open", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_StartStream", "Bambu_StartStream", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_StartStreamEx", "Bambu_StartStreamEx", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_GetStreamCount", "Bambu_GetStreamCount", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_GetStreamInfo", "Bambu_GetStreamInfo", "source", "implemented", "stable", "format_buffer marshaled and cached"},
MethodManifestEntry{"Bambu_GetDuration", "Bambu_GetDuration", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_Seek", "Bambu_Seek", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_ReadSample", "Bambu_ReadSample", "source", "implemented", "stable", "buffer marshaled as byte array"},
MethodManifestEntry{"Bambu_SendMessage", "Bambu_SendMessage", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_RecvMessage", "Bambu_RecvMessage", "source", "implemented", "stable", "ctrl/data marshaled via rpc"},
MethodManifestEntry{"Bambu_Close", "Bambu_Close", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_Destroy", "Bambu_Destroy", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_Init", "Bambu_Init", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_Deinit", "Bambu_Deinit", "source", "implemented", "stable", "rpc passthrough"},
MethodManifestEntry{"Bambu_GetLastErrorMsg", "Bambu_GetLastErrorMsg", "source", "implemented", "stable", "message copied via rpc and cached locally"},
MethodManifestEntry{"Bambu_FreeLogMsg", "Bambu_FreeLogMsg", "source", "implemented", "stable", "no-op in forwarder; host frees vendor log messages after copying"}
    };
    return entries;
}

std::optional<MethodManifestEntry> find_method_manifest_entry(const std::string& exported_name)
{
    for (const auto& entry : method_manifest()) {
        if (entry.exported_name == exported_name)
            return entry;
    }
    return std::nullopt;
}

nlohmann::json method_manifest_as_json()
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& entry : method_manifest()) {
        out.push_back({
            {"symbol", entry.symbol},
            {"exported_name", entry.exported_name},
            {"area", entry.area},
            {"status", entry.status},
            {"stability", entry.stability},
            {"notes", entry.notes}
        });
    }
    return out;
}

}
