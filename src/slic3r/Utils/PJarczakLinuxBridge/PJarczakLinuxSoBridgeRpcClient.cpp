#include "PJarczakLinuxSoBridgeRpcClient.hpp"
#include "PJarczakLinuxSoBridgeLauncher.hpp"
#include "PJarczakLinuxSoBridgeRpcProtocol.hpp"

#include <boost/process/environment.hpp>
#if defined(_WIN32)
#include <boost/process/windows.hpp>
#endif

#include <stdexcept>

namespace bp = boost::process;

namespace Slic3r::PJarczakLinuxBridge {

namespace {

nlohmann::json make_error_payload(const std::string& error)
{
    return {{"ok", false}, {"error", error}};
}

}

RpcClient& RpcClient::instance()
{
    static RpcClient client;
    return client;
}

RpcClient::~RpcClient()
{
    stop();
}

bool RpcClient::start_locked()
{
    if (m_proc && m_proc->child.running())
        return true;

    if (m_reader.joinable()) {
        m_state_mutex.unlock();
        m_reader.join();
        m_state_mutex.lock();
    }

    try {
        auto spec = build_default_launch_spec();
        if (spec.argv.empty()) {
            m_last_error = "empty launch spec";
            return false;
        }

        bp::environment env = boost::this_process::environment();
        for (const auto& kv : spec.env)
            env[kv.first] = kv.second;

        std::vector<std::string> args;
        for (std::size_t i = 1; i < spec.argv.size(); ++i)
            args.push_back(spec.argv[i]);

        const auto preflight_error = launch_preflight_error();
        if (!preflight_error.empty()) {
            m_last_error = preflight_error;
            return false;
        }

        auto proc = std::make_unique<Proc>();
#if defined(_WIN32)
        proc->child = bp::child(spec.argv[0], bp::args(args), bp::std_in < proc->in, bp::std_out > proc->out,
                                bp::windows::create_no_window, bp::windows::hide, env);
#else
        proc->child = bp::child(spec.argv[0], bp::args(args), bp::std_in < proc->in, bp::std_out > proc->out, env);
#endif
        m_proc = std::move(proc);
        m_reader_stop.store(false, std::memory_order_release);
        m_handshake_ok = false;
        m_reader = std::thread([this] { reader_loop(); });
        m_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        m_last_error = e.what();
        m_proc.reset();
        m_handshake_ok = false;
        return false;
    }
}

void RpcClient::stop()
{
    std::unique_ptr<Proc> proc;
    std::thread reader;
    std::map<int, std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_reader_stop.store(true, std::memory_order_release);
        m_handshake_ok = false;
        proc = std::move(m_proc);
        reader = std::move(m_reader);
        pending = std::move(m_pending);
        m_pending.clear();
    }

    if (proc) {
        try {
            if (proc->child.running())
                proc->child.terminate();
        } catch (...) {}
    }

    if (reader.joinable())
        reader.join();

    for (auto& it : pending) {
        std::lock_guard<std::mutex> plock(it.second->mutex);
        if (!it.second->ready) {
            it.second->payload = make_error_payload("bridge host stopped");
            it.second->ready = true;
            it.second->cv.notify_all();
        }
    }
}

bool RpcClient::ensure_started()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!start_locked())
            return false;
    }
    return ensure_handshake();
}

bool RpcClient::is_started() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_proc && m_proc->child.running();
}

bool RpcClient::ensure_handshake()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_proc || !m_proc->child.running())
            return false;
        if (m_handshake_ok)
            return true;
    }

    const auto reply = request_impl("bridge.handshake", nlohmann::json::object(), {}, true);
    if (!reply.payload.value("ok", false)) {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = reply.payload.value("error", std::string("bridge handshake failed"));
        }
        stop();
        return false;
    }

    if (reply.payload.value("protocol_version", 0) != 1) {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = "bridge protocol version mismatch";
        }
        stop();
        return false;
    }

    if (!reply.payload.value("network_loaded", false) || !reply.payload.value("source_loaded", false)) {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = "bridge host failed to load linux payload: network=" +
                reply.payload.value("network_status", std::string("unknown")) +
                ", source=" + reply.payload.value("source_status", std::string("unknown"));
        }
        stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_handshake_ok = true;
        m_last_error.clear();
    }
    return true;
}

void RpcClient::reader_loop()
{
    while (!m_reader_stop.load(std::memory_order_acquire)) {
        Proc* proc = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_proc)
                break;
            proc = m_proc.get();
        }

        RawRpcFrame raw;
        std::string error;
        if (!read_raw_frame(proc->out, raw, error)) {
            std::map<int, std::shared_ptr<Pending>> pending;
            std::string local_error;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_reader_stop.load(std::memory_order_acquire))
                    break;
                m_last_error = error.empty() ? "bridge host closed stdout" : error;
                local_error = m_last_error;
                pending = m_pending;
                m_pending.clear();
            }
            for (auto& it : pending) {
                std::lock_guard<std::mutex> plock(it.second->mutex);
                it.second->payload = make_error_payload(local_error);
                it.second->ready = true;
                it.second->cv.notify_all();
            }
            break;
        }

        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_pending.find(raw.id);
            if (it != m_pending.end())
                pending = it->second;
        }
        if (!pending)
            continue;

        if (raw.type == RpcFrameType::json_response) {
            nlohmann::json payload;
            if (!read_json_frame(raw, payload, error))
                payload = make_error_payload(error);

            bool ready = false;
            {
                std::lock_guard<std::mutex> plock(pending->mutex);
                pending->payload = std::move(payload);
                pending->expects_binary = pending->payload.value("__binary_pending", false);
                pending->json_received = true;
                ready = !pending->expects_binary || pending->binary_received;
                pending->ready = ready;
            }

            if (ready) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_pending.erase(raw.id);
                pending->cv.notify_all();
            }
            continue;
        }

        if (raw.type == RpcFrameType::binary_data) {
            bool ready = false;
            {
                std::lock_guard<std::mutex> plock(pending->mutex);
                pending->binary = std::move(raw.payload);
                pending->binary_received = true;
                ready = pending->json_received;
                pending->ready = ready;
            }

            if (ready) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_pending.erase(raw.id);
                pending->cv.notify_all();
            }
            continue;
        }
    }
}

RpcBinaryReply RpcClient::request_impl(const std::string& method, const nlohmann::json& payload, const std::vector<unsigned char>& request_binary, bool skip_handshake)
{
    if (!skip_handshake && !ensure_started())
        return {make_error_payload(last_error()), {}};

    std::shared_ptr<Pending> pending = std::make_shared<Pending>();
    int id = 0;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_proc || !m_proc->child.running())
            return {make_error_payload("bridge host is not running"), {}};
        id = m_next_id++;
        m_pending[id] = pending;
    }

    try {
        RpcFrame frame;
        frame.id = id;
        frame.method = method;
        frame.payload = payload;
        if (!request_binary.empty()) {
            frame.payload["__binary_request"] = true;
            frame.payload["__binary_request_size"] = request_binary.size();
        }

        std::lock_guard<std::mutex> wlock(m_write_mutex);
        if (!m_proc || !m_proc->child.running())
            throw std::runtime_error("bridge host is not running");
        if (!write_request_frame(m_proc->in, frame))
            throw std::runtime_error("failed to write request frame");
        if (!request_binary.empty() && !write_raw_frame(m_proc->in, RpcFrameType::binary_data, id, request_binary.data(), request_binary.size()))
            throw std::runtime_error("failed to write request binary frame");
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_pending.erase(id);
        m_last_error = e.what();
        return {make_error_payload(m_last_error), {}};
    }

    std::unique_lock<std::mutex> plock(pending->mutex);
    pending->cv.wait(plock, [&] { return pending->ready; });
    return {pending->payload, pending->binary};
}

int RpcClient::invoke_int(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    if (reply.payload.contains("ret"))
        return reply.payload.value("ret", -1);
    return reply.payload.value("value", -1);
}

bool RpcClient::invoke_bool(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    return reply.payload.value("value", false);
}

std::string RpcClient::invoke_string(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    return reply.payload.value("value", std::string());
}

nlohmann::json RpcClient::invoke_json(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload;
}

RpcBinaryReply RpcClient::invoke_binary(const std::string& method, const nlohmann::json& payload, const std::vector<unsigned char>& request_binary)
{
    return request_impl(method, payload, request_binary, false);
}

void RpcClient::invoke_void(const std::string& method, const nlohmann::json& payload)
{
    (void) request_impl(method, payload, {}, false);
}

std::string RpcClient::last_error() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_last_error;
}

}
