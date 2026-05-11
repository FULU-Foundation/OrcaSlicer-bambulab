#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <boost/filesystem/path.hpp>

namespace Slic3r::PJarczakLinuxBridge {

bool enabled();
bool use_bridge_network_module();
bool source_module_is_network_module();
bool should_force_linux_plugin_payload(const std::string& plugin_name);
const char* forced_download_os_type();
const char* forced_client_version();

std::string bridge_network_module_stem();
std::string bridge_network_current_dir_name();
std::string bridge_network_library_path(const std::filesystem::path& plugin_folder);
std::string bridge_network_library_path(const boost::filesystem::path& plugin_folder);

std::string linux_network_library_name();
std::string linux_source_library_name();
std::string host_executable_file_name();
std::string mac_host_wrapper_file_name();
std::string mac_lima_instance_file_name();
std::string mac_runtime_install_script_file_name();
std::string mac_runtime_verify_script_file_name();
std::string windows_wsl_distro_file_name();
std::string windows_wsl_import_script_file_name();
std::string windows_wsl_validate_script_file_name();
std::string windows_wsl_bootstrap_script_file_name();
std::string windows_wsl_rootfs_file_name();
std::string windows_plugin_cache_subdir_file_name();

bool is_linux_payload_filename(const std::string& file_name);
bool is_overlay_runtime_filename(const std::string& file_name);
bool validate_linux_so_binary(const std::string& file_path, std::string* reason = nullptr);
std::string linux_payload_manifest_file_name();
std::string linux_payload_manifest_path(const std::filesystem::path& plugin_folder);
std::string linux_payload_manifest_path(const boost::filesystem::path& plugin_folder);
std::string sha256_file_hex(const std::string& file_path, std::string* reason = nullptr);
std::string expected_network_abi_version();
bool validate_linux_payload_file(const std::string& file_path, std::string* reason = nullptr);
bool validate_linux_payload_file_against_manifest(const std::string& file_path, const std::string& manifest_path, std::string* reason = nullptr);
bool validate_linux_payload_set_against_manifest(const std::filesystem::path& plugin_folder, std::string* reason = nullptr);
bool validate_linux_payload_set_against_manifest(const boost::filesystem::path& plugin_folder, std::string* reason = nullptr);
bool abi_version_matches_expected(const std::string& actual_version, std::string* reason = nullptr);
std::vector<std::string> ota_copy_extensions();

}
