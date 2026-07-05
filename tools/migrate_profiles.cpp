// This file is part of VRto3D (LGPL-3.0).
//
// One-shot: rewrite shipped game profiles' keybind names from the legacy
// VK_*/XINPUT_* spelling to the portable Key_*/Pad_* vocabulary, using the
// same vrto3d::keys::MigrateName the loader applies (so output is identical to
// a load+save round-trip). Handles '+'-joined gamepad combos token-by-token.
// Build: g++ -std=c++17 -I external/VRto3DLib/include -I external/json/single_include \
//        tools/migrate_profiles.cpp external/VRto3DLib/src/key_names.cpp -o /tmp/migrate_profiles
// (nlohmann/json.hpp is vendored at external/json/single_include/;
//  )
#include <cstdio>
#include <fstream>
#include <string>
#include <sstream>

#include <nlohmann/json.hpp>

#include "vrto3dlib/key_names.h"

static std::string MigrateMaybeCombo(const std::string& s)
{
    if (!vrto3d::keys::IsLegacyName(s))
        return s;
    if (s.find('+') == std::string::npos)
        return vrto3d::keys::MigrateName(s);
    std::string out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        if (!out.empty()) out += '+';
        out += vrto3d::keys::MigrateName(tok);
    }
    return out;
}

static bool Walk(nlohmann::ordered_json& j)
{
    bool changed = false;
    if (j.is_object()) {
        for (auto& [k, v] : j.items())
            changed |= Walk(v);
    } else if (j.is_array()) {
        for (auto& v : j)
            changed |= Walk(v);
    } else if (j.is_string()) {
        const std::string s = j.get<std::string>();
        const std::string m = MigrateMaybeCombo(s);
        if (m != s) { j = m; changed = true; }
    }
    return changed;
}

int main(int argc, char** argv)
{
    int converted = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i]);
        if (!in) { std::fprintf(stderr, "skip (open): %s\n", argv[i]); continue; }
        nlohmann::ordered_json j;
        try { in >> j; } catch (const std::exception& e) {
            std::fprintf(stderr, "skip (parse %s): %s\n", e.what(), argv[i]);
            continue;
        }
        in.close();
        if (Walk(j)) {
            std::ofstream out(argv[i]);
            out << j.dump(4) << "\n";
            std::printf("converted %s\n", argv[i]);
            ++converted;
        }
    }
    std::printf("done: %d file(s) changed\n", converted);
    return 0;
}
