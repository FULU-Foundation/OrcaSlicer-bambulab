#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <boost/process.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r::PJarczakLinuxBridge {

struct RpcBinaryReply {
    nlohmann::json payload;
    std::vector<unsigned char> binary;
};

class RpcClient {
public:
    static RpcClient& instance();

    bool ensure_started();
    bool is_started() const;
    int invoke_int(const std::string& method, const nlohmann::json& payload = {});
    bool invoke_bool(const std::string& method, const nlohmann::json& payload = {});
    std::string invoke_string(const std::string& method, const nlohmann::json& payload = {});
    nlohmann::json invoke_json(const std::string& method, const nlohmann::json& payload = {});
    RpcBinaryReply invoke_binary(const std::string& method, const nlohmann::json& payload = {}, const std::vector<unsigned char>& request_binary = {});
    void invoke_void(const std::string& method, const nlohmann::json& payload = {});
    std::string last_error() const;

private:
    RpcClient() = default;
    ~RpcClient();
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    struct Proc {
        boost::process::opstream in;
        boost::process::ipstream out;
        boost::process::child child;
    };

    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready{false};
        bool expects_binary{false};
        bool json_received{false};
        bool binary_received{false};
        nlohmann::json payload;
        std::vector<unsigned char> binary;
    };

    RpcBinaryReply request_impl(const std::string& method, const nlohmann::json& payload, const std::vector<unsigned char>& request_binary, bool skip_handshake);
    bool ensure_handshake();
    bool start_locked();
    void stop();
    void reader_loop();

    mutable std::mutex m_state_mutex;
    std::mutex m_write_mutex;
    std::unique_ptr<Proc> m_proc;
    std::thread m_reader;
    int m_next_id{1};
    std::string m_last_error;
    std::map<int, std::shared_ptr<Pending>> m_pending;
    std::atomic<bool> m_reader_stop{false};
    bool m_handshake_ok{false};
};

}
