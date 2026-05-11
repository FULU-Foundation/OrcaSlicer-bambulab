#pragma once

#include "BridgeCoreTypes.hpp"

namespace PJarczak::LinuxPluginBridgeCore {

std::string encode_rpc_frame(const RpcFrame& frame);
bool decode_rpc_frame(const std::string& line, RpcFrame& frame, std::string& error);

std::string encode_bridge_event(const BridgeEvent& event);
bool decode_bridge_event(const std::string& line, BridgeEvent& event, std::string& error);

}
