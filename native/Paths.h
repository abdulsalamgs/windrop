// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <filesystem>
#include <array>
#include <stdexcept>
inline std::filesystem::path executable_directory() {
    std::array<wchar_t,32768> path{};
    DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n==path.size()) throw std::runtime_error("Cannot locate native worker");
    return std::filesystem::path(path.data()).parent_path();
}
