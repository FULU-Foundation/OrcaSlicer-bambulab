
#!/usr/bin/env python3
from pathlib import Path
import sys

def read(p: Path) -> str:
    return p.read_text(encoding='utf-8')

def write(p: Path, s: str) -> None:
    p.write_text(s, encoding='utf-8')

def replace_once(s: str, old: str, new: str, path: Path) -> str:
    if old not in s:
        raise RuntimeError(f"missing expected snippet in {path}: {old[:120]!r}")
    return s.replace(old, new, 1)

def replace_function_body(s: str, signature: str, new_body: str, path: Path) -> str:
    idx = s.find(signature)
    if idx < 0:
        raise RuntimeError(f"signature not found in {path}: {signature}")
    brace = s.find('{', idx)
    if brace < 0:
        raise RuntimeError(f"opening brace not found in {path}: {signature}")
    depth = 0
    end = brace
    while end < len(s):
        ch = s[end]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end += 1
                break
        end += 1
    if depth != 0:
        raise RuntimeError(f"unterminated function body in {path}: {signature}")
    return s[:brace] + new_body + s[end:]

BBL_INIT = r'''{
    clear_load_error();

    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

    if (using_backup) {
        plugin_folder = plugin_folder / "backup";
    }

    const bool pj_bridge = Slic3r::PJarczakLinuxBridge::enabled();

    if (pj_bridge) {
#if defined(_MSC_VER) || defined(_WIN32)
        _putenv_s("PJARCZAK_BAMBU_PLUGIN_DIR", plugin_folder.string().c_str());
        _putenv_s("PJARCZAK_EXPECTED_BAMBU_NETWORK_VERSION", version.c_str());
#else
        setenv("PJARCZAK_BAMBU_PLUGIN_DIR", plugin_folder.string().c_str(), 1);
        setenv("PJARCZAK_EXPECTED_BAMBU_NETWORK_VERSION", version.c_str(), 1);
#endif
    }

    if (version.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": version is required but not provided";
        set_load_error(
            "Network library version not specified",
            "A version must be specified to load the network library",
            ""
        );
        return -1;
    }

#if defined(_MSC_VER) || defined(_WIN32)
    if (pj_bridge) {
        library = Slic3r::PJarczakLinuxBridge::bridge_network_library_path(plugin_folder);
        wchar_t lib_wstr[512];
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, 0, library.c_str(), int(library.size()) + 1, lib_wstr, int(sizeof(lib_wstr) / sizeof(lib_wstr[0])));
        m_networking_module = LoadLibrary(lib_wstr);
    } else {
        library = plugin_folder.string() + "\\" + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dll";
        wchar_t lib_wstr[256];
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        m_networking_module = LoadLibrary(lib_wstr);
        if (!m_networking_module) {
            std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_NETWORK_LIBRARY));
            if (library_path.empty()) {
                set_load_error(
                    "Network library not found",
                    "Could not locate versioned library: " + library,
                    library
                );
                return -1;
            }
            memset(lib_wstr, 0, sizeof(lib_wstr));
            ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
            m_networking_module = LoadLibrary(lib_wstr);
        }
    }
#else
    if (pj_bridge) {
        library = Slic3r::PJarczakLinuxBridge::bridge_network_library_path(plugin_folder);
        m_networking_module = dlopen(library.c_str(), RTLD_LAZY);
    } else {
    #if defined(__WXMAC__)
        std::string lib_ext = ".dylib";
    #else
        std::string lib_ext = ".so";
    #endif
        library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + lib_ext;
        m_networking_module = dlopen(library.c_str(), RTLD_LAZY);
        if (!m_networking_module) {
            char* dll_error = dlerror();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": dlopen failed: " << (dll_error ? dll_error : "unknown error");
            set_load_error(
                "Failed to load network library",
                dll_error ? std::string(dll_error) : "Unknown dlopen error",
                library
            );
        }
    }
#endif

    if (!m_networking_module) {
        if (!m_load_error.has_error) {
            set_load_error(
                "Network library failed to load",
                "LoadLibrary/dlopen returned null",
                library
            );
        }
        return -1;
    }

    InitFTModule(m_networking_module);

    load_all_function_pointers();

    m_use_legacy_network = NetworkAgent::use_legacy_network;

    std::string loaded_version;
    if (m_get_version) {
        loaded_version = m_get_version();
    }

    BOOST_LOG_TRIVIAL(info) << "BBLNetworkPlugin::initialize: legacy_mode="
        << (m_use_legacy_network ? "true" : "false")
        << ", bridge_mode=" << (pj_bridge ? "true" : "false")
        << ", library=" << library
        << ", version=" << (loaded_version.empty() ? "unknown" : loaded_version)
        << ", send_message=" << (m_send_message ? "loaded" : "null")
        << ", start_print=" << (m_start_print ? "loaded" : "null")
        << ", start_local_print=" << (m_start_local_print ? "loaded" : "null");

    return 0;
}'''

BBL_UNLOAD = r'''{
    UnloadFTModule();

#if defined(_MSC_VER) || defined(_WIN32)
    const bool same_handles = m_source_module && (m_source_module == m_networking_module);
    if (m_source_module && !same_handles) {
        FreeLibrary(m_source_module);
        m_source_module = NULL;
    }
    if (m_networking_module) {
        FreeLibrary(m_networking_module);
        m_networking_module = NULL;
    }
#else
    const bool same_handles = m_source_module && (m_source_module == m_networking_module);
    if (m_source_module && !same_handles) {
        dlclose(m_source_module);
        m_source_module = NULL;
    }
    if (m_networking_module) {
        dlclose(m_networking_module);
        m_networking_module = NULL;
    }
#endif

    m_source_module = NULL;
    clear_all_function_pointers();

    return 0;
}'''

BBL_SOURCE = r'''{
    if ((m_source_module) || (!m_networking_module))
        return m_source_module;

    if (Slic3r::PJarczakLinuxBridge::enabled() && Slic3r::PJarczakLinuxBridge::source_module_is_network_module()) {
        m_source_module = m_networking_module;
        return m_source_module;
    }

    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t lib_wstr[128];

    library = plugin_folder.string() + "/" + std::string(BAMBU_SOURCE_LIBRARY) + ".dll";
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    m_source_module = LoadLibrary(lib_wstr);
    if (!m_source_module) {
        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_SOURCE_LIBRARY));
        if (library_path.empty()) {
            return m_source_module;
        }
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        m_source_module = LoadLibrary(lib_wstr);
    }
#else
#if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".dylib";
#else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".so";
#endif
    m_source_module = dlopen(library.c_str(), RTLD_LAZY);
#endif

    return m_source_module;
}'''

GUI_DOWNLOAD = r'''{
    int result = 0;
    std::string err_msg;

    AppConfig* app_config = wxGetApp().app_config;
    if (!app_config) {
        return -1;
    }

    m_networking_cancel_update = false;
    fs::path target_file_path = (fs::temp_directory_path() / package_name);
    fs::path tmp_path = target_file_path;
    tmp_path += format(".%1%%2%", get_current_pid(), ".tmp");

    const bool pj_force_linux_payload = Slic3r::PJarczakLinuxBridge::should_force_linux_plugin_payload(name);
    std::map<std::string, std::string> saved_headers = Slic3r::Http::get_extra_headers();
    bool changed_headers = false;

    auto restore_headers = [&]() {
        if (changed_headers) {
            Slic3r::Http::set_extra_headers(saved_headers);
            changed_headers = false;
        }
    };

    if (pj_force_linux_payload) {
        auto headers = saved_headers;
        headers["X-BBL-OS-Type"] = Slic3r::PJarczakLinuxBridge::forced_download_os_type();
        Slic3r::Http::set_extra_headers(headers);
        changed_headers = true;
    }

    std::string url = get_plugin_url(name, app_config->get_country_code());
    std::string download_url;
    Slic3r::Http http_url = Slic3r::Http::get(url);
    BOOST_LOG_TRIVIAL(info) << "[download_plugin]: check the plugin from " << url;
    http_url.timeout_connect(TIMEOUT_CONNECT)
        .timeout_max(TIMEOUT_RESPONSE)
        .on_complete(
            [&download_url](std::string body, unsigned status) {
                try {
                    json j = json::parse(body);
                    std::string message = j["message"].get<std::string>();
                    if (message == "success") {
                        json resource = j.at("resources");
                        if (resource.is_array()) {
                            for (auto iter = resource.begin(); iter != resource.end(); iter++) {
                                for (auto sub_iter = iter.value().begin(); sub_iter != iter.value().end(); sub_iter++) {
                                    if (boost::iequals(sub_iter.key(), "url"))
                                        download_url = sub_iter.value();
                                }
                            }
                        }
                    }
                } catch (...) {}
            })
        .on_error(
            [&result, &err_msg](std::string body, std::string error, unsigned int status) {
                BOOST_LOG_TRIVIAL(error) << "[download_plugin 1] on_error: " << error << ", body = " << body;
                err_msg += "[download_plugin 1] on_error: " + error + ", body = " + body;
                result = -1;
            })
        .perform_sync();

    restore_headers();

    bool cancel = false;
    if (result < 0) {
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        return result;
    }

    if (download_url.empty()) {
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        return -1;
    } else if (pro_fn) {
        pro_fn(InstallStatusNormal, 5, cancel);
    }

    if (m_networking_cancel_update || cancel) {
        return -1;
    }

    BOOST_LOG_TRIVIAL(info) << "[download_plugin] get_url = " << download_url;

    Slic3r::Http http = Slic3r::Http::get(download_url);
    int reported_percent = 0;
    http.on_progress(
            [this, &pro_fn, cancel_fn, &result, &reported_percent, &err_msg](Slic3r::Http::Progress progress, bool& cancel) {
                int percent = 0;
                if (progress.dltotal != 0)
                    percent = progress.dlnow * 50 / progress.dltotal;
                bool was_cancel = false;
                if (pro_fn && ((percent - reported_percent) >= 10)) {
                    pro_fn(InstallStatusNormal, percent, was_cancel);
                    reported_percent = percent;
                    BOOST_LOG_TRIVIAL(info) << "[download_plugin 2] progress: " << reported_percent;
                }
                cancel = m_networking_cancel_update || was_cancel;
                if (cancel_fn && cancel_fn())
                    cancel = true;
                if (cancel) {
                    err_msg += "[download_plugin] cancel";
                    result = -1;
                }
            })
        .on_complete([&pro_fn, tmp_path, target_file_path](std::string body, unsigned status) {
            bool cancel = false;
            fs::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
            file.write(body.c_str(), body.size());
            file.close();
            fs::rename(tmp_path, target_file_path);
            if (pro_fn) pro_fn(InstallStatusDownloadCompleted, 80, cancel);
        })
        .on_error([&pro_fn, &result, &err_msg](std::string body, std::string error, unsigned int status) {
            bool cancel = false;
            if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
            BOOST_LOG_TRIVIAL(error) << "[download_plugin 2] on_error: " << error << ", body = " << body;
            err_msg += "[download_plugin 2] on_error: " + error + ", body = " + body;
            result = -1;
        });

    http.perform_sync();
    return result;
}'''

GUI_INSTALL = r'''{
    bool cancel = false;
    std::string target_file_path = (fs::temp_directory_path() / package_name).string();

    BOOST_LOG_TRIVIAL(info) << "[install_plugin] enter";
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / name;
    auto backup_folder = plugin_folder / "backup";
    if (!boost::filesystem::exists(plugin_folder))
        boost::filesystem::create_directory(plugin_folder);
    if (!boost::filesystem::exists(backup_folder))
        boost::filesystem::create_directory(backup_folder);

    if (m_networking_cancel_update)
        return -1;
    if (pro_fn)
        pro_fn(InstallStatusNormal, 50, cancel);

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, target_file_path)) {
        if (pro_fn) pro_fn(InstallStatusDownloadFailed, 0, cancel);
        return InstallStatusUnzipFailed;
    }

    const bool pj_force_linux_payload = Slic3r::PJarczakLinuxBridge::should_force_linux_plugin_payload(name);
    const std::string manifest_name = Slic3r::PJarczakLinuxBridge::linux_payload_manifest_file_name();

    mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;
    for (mz_uint i = 0; i < num_entries; i++) {
        if (m_networking_cancel_update || cancel) {
            close_zip_reader(&archive);
            return -1;
        }
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        if (stat.m_uncomp_size == 0)
            continue;

        std::string dest_file;
        if (stat.m_is_utf8) {
            dest_file = stat.m_filename;
        } else {
            std::string extra(1024, 0);
            size_t n = mz_zip_reader_get_extra(&archive, stat.m_file_index, extra.data(), extra.size());
            dest_file = decode(extra.substr(0, n), stat.m_filename);
        }

        boost::filesystem::path relative(dest_file);
        if (pj_force_linux_payload) {
            const std::string file_name = relative.filename().string();
            if (!(file_name == manifest_name || Slic3r::PJarczakLinuxBridge::is_linux_payload_filename(file_name)))
                continue;
            relative = boost::filesystem::path(file_name);
        }

        auto dest_path = plugin_folder / relative;
        boost::filesystem::create_directories(dest_path.parent_path());
        std::string dest_zip_file = encode_path(dest_path.string().c_str());

        try {
            if (fs::exists(dest_path))
                fs::remove(dest_path);
            mz_bool res = 0;
#ifndef WIN32
            if (S_ISLNK(stat.m_external_attr >> 16)) {
                std::string link(stat.m_uncomp_size + 1, 0);
                res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, link.data(), stat.m_uncomp_size, 0);
                try {
                    boost::filesystem::create_symlink(link, dest_path);
                } catch (const std::exception &) {}
            } else {
#endif
                res = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_zip_file.c_str(), 0);
#ifndef WIN32
            }
#endif
            if (res == 0) {
#ifdef WIN32
                std::wstring new_dest_zip_file = boost::locale::conv::utf_to_utf<wchar_t>(dest_path.generic_string());
                res = mz_zip_reader_extract_to_file_w(&archive, stat.m_file_index, new_dest_zip_file.c_str(), 0);
#endif
                if (res == 0) {
                    close_zip_reader(&archive);
                    if (pro_fn) pro_fn(InstallStatusUnzipFailed, 0, cancel);
                    return InstallStatusUnzipFailed;
                }
            }

            if (pj_force_linux_payload && relative.filename().string() != manifest_name) {
                std::string validate_reason;
                if (!Slic3r::PJarczakLinuxBridge::validate_linux_payload_file(dest_path.string(), &validate_reason)) {
                    BOOST_LOG_TRIVIAL(error) << "[install_plugin] linux payload validation failed for " << dest_path.string() << ": " << validate_reason;
                    close_zip_reader(&archive);
                    if (pro_fn) pro_fn(InstallStatusUnzipFailed, 0, cancel);
                    return InstallStatusUnzipFailed;
                }
            }
        } catch (const std::exception &) {
            close_zip_reader(&archive);
            if (pro_fn) pro_fn(InstallStatusUnzipFailed, 0, cancel);
            return InstallStatusUnzipFailed;
        }
    }

    close_zip_reader(&archive);

    if (pj_force_linux_payload) {
        std::string validate_reason;
        const auto manifest_path = plugin_folder / manifest_name;
        if (boost::filesystem::exists(manifest_path) &&
            !Slic3r::PJarczakLinuxBridge::validate_linux_payload_set_against_manifest(plugin_folder, &validate_reason)) {
            BOOST_LOG_TRIVIAL(error) << "[install_plugin] manifest validation failed: " << validate_reason;
            if (pro_fn) pro_fn(InstallStatusUnzipFailed, 0, cancel);
            return InstallStatusUnzipFailed;
        }
    }

    {
        fs::path dir_path(plugin_folder);
        if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
            int file_count = 0, file_index = 0;
            for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
                if (it->path().string() == backup_folder.string())
                    continue;
                if (fs::is_regular_file(it->status()))
                    ++file_count;
            }
            for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
                if (it->path().string() == backup_folder.string())
                    continue;
                auto dest_path = backup_folder.string() + "/" + it->path().filename().string();
                if (fs::is_regular_file(it->status())) {
                    try {
                        if (file_count > 0 && pro_fn)
                            pro_fn(InstallStatusNormal, 50 + file_index / file_count, cancel);
                        ++file_index;
                        if (fs::exists(dest_path))
                            fs::remove(dest_path);
                        std::string error_message;
                        CopyFileResult cfr = copy_file(it->path().string(), dest_path, error_message, false);
                        if (cfr != CopyFileResult::SUCCESS)
                            BOOST_LOG_TRIVIAL(error) << "Copying to backup failed(" << cfr << "): " << error_message;
                    } catch (const std::exception &) {}
                } else {
                    copy_framework(it->path().string(), dest_path);
                }
            }
        }
    }

    if (pro_fn)
        pro_fn(InstallStatusInstallCompleted, 100, cancel);
    if (name == "plugins")
        app_config->set_bool("installed_networking", true);
    BOOST_LOG_TRIVIAL(info) << "[install_plugin] success";
    return 0;
}'''

GUI_COPY = r'''{
    if (app_config->get("update_network_plugin") != "true")
        return;

    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";
    auto cache_folder = data_dir_path / "ota";
    std::string changelog_file = cache_folder.string() + "/network_plugins.json";

    std::string cached_version;
    if (boost::filesystem::exists(changelog_file)) {
        try {
            boost::nowide::ifstream ifs(changelog_file);
            json j;
            ifs >> j;
            if (j.contains("version"))
                cached_version = j["version"];
        } catch (nlohmann::detail::parse_error&) {}
    }

    if (!boost::filesystem::exists(plugin_folder))
        boost::filesystem::create_directory(plugin_folder);

    const bool pj_force_linux_payload = Slic3r::PJarczakLinuxBridge::enabled();
    std::string error_message;

    auto copy_one = [&](const boost::filesystem::path& src, const boost::filesystem::path& dst) -> bool {
        CopyFileResult cfr = copy_file(src.string(), dst.string(), error_message, false);
        if (cfr != CopyFileResult::SUCCESS) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": Copying failed(" << cfr << "): " << error_message;
            return false;
        }
        static constexpr const auto perms = fs::owner_read | fs::owner_write | fs::group_read | fs::others_read;
        fs::permissions(dst, perms);
        return true;
    };

    if (pj_force_linux_payload) {
        for (const auto& file_name : {
                Slic3r::PJarczakLinuxBridge::linux_network_library_name(),
                Slic3r::PJarczakLinuxBridge::linux_source_library_name(),
                "liblive555.so",
                Slic3r::PJarczakLinuxBridge::linux_payload_manifest_file_name() }) {
            const auto src = cache_folder / file_name;
            if (!boost::filesystem::exists(src))
                continue;
            if (file_name != Slic3r::PJarczakLinuxBridge::linux_payload_manifest_file_name()) {
                std::string validate_reason;
                if (!Slic3r::PJarczakLinuxBridge::validate_linux_payload_file(src.string(), &validate_reason)) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": invalid linux payload " << src.string() << ": " << validate_reason;
                    continue;
                }
            }
            if (!copy_one(src, plugin_folder / file_name))
                return;
            fs::remove(src);
        }

        const auto manifest = plugin_folder / Slic3r::PJarczakLinuxBridge::linux_payload_manifest_file_name();
        if (boost::filesystem::exists(manifest)) {
            std::string validate_reason;
            if (!Slic3r::PJarczakLinuxBridge::validate_linux_payload_set_against_manifest(plugin_folder, &validate_reason)) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": manifest validation failed after copy: " << validate_reason;
                return;
            }
        }

        if (!cached_version.empty()) {
            app_config->set(SETTING_NETWORK_PLUGIN_VERSION, cached_version);
            app_config->save();
        }
        if (boost::filesystem::exists(changelog_file))
            fs::remove(changelog_file);
        app_config->set("update_network_plugin", "false");
        return;
    }

    if (cached_version.empty()) {
        app_config->set("update_network_plugin", "false");
        return;
    }

    std::string network_library, player_library, live555_library, network_library_dst, player_library_dst, live555_library_dst;
#if defined(_MSC_VER) || defined(_WIN32)
    network_library = cache_folder.string() + "/bambu_networking.dll";
    player_library = cache_folder.string() + "/BambuSource.dll";
    live555_library = cache_folder.string() + "/live555.dll";
    network_library_dst = plugin_folder.string() + "/" + std::string(BAMBU_NETWORK_LIBRARY) + "_" + cached_version + ".dll";
    player_library_dst = plugin_folder.string() + "/BambuSource.dll";
    live555_library_dst = plugin_folder.string() + "/live555.dll";
#elif defined(__WXMAC__)
    network_library = cache_folder.string() + "/libbambu_networking.dylib";
    player_library = cache_folder.string() + "/libBambuSource.dylib";
    live555_library = cache_folder.string() + "/liblive555.dylib";
    network_library_dst = plugin_folder.string() + "/lib" + std::string(BAMBU_NETWORK_LIBRARY) + "_" + cached_version + ".dylib";
    player_library_dst = plugin_folder.string() + "/libBambuSource.dylib";
    live555_library_dst = plugin_folder.string() + "/liblive555.dylib";
#else
    network_library = cache_folder.string() + "/libbambu_networking.so";
    player_library = cache_folder.string() + "/libBambuSource.so";
    live555_library = cache_folder.string() + "/liblive555.so";
    network_library_dst = plugin_folder.string() + "/lib" + std::string(BAMBU_NETWORK_LIBRARY) + "_" + cached_version + ".so";
    player_library_dst = plugin_folder.string() + "/libBambuSource.so";
    live555_library_dst = plugin_folder.string() + "/liblive555.so";
#endif

    if (boost::filesystem::exists(network_library)) {
        if (!copy_one(network_library, network_library_dst))
            return;
        fs::remove(network_library);
        app_config->set(SETTING_NETWORK_PLUGIN_VERSION, cached_version);
        app_config->save();
    }

    if (boost::filesystem::exists(player_library)) {
        if (!copy_one(player_library, player_library_dst))
            return;
        fs::remove(player_library);
    }

    if (boost::filesystem::exists(live555_library)) {
        if (!copy_one(live555_library, live555_library_dst))
            return;
        fs::remove(live555_library);
    }

    if (boost::filesystem::exists(changelog_file))
        fs::remove(changelog_file);
    app_config->set("update_network_plugin", "false");
}'''

def patch_cmake(repo: Path):
    path = repo / "src/slic3r/CMakeLists.txt"
    s = read(path)
    if 'add_subdirectory(Utils/PJarczakLinuxBridge)' not in s:
        s = replace_once(
            s,
            'add_subdirectory(GUI/DeviceCore)\nadd_subdirectory(GUI/DeviceTab)\n',
            'add_subdirectory(GUI/DeviceCore)\nadd_subdirectory(GUI/DeviceTab)\nadd_subdirectory(Utils/PJarczakLinuxBridge)\n',
            path
        )
    if 'Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.cpp' not in s:
        s = replace_once(
            s,
            '    Utils/bambu_networking.hpp\n',
            '    Utils/bambu_networking.hpp\n    Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.cpp\n',
            path
        )
    write(path, s)

def patch_plugin_cpp(repo: Path):
    path = repo / "src/slic3r/Utils/BBLNetworkPlugin.cpp"
    s = read(path)
    if '#include "PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"\n' not in s:
        s = replace_once(
            s,
            '#include "NetworkAgent.hpp"\n',
            '#include "NetworkAgent.hpp"\n#include "PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"\n',
            path
        )
    s = replace_function_body(s, 'int BBLNetworkPlugin::initialize(bool using_backup, const std::string& version)\n', BBL_INIT, path)
    s = replace_function_body(s, 'int BBLNetworkPlugin::unload()\n', BBL_UNLOAD, path)
    s = replace_function_body(s, '#if defined(_MSC_VER) || defined(_WIN32)\nHMODULE BBLNetworkPlugin::get_source_module()\n#else\nvoid* BBLNetworkPlugin::get_source_module()\n#endif\n', BBL_SOURCE, path)
    write(path, s)

def patch_gui_app(repo: Path):
    path = repo / "src/slic3r/GUI/GUI_App.cpp"
    s = read(path)
    if '#include "slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"\n' not in s:
        s = replace_once(
            s,
            '#include "slic3r/Utils/bambu_networking.hpp"\n',
            '#include "slic3r/Utils/bambu_networking.hpp"\n#include "slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"\n',
            path
        )
    s = replace_function_body(s, 'int GUI_App::download_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn, WasCancelledFn cancel_fn)\n', GUI_DOWNLOAD, path)
    s = replace_function_body(s, 'int GUI_App::install_plugin(std::string name, std::string package_name, InstallProgressFn pro_fn, WasCancelledFn cancel_fn)\n', GUI_INSTALL, path)
    s = replace_function_body(s, 'void GUI_App::copy_network_if_available()\n', GUI_COPY, path)
    write(path, s)

def main():
    if len(sys.argv) != 2:
        print("usage: apply_pjarczak_linux_bridge_orca.py /path/to/OrcaSlicer")
        raise SystemExit(2)

    repo = Path(sys.argv[1]).resolve()
    patch_cmake(repo)
    patch_plugin_cpp(repo)
    patch_gui_app(repo)
    print("patched:", repo)

if __name__ == "__main__":
    main()
