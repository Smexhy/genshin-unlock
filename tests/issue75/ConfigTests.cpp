#include "plugin/Config.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using z3lx::plugin::Config;
static unsigned checks = 0;
void Check(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
    ++checks;
    std::cout << "PASS " << text << '\n';
}
void Read(Config& c, std::string_view json) {
    c.Deserialize(std::vector<uint8_t>(json.begin(), json.end()));
}
std::string Write(Config& c) {
    std::vector<uint8_t> b;
    c.Serialize(b);
    return { b.begin(), b.end() };
}
int main() try {
    Config c;
    Check(!c.fixBurstFov && c.burstFovDelayMs == 1600, "opt-in / 1600ms defaults");
    Read(c, R"({"unlockFps":true,"targetFps":240,"autoThrottle":true,
        "unlockFov":true,"targetFov":110,"fovPresets":[45,90,110],
        "fovSmoothing":0.125,"unlockFovKey":"DownArrow",
        "nextFovPresetKey":"RightArrow","prevFovPresetKey":"LeftArrow"})");
    Check(c.targetFps==240 && c.targetFov==110 && !c.fixBurstFov && c.burstFovDelayMs==1600,
          "legacy config keeps supplied settings and defaults new fields");
    Read(c, R"({"fixBurstFov":true,"burstFovDelayMs":1800,"targetFov":90})");
    Check(c.fixBurstFov && c.burstFovDelayMs==1800, "new fields load");
    std::string encoded = Write(c);
    Config round;
    Read(round, encoded);
    Check(round.fixBurstFov && round.burstFovDelayMs==1800, "new fields round-trip");
    for(const char* invalid : {
        R"({"fixBurstFov":false,"burstFovDelayMs":-1})",
        R"({"fixBurstFov":false,"burstFovDelayMs":10001})",
        R"({"burstFovDelayMs":"1600"})",
        R"({"burstFovDelayMs":1600.5})",
        R"({"burstFovDelayMs":true})",
        R"({"fixBurstFov":"true"})",
        R"({"fixBurstFov":1})",
        R"({"fixBurstFov":true,"unexpected":0})",
        R"({"fixBurstFov":true,"targetFps":-2})",
        R"({"fixBurstFov":true,"targetFov":180})",
        R"({"fixBurstFov":true,"fovPresets":[]})",
        R"({"fixBurstFov":true,"fovSmoothing":2})",
        R"({"fixBurstFov":false,"burstFovDelayMs":)"
    }) {
        bool rejected=false;
        try { Read(c, invalid); } catch(...) { rejected=true; }
        Check(rejected, "invalid setting rejected");
        Check(Write(c)==encoded, "invalid read leaves existing config unchanged");
    }
    Read(c, R"({"fixBurstFov":false,"burstFovDelayMs":0})");
    Check(!c.fixBurstFov && c.burstFovDelayMs==0, "disabled and minimum delay accepted");
    Read(c, R"({"fixBurstFov":true,"burstFovDelayMs":10000})");
    Check(c.fixBurstFov && c.burstFovDelayMs==10000, "maximum delay accepted");
    Read(c, R"({"targetFov":75})");
    Check(!c.fixBurstFov && c.burstFovDelayMs==1600 && c.targetFov==75,
          "removed new fields reset to defaults on reload");
    std::cout << checks << " config checks passed against actual Config.cpp and pinned Glaze.\n";
    return 0;
} catch(const std::exception& ex) {
    std::cerr << "FAIL " << ex.what() << '\n';
    return 1;
}
