#include <iostream>
#include <exception>
#ifdef _WIN32
#include <windows.h>
#endif
#include "Progressiv.h"

namespace
{
    void keep_console_open_on_error()
    {
#ifdef _WIN32
        if (GetConsoleWindow() != nullptr)
        {
            std::cerr << "\nPress Enter to exit...\n" << std::flush;
            std::cin.get();
        }
#endif
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try
    {
        Progressiv app;
        std::cerr << "Progressiv starting...\n";
        app.init();
        std::cerr << "Progressiv init OK, entering loop\n";
        app.run();
        app.destroy();
        std::cerr << "Progressiv stopped\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "fatal: " << e.what() << std::endl;
        keep_console_open_on_error();
        return 1;
    }
    catch (...)
    {
        std::cerr << "fatal: unknown exception\n";
        keep_console_open_on_error();
        return 1;
    }
}
