cmake_minimum_required(VERSION 4.2)
project(Progressiv)

set(CMAKE_CXX_STANDARD 20)

add_executable(Progressiv main.cpp
        src/Progressiv.cpp
        src/loader.cpp
        src/Binance_interface.cpp
        src/ed25519.cpp
        src/orderbook_script.cpp
        src/script.cpp)

target_include_directories(Progressiv PUBLIC ${CMAKE_SOURCE_DIR}/include)

if (WIN32)
    target_link_libraries(Progressiv PRIVATE winhttp)
    if (MINGW)
        target_link_libraries(Progressiv PRIVATE pthread)

        # MinGW 运行时 DLL：构建后自动拷到 exe 同目录
        get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(PROGRESSIV_MINGW_RUNTIME_DLLS
                libstdc++-6.dll
                libgcc_s_seh-1.dll
                libgcc_s_dw2-1.dll
                libwinpthread-1.dll)

        set(PROGRESSIV_DLLS_TO_COPY "")
        foreach (dll_name IN LISTS PROGRESSIV_MINGW_RUNTIME_DLLS)
            if (EXISTS "${MINGW_BIN_DIR}/${dll_name}")
                list(APPEND PROGRESSIV_DLLS_TO_COPY "${MINGW_BIN_DIR}/${dll_name}")
            endif ()
        endforeach ()

        if (PROGRESSIV_DLLS_TO_COPY)
            add_custom_command(TARGET Progressiv POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${PROGRESSIV_DLLS_TO_COPY}
                    $<TARGET_FILE_DIR:Progressiv>
                    COMMENT "Copying MinGW runtime DLLs next to Progressiv.exe")
        endif ()
    endif ()
endif ()
