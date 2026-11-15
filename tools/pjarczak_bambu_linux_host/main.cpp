#include "LinuxPluginHost.hpp"
#include "../../src/slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxSoBridgeRpcProtocol.hpp"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>
#include <cstdio>
#include <thread>

using namespace Slic3r::PJarczakLinuxBridge;

namespace {
int run_probe_auth()
{
    LinuxPluginHost host;
    auto hs = host.handle("bridge.handshake", nlohmann::json::object());
    if (!hs.value("network_loaded", false))
        return 2;

    const std::string log_dir = std::getenv("PJARCZAK_BAMBU_PROBE_LOG_DIR") ? std::getenv("PJARCZAK_BAMBU_PROBE_LOG_DIR") : std::string(".");
    const std::string country = std::getenv("PJARCZAK_BAMBU_COUNTRY_CODE") ? std::getenv("PJARCZAK_BAMBU_COUNTRY_CODE") : std::string("PL");
    auto created = host.handle("net.create_agent", {{"log_dir", log_dir}, {"country_code", country}});
    if (!created.value("ok", false))
        return 3;
    const auto agent = created.value("value", 0LL);
    if (agent <= 0)
        return 4;

    auto step = [&](const char* method, nlohmann::json payload) -> bool {
        payload["agent"] = agent;
        auto r = host.handle(method, payload);
        return r.value("ok", false);
    };

    if (!step("net.set_config_dir", {{"config_dir", log_dir}}))
        return 5;
    if (!step("net.init_log", nlohmann::json::object()))
        return 6;
    step("net.set_country_code", {{"country_code", country}});
    step("net.start", nlohmann::json::object());

    auto is_login = host.handle("net.is_user_login", {{"agent", agent}});
    if (!is_login.value("ok", false))
        return 7;

    return 0;
}
int run_probe_devices()
{
    LinuxPluginHost host;
    auto hs = host.handle("bridge.handshake", nlohmann::json::object());
    std::fprintf(stderr, "probe handshake: %s\n", hs.dump().c_str());
    if (!hs.value("network_loaded", false))
        return 2;

    const std::string log_dir = std::getenv("PJARCZAK_BAMBU_PROBE_LOG_DIR") ? std::getenv("PJARCZAK_BAMBU_PROBE_LOG_DIR") : std::string(".");
    const std::string config_dir = std::getenv("PJARCZAK_BAMBU_PROBE_CONFIG_DIR") ? std::getenv("PJARCZAK_BAMBU_PROBE_CONFIG_DIR") : log_dir;
    const std::string country = std::getenv("PJARCZAK_BAMBU_COUNTRY_CODE") ? std::getenv("PJARCZAK_BAMBU_COUNTRY_CODE") : std::string("PL");
    auto created = host.handle("net.create_agent", {{"log_dir", log_dir}, {"country_code", country}});
    std::fprintf(stderr, "probe create_agent: %s\n", created.dump().c_str());
    if (!created.value("ok", false))
        return 3;
    const auto agent = created.value("value", 0LL);
    if (agent <= 0)
        return 4;

    auto step = [&](const char* method, nlohmann::json payload) -> nlohmann::json {
        payload["agent"] = agent;
        auto r = host.handle(method, payload);
        std::fprintf(stderr, "probe %s: %s\n", method, r.dump().c_str());
        return r;
    };

    step("net.set_config_dir", {{"config_dir", config_dir}});
    step("net.init_log", nlohmann::json::object());
    step("net.set_country_code", {{"country_code", country}});
    step("net.start", nlohmann::json::object());
    step("net.is_user_login", nlohmann::json::object());
    step("net.get_user_print_info", nlohmann::json::object());
    step("net.start_discovery", {{"start", true}, {"sending", true}});
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto ev = host.handle("bridge.poll_events", {{"limit", 100}});
        std::fprintf(stderr, "probe poll_events: %s\n", ev.dump().c_str());
    }
    return 0;
}
}

int main(int argc, char** argv)
{
    std::ios::sync_with_stdio(false);

    if (argc > 1 && std::string(argv[1]) == "--probe-auth")
        return run_probe_auth();
    if (argc > 1 && std::string(argv[1]) == "--probe-devices")
        return run_probe_devices();

    const int rpc_fd = ::dup(STDOUT_FILENO);
    if (rpc_fd < 0)
        return 100;
    std::fflush(stdout);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    std::ofstream rpc_out(std::string("/proc/self/fd/") + std::to_string(rpc_fd), std::ios::binary | std::ios::out);
    if (!rpc_out.good())
        return 101;

    LinuxPluginHost host;
    std::mutex out_mutex;
    std::vector<std::thread> workers;
    std::mutex workers_mutex;

    while (true) {
        RawRpcFrame raw;
        std::string err;
        if (!read_raw_frame(std::cin, raw, err))
            break;

        RpcFrame req;
        if (!read_request_frame(raw, req, err)) {
            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, 0, {{"ok", false}, {"error", err}});
            continue;
        }

        std::vector<unsigned char> request_binary;
        if (req.payload.value("__binary_request", false)) {
            RawRpcFrame binary_raw;
            if (!read_raw_frame(std::cin, binary_raw, err) ||
                binary_raw.type != RpcFrameType::binary_data ||
                binary_raw.id != req.id) {
                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req.id, {{"ok", false}, {"error", "missing binary request payload"}});
                continue;
            }
            request_binary = std::move(binary_raw.payload);
        }

        if (req.method == "bridge.handshake") {
            // Answer the handshake synchronously: it is a pure metadata reply and
            // must never race with process shutdown (FEX/muvm tears the VM down
            // as soon as the host exits, which would drop a detached reply).
            nlohmann::json resp = host.handle(req.method, req.payload);
            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, req.id, resp);
            continue;
        }

        std::thread worker([&host, &rpc_out, &out_mutex, req_id = req.id, req_method = req.method, req_payload = req.payload, request_binary = std::move(request_binary)]() mutable {
            try {
                LinuxPluginHost::set_thread_request_binary(std::move(request_binary));
                nlohmann::json resp = host.handle(req_method, req_payload);

                std::vector<unsigned char> reply_binary;
                const bool has_reply_binary = LinuxPluginHost::consume_thread_reply_binary(reply_binary);

                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req_id, resp);
                if (has_reply_binary)
                    write_raw_frame(rpc_out, RpcFrameType::binary_data, req_id, reply_binary.data(), reply_binary.size());
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req_id, {{"ok", false}, {"error", e.what()}});
            } catch (...) {
                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req_id, {{"ok", false}, {"error", "unknown handler exception"}});
            }
        });
        {
            std::lock_guard<std::mutex> lock(workers_mutex);
            workers.emplace_back(std::move(worker));
            // Keep the bookkeeping bounded: join the oldest finished request
            // instead of accumulating one std::thread object per RPC call.
            if (workers.size() > 64) {
                workers.front().join();
                workers.erase(workers.begin());
            }
        }
    }

    for (auto& worker : workers) {
        if (worker.joinable())
            worker.join();
    }

    return 0;
}
