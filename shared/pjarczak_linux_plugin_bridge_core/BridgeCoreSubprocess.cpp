#include "BridgeCoreSubprocess.hpp"
#include "BridgeCoreFrame.hpp"

#include <boost/process.hpp>

namespace bp = boost::process;

namespace PJarczak::LinuxPluginBridgeCore {

struct BridgeSubprocess::Impl {
    bp::opstream in;
    bp::ipstream out;
    std::unique_ptr<bp::child> child;
};

BridgeSubprocess::BridgeSubprocess() : m_impl(std::make_unique<Impl>()) {}
BridgeSubprocess::~BridgeSubprocess() { stop(); }

bool BridgeSubprocess::start(const LaunchSpec& spec)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        stop();
        if (spec.argv.empty()) {
            m_last_error = "empty argv";
            return false;
        }
        bp::environment env = boost::this_process::environment();
        for (const auto& kv : spec.env)
            env[kv.first] = kv.second;

        std::vector<std::string> args;
        for (std::size_t i = 1; i < spec.argv.size(); ++i)
            args.push_back(spec.argv[i]);

        m_impl->child = std::make_unique<bp::child>(
            bp::search_path(spec.argv[0]),
            bp::args(args),
            bp::std_in < m_impl->in,
            bp::std_out > m_impl->out,
            env
        );
        m_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        m_last_error = e.what();
        return false;
    }
}

bool BridgeSubprocess::running() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl && m_impl->child && m_impl->child->running();
}

void BridgeSubprocess::stop()
{
    if (!m_impl || !m_impl->child)
        return;
    if (m_impl->child->running())
        m_impl->child->terminate();
    m_impl->child.reset();
}

nlohmann::json BridgeSubprocess::request(const std::string& method, const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl || !m_impl->child || !m_impl->child->running())
        return {{"ok", false}, {"error", "host not running"}};

    RpcFrame req;
    req.id = m_next_id++;
    req.method = method;
    req.payload = payload;

    m_impl->in << encode_rpc_frame(req);
    m_impl->in.flush();

    std::string line;
    if (!std::getline(m_impl->out, line)) {
        m_last_error = "host closed stdout";
        return {{"ok", false}, {"error", m_last_error}};
    }

    RpcFrame reply;
    std::string error;
    if (!decode_rpc_frame(line, reply, error)) {
        m_last_error = error;
        return {{"ok", false}, {"error", error}};
    }
    return reply.payload;
}

std::string BridgeSubprocess::last_error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

}
