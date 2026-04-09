# RouteASCII — TUI Driving Route Plotter

A terminal-based (TUI) map viewer and driving route plotter for Windows `conhost` / Windows Terminal.
Rasterizes **Natural Earth vector data** (coastlines, country/state borders, lakes, rivers, cities) into
ASCII art with Web-Mercator projection, and overlays OSRM-computed driving routes on top.

## Features

- **Vector ASCII Rendering** — land polygons scanline-filled, coastlines and borders stroked via
  Bresenham with slope-aware characters, Cohen-Sutherland clipping, antimeridian handling, and
  per-feature bbox culling
- **Three LODs** — Natural Earth 1:110m / 1:50m / 1:10m, auto-selected by zoom level
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
│   ├── main.c              # Entry point, input loop, UI orchestration
│   ├── map.c / map.h       # Layer draw order, viewport math, overlay
│   ├── render.c / render.h # Scanline polygon fill, Bresenham + CS clip,
│   │                       # projection, graticule, place markers
│   ├── geodata.c/.h        # Natural Earth GeoJSON loader (cJSON-based),
│   │                       # LOD selection, on-demand file bootstrap
│   ├── route.c / route.h   # OSRM API calls, polyline decoding, directions
│   ├── geo.c / geo.h       # Nominatim geocoding, Mercator math
│   ├── ui.c / ui.h         # Curses UI panels, status bar, input dialogs
│   └── http.c / http.h     # libcurl wrapper for HTTP requests
├── data/                   # Natural Earth GeoJSON (auto-downloaded)
├── Makefile.win            # Build for Windows (PDCurses + libcurl)
├── CMakeLists.txt          # CMake cross-platform build
└── README.md
```

## Rendering pipeline

Each frame, for the current `MapView` (center lat/lon + zoom):

1. **Clear** to water (space + blue/cyan color pair).
2. **Graticule** — 10°/30° lat-lon grid on the water (low zooms only).
3. **Land polygons** — Natural Earth `ne_*_land.geojson`, scanline-filled (even-odd rule, with hole
   support from multi-ring polygons).
4. **Lakes** — re-filled as water to punch holes back through the land.
5. **Rivers** — linestrings stroked in dim cyan.
6. **Admin-1** (state/province) boundaries — dotted.
7. **Admin-0** (country) boundaries — bold.
8. **Coastlines** — stroked with slope-aware characters (`- | / \`).
9. **OSRM route polyline** — bold `*`, with `A`/`B` markers.
10. **Populated places** — ranked by Natural Earth `scalerank`, labeled at mid zoom.

All polygon and line features are bbox-culled against the viewport (with antimeridian ±360° passes)
before projection. Line segments are clipped with Cohen-Sutherland before rasterization so off-screen
features near the viewport don't loop Bresenham over millions of cells.

## Data

On first run (or when a new LOD is needed), `geodata.c` downloads the required Natural Earth files
from `github.com/nvkelso/natural-earth-vector` into a `data/` directory next to the executable. The
three resolutions are loaded lazily:

| Zoom   | LOD   | Size (approx) |
|--------|-------|---------------|
| 2–4    | 110m  | ~1 MB         |
| 5–7    | 50m   | ~10 MB        |
| 8–16   | 10m   | ~100 MB       |

Natural Earth is public domain.

## Dependencies

| Library   | Purpose                | Windows           | Linux/macOS       |
|-----------|------------------------|--------------------|--------------------|
| PDCurses  | TUI rendering          | included / vcpkg  | n/a (use ncurses)  |
| libcurl   | HTTP requests          | vcpkg / prebuilt   | apt / brew          |
| cJSON     | JSON parsing           | bundled (single-file) | bundled          |

## Building

### Windows (MSVC + vcpkg, manifest mode)
```powershell
vcpkg x-update-baseline --add-initial-baseline
vcpkg install
$vcpkgRoot = Split-Path (Get-Command vcpkg).Source -Parent
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
.\build\Release\routeascii.exe
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