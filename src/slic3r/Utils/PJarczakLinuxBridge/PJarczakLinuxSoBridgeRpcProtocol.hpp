#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r::PJarczakLinuxBridge {

enum class RpcFrameType : std::uint32_t {
    json_request = 1,
    json_response = 2,
    binary_data = 3,
    log = 4
};

struct RpcFrame {
    int id{0};
    std::string method;
    nlohmann::json payload;
};

struct RawRpcFrame {
    RpcFrameType type{RpcFrameType::json_request};
    int id{0};
    std::vector<unsigned char> payload;
};

bool write_raw_frame(std::ostream& out, RpcFrameType type, int id, const void* data, std::size_t size);
bool read_raw_frame(std::istream& in, RawRpcFrame& frame, std::string& error);

bool write_json_frame(std::ostream& out, RpcFrameType type, int id, const nlohmann::json& payload);
bool read_json_frame(const RawRpcFrame& frame, nlohmann::json& payload, std::string& error);

bool write_request_frame(std::ostream& out, const RpcFrame& frame);
bool read_request_frame(const RawRpcFrame& raw, RpcFrame& frame, std::string& error);

}
