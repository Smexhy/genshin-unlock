#include "plugin/Config.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using z3lx::plugin::Config;
using z3lx::util::VirtualKey;
static unsigned checks = 0;
void Check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
    ++checks;
    std::cout << "PASS " << text << '\n';
}
bool Read(Config& c, std::string_view json) {
    try { c.Deserialize(std::vector<uint8_t>(json.begin(), json.end())); return true; }
    catch (...) { return false; }
}
std::string Write(Config& c) {
    std::vector<uint8_t> b;
    c.Serialize(b);
    return { b.begin(), b.end() };
}
int main() try {
    Config c;
    Check(!c.fixBurstFov && c.burstFovDelayMs == 1600 && c.burstFovKey == VirtualKey::Q,
          "defaults: off, 1600 ms, Q");

    // Mirrors PersistentObject: a failed read is followed by a rewrite.
    const bool ok = Read(c, R"({"unlockFps":true,"targetFps":240,"autoThrottle":false,
        "unlockFov":true,"targetFov":110,"fovPresets":[45,90,110],
        "fovSmoothing":0.25,"unlockFovKey":"DownArrow",
        "nextFovPresetKey":"RightArrow","prevFovPresetKey":"LeftArrow"})");
    Check(!ok, "legacy config reports missing keys (triggers rewrite)");
    Check(c.targetFps == 240 && !c.autoThrottle && c.targetFov == 110 &&
          c.fovPresets.size() == 3 && c.fovSmoothing == 0.25f,
          "legacy config values kept in memory");
    Check(!c.fixBurstFov && c.burstFovDelayMs == 1600 && c.burstFovKey == VirtualKey::Q,
          "new fields default after legacy read");
    const std::string upgraded = Write(c);
    std::cout << upgraded << '\n';
    Check(upgraded.find("\"burstFovKey\"") != std::string::npos &&
          upgraded.find("\"fixBurstFov\"") != std::string::npos,
          "rewritten file contains new fields");
    Config reloaded;
    Check(Read(reloaded, upgraded), "upgraded file loads cleanly");
    Check(reloaded.targetFps == 240 && reloaded.targetFov == 110,
          "upgraded file keeps user values");

    std::string custom = upgraded;
    auto replace = [&](std::string_view from, std::string_view to) {
        const auto pos = custom.find(from);
        if (pos == std::string::npos) throw std::runtime_error("pattern not found");
        custom.replace(pos, from.size(), to);
    };
    replace("\"fixBurstFov\": false", "\"fixBurstFov\": true");
    replace("\"burstFovDelayMs\": 1600", "\"burstFovDelayMs\": 1800");
    replace("\"burstFovKey\": \"Q\"", "\"burstFovKey\": \"E\"");
    Config custom1;
    Check(Read(custom1, custom), "custom burst settings load");
    Check(custom1.fixBurstFov && custom1.burstFovDelayMs == 1800 &&
          custom1.burstFovKey == VirtualKey::E, "custom burst settings applied");

    std::string bad = upgraded;
    bad.replace(bad.find("\"burstFovDelayMs\": 1600"), 23, "\"burstFovDelayMs\": 10001");
    Config bad1;
    Check(!Read(bad1, bad), "out-of-range delay rejected");

    std::cout << checks << " config checks passed.\n";
    return 0;
} catch (const std::exception& ex) {
    std::cerr << "FAIL " << ex.what() << '\n';
    return 1;
}
