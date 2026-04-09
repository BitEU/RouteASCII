cmake_minimum_required(VERSION 3.14)
project(routeascii C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Sources
set(SOURCES
    src/main.c
    src/http.c
    src/geo.c
    src/geodata.c
    src/render.c
    src/subpixel.c
    src/osm.c
    src/map.c
    src/route.c
    src/ui.c
    src/cjson.c
)

add_executable(routeascii ${SOURCES})

# Platform-specific configuration
if(WIN32)
    # Windows: use PDCurses
    find_package(unofficial-pdcurses CONFIG QUIET)
    if(unofficial-pdcurses_FOUND)
        target_link_libraries(routeascii PRIVATE unofficial::pdcurses::pdcurses)
    else()
        # Try to find PDCurses manually
        find_library(PDCURSES_LIB pdcurses NAMES pdcurses curses)
        find_path(PDCURSES_INCLUDE curses.h)
        if(PDCURSES_LIB AND PDCURSES_INCLUDE)
            target_include_directories(routeascii PRIVATE ${PDCURSES_INCLUDE})
            target_link_libraries(routeascii PRIVATE ${PDCURSES_LIB})
        else()
            message(FATAL_ERROR "PDCurses not found. Install via vcpkg: vcpkg install pdcurses")
        endif()
    endif()
else()
    # Linux/macOS: use ncurses
    find_package(Curses REQUIRED)
    target_include_directories(routeascii PRIVATE ${CURSES_INCLUDE_DIR})
    target_link_libraries(routeascii PRIVATE ${CURSES_LIBRARIES})
endif()

# libcurl
find_package(CURL REQUIRED)
target_include_directories(routeascii PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(routeascii PRIVATE ${CURL_LIBRARIES})

# Math library (Linux)
if(UNIX)
    target_link_libraries(routeascii PRIVATE m)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(routeascii PRIVATE /W4)
else()
    target_compile_options(routeascii PRIVATE -Wall -Wextra -Wpedantic)
endif()