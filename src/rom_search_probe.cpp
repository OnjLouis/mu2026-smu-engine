// license:BSD-3-Clause
#include "rom_search.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("smu-rom-pointer-probe-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "portable");
    const auto pointer = root / "portable" / "roms.txt";
    const auto check = [&](const std::string &text, const fs::path &expected) {
        std::ofstream(pointer, std::ios::binary) << text;
        const auto actual = fs::path(smu2000::read_roms_pointer(pointer.string()));
        if (actual != expected) {
            std::cerr << "expected " << expected << ", got " << actual << '\n';
            return false;
        }
        return true;
    };
    bool passed = check("\xef\xbb\xbf../roms \r\n", (root / "roms").lexically_normal());
    passed &= check((root / "absolute").string() + "\n", root / "absolute");
    passed &= check("\n", fs::path());
    fs::remove(pointer);
    fs::remove(root / "portable");
    fs::remove(root);
    return passed ? 0 : 1;
}
