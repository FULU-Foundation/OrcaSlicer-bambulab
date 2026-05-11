#pragma once

namespace Slic3r::PJarczakLinuxBridge {

class EventPump {
public:
    static EventPump& instance();
    void ensure_started();
    void stop();

private:
    EventPump() = default;
    EventPump(const EventPump&) = delete;
    EventPump& operator=(const EventPump&) = delete;
    ~EventPump();

    void run();

    bool m_running{false};
};

}
