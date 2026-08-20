//
// Created by zagym on 09/08/2026.
//
#include "loader.h"

#include <sstream>

namespace loader
{
    std::string load_file(std::string path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    void write_file(std::string path, const std::string& content, write_mode mode)
    {
        auto flags = std::ios::binary;
        flags |= (mode == append) ? std::ios::app : std::ios::trunc;

        std::ofstream file(path, flags);
        if (!file.is_open())
            return;

        file << content;
    }
}
