//
// Created by zagym on 09/08/2026.
//

#ifndef PROGRESSIV_LOADER_H
#define PROGRESSIV_LOADER_H

#include <fstream>
#include <string>

enum write_mode
{
    override,
    append
};

namespace loader
{
    std::string load_file(std::string path);
    void write_file(std::string path, const std::string& content, write_mode mode);
}

#endif //PROGRESSIV_LOADER_H
