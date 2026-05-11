#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>

#if defined(PJARCZAK_LINUX_BRIDGE_LIGHTWEIGHT_TASKS) || defined(PJARCZAK_LINUX_BRIDGE_STANDALONE_HOST) || !__has_include("../../../libslic3r/ProjectTask.hpp")
namespace Slic3r {
class BBLModelTask {
public:
    int job_id{0};
    int design_id{0};
    int profile_id{0};
    int instance_id{0};
    std::string task_id;
    std::string model_id;
    std::string model_name;
    std::string profile_name;
};
typedef std::function<void(BBLModelTask* subtask)> OnGetSubTaskFn;
}
#else
#include "../../../libslic3r/ProjectTask.hpp"
#endif

namespace Slic3r::PJarczakLinuxBridge {

inline nlohmann::json model_task_to_json(const Slic3r::BBLModelTask* task)
{
    if (!task)
        return nlohmann::json::object();
    return {
        {"job_id", task->job_id},
        {"design_id", task->design_id},
        {"profile_id", task->profile_id},
        {"instance_id", task->instance_id},
        {"task_id", task->task_id},
        {"model_id", task->model_id},
        {"model_name", task->model_name},
        {"profile_name", task->profile_name},
    };
}

inline void json_to_model_task(const nlohmann::json& j, Slic3r::BBLModelTask& task)
{
    task.job_id = j.value("job_id", 0);
    task.design_id = j.value("design_id", 0);
    task.profile_id = j.value("profile_id", 0);
    task.instance_id = j.value("instance_id", 0);
    task.task_id = j.value("task_id", std::string());
    task.model_id = j.value("model_id", std::string());
    task.model_name = j.value("model_name", std::string());
    task.profile_name = j.value("profile_name", std::string());
}

}
