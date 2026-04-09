# RouteASCII — TUI Driving Route Plotter

A terminal-based (TUI) map viewer and driving route plotter for Windows `conhost` / Windows Terminal.
Renders OpenStreetMap tile data as ASCII art with driving route overlays powered by OSRM.

## Features

- **ASCII Map Rendering** — Mercator-projected world map rendered with Unicode block/braille characters
- **Pan & Zoom** — Arrow keys to pan, +/- to zoom (levels 2–16)
- **Geocoding** — Type place names to jump to locations (via Nominatim)
- **Route Plotting** — Enter origin & destination for A→B driving routes (via OSRM)
- **Turn-by-Turn Directions** — Sidebar with maneuver list, distance & duration
- **Route Overlay** — Route polyline drawn on the ASCII map with highlighted waypoints
- **Cross-platform** — Targets Windows (PDCurses) and Linux/macOS (ncurses)

## Architecture

```
routeascii/
├── src/
│   ├── main.c            # Entry point, input loop, UI orchestration
│   ├── map.c / map.h     # Tile fetching, ASCII rendering, viewport math
│   ├── route.c / route.h # OSRM API calls, polyline decoding, directions
│   ├── geo.c / geo.h     # Nominatim geocoding, coordinate utils
│   ├── ui.c / ui.h       # Curses UI panels, status bar, input dialogs
│   └── http.c / http.h   # libcurl wrapper for HTTP requests
├── Makefile.win          # Build for Windows (PDCurses + libcurl)
├── CMakeLists.txt        # CMake cross-platform build
└── README.md
```

## Dependencies

| Library   | Purpose                | Windows           | Linux/macOS       |
|-----------|------------------------|--------------------|--------------------|
| PDCurses  | TUI rendering          | included / vcpkg  | n/a (use ncurses)  |
| ncurses   | TUI rendering          | n/a                | apt / brew          |
| libcurl   | HTTP requests          | vcpkg / prebuilt   | apt / brew          |
| cJSON     | JSON parsing           | bundled (single-file) | bundled          |

## Building

### Windows (MSVC + vcpkg)
```cmd
vcpkg install pdcurses curl
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build\Release\routeascii.exe
```

### Windows (MinGW)
```cmd
make -f Makefile.win
routeascii.exe
```

## Controls

| Key          | Action                          |
|--------------|---------------------------------|
| Arrow keys   | Pan map                         |
| +  /  -      | Zoom in / out                   |
| g            | Goto — enter place name         |
| r            | Route — enter origin & dest     |
| d            | Toggle directions sidebar       |
| c            | Clear route                     |
| q / Esc      | Quit                            |

## API Usage (no keys required)

- **Map tiles**: OpenStreetMap raster tiles (respected usage policy)
- **Routing**: OSRM demo server (`router.project-osrm.org`)
- **Geocoding**: Nominatim (`nominatim.openstreetmap.org`)

> Note: The OSRM demo server is for light use. For heavy usage,
> self-host OSRM or use a commercial provider.

## License

MIT