#include "PJarczakLinuxSoBridgeRpcProtocol.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace Slic3r::PJarczakLinuxBridge {

namespace {

constexpr std::uint32_t k_magic = 0x52424a50u;
constexpr std::size_t k_header_size = 16;

void write_u32_le(unsigned char* dst, std::uint32_t value)
{
    dst[0] = static_cast<unsigned char>(value & 0xffu);
    dst[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
    dst[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
    dst[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
}

std::uint32_t read_u32_le(const unsigned char* src)
{
    return std::uint32_t(src[0]) |
           (std::uint32_t(src[1]) << 8) |
           (std::uint32_t(src[2]) << 16) |
           (std::uint32_t(src[3]) << 24);
}

bool read_exact(std::istream& in, void* data, std::size_t size)
{
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.good() || in.gcount() == static_cast<std::streamsize>(size);
}

}

bool write_raw_frame(std::ostream& out, RpcFrameType type, int id, const void* data, std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return false;

    std::array<unsigned char, k_header_size> header{};
    write_u32_le(header.data() + 0, k_magic);
    write_u32_le(header.data() + 4, static_cast<std::uint32_t>(type));
    write_u32_le(header.data() + 8, static_cast<std::uint32_t>(id));
    write_u32_le(header.data() + 12, static_cast<std::uint32_t>(size));

    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!out.good())
        return false;

    if (size != 0) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out.good())
            return false;
    }

    out.flush();
    return out.good();
}

bool read_raw_frame(std::istream& in, RawRpcFrame& frame, std::string& error)
{
    std::array<unsigned char, k_header_size> header{};
    if (!read_exact(in, header.data(), header.size())) {
        error = "failed to read frame header";
        return false;
    }

    if (read_u32_le(header.data() + 0) != k_magic) {
        error = "invalid frame magic";
        return false;
    }

    frame.type = static_cast<RpcFrameType>(read_u32_le(header.data() + 4));
    frame.id = static_cast<int>(read_u32_le(header.data() + 8));
    const auto size = static_cast<std::size_t>(read_u32_le(header.data() + 12));

    frame.payload.assign(size, 0);
    if (size != 0 && !read_exact(in, frame.payload.data(), size)) {
        error = "failed to read frame payload";
        frame.payload.clear();
        return false;
    }

    return true;
}

bool write_json_frame(std::ostream& out, RpcFrameType type, int id, const nlohmann::json& payload)
{
    const auto dumped = payload.dump();
    return write_raw_frame(out, type, id, dumped.data(), dumped.size());
}

bool read_json_frame(const RawRpcFrame& frame, nlohmann::json& payload, std::string& error)
{
    try {
        payload = nlohmann::json::parse(frame.payload.begin(), frame.payload.end());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool write_request_frame(std::ostream& out, const RpcFrame& frame)
{
    nlohmann::json payload;
    payload["method"] = frame.method;
    payload["payload"] = frame.payload;
    return write_json_frame(out, RpcFrameType::json_request, frame.id, payload);
}

bool read_request_frame(const RawRpcFrame& raw, RpcFrame& frame, std::string& error)
{
    if (raw.type != RpcFrameType::json_request) {
        error = "unexpected frame type for request";
        return false;
    }

    nlohmann::json payload;
    if (!read_json_frame(raw, payload, error))
        return false;

    frame.id = raw.id;
    frame.method = payload.value("method", std::string());
    frame.payload = payload.contains("payload") ? payload["payload"] : nlohmann::json::object();
    return true;
}

}
