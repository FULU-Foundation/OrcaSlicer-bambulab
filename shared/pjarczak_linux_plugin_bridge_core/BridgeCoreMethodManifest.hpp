#pragma once

#include "BridgeCoreTypes.hpp"

namespace PJarczak::LinuxPluginBridgeCore {

const std::vector<MethodManifestEntry>& method_manifest();

std::optional<MethodManifestEntry> find_method_manifest_entry(const std::string& exported_name);

nlohmann::json method_manifest_as_json();

}
