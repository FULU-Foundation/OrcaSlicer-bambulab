#pragma once

#include "BridgeCoreTypes.hpp"

#include <deque>

namespace PJarczak::LinuxPluginBridgeCore {

class CallbackQueue {
public:
    void push(BridgeEvent event);
    bool try_pop(BridgeEvent& event);
    std::vector<BridgeEvent> drain();
    std::size_t size() const;
    void clear();

private:
    mutable std::mutex m_mutex;
    std::deque<BridgeEvent> m_queue;
};

}
