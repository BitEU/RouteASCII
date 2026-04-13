# RouteASCII — TUI Driving Route Plotter

A terminal-based (TUI) map viewer and driving route plotter for Windows `conhost` / Windows Terminal.
Rasterizes **Natural Earth vector data** (coastlines, country/state borders, lakes, rivers, cities) into
ASCII art with Web-Mercator projection, and overlays OSRM-computed driving routes on top.

## Features

- **Subpixel braille rendering** — all line primitives (coastlines, borders, rivers, roads, route)
  are rasterized into a 2×4-per-cell Unicode braille canvas, giving 8× the effective resolution
  of plain character rendering
- **Vector ASCII rendering** — land polygons scanline-filled (even-odd, with hole support),
  Cohen-Sutherland clipping, antimeridian ±360° passes, per-feature bbox culling
- **Two-tier vector data** — Natural Earth 1:110m/1:50m for world and continent views, plus
  on-demand **OpenStreetMap via Overpass API** for zoom 9+, fetched in z=13 tiles and cached to
  disk
- **Pan & Zoom** — Arrow keys to pan, +/- to zoom (levels 2–16)
- **Geocoding** — Type place names to jump to locations (via Nominatim)
- **Route Plotting** — Enter origin & destination for A→B driving routes (via OSRM)
- **Turn-by-Turn Directions** — Sidebar with maneuver list, distance & duration
- **Route Overlay** — Route polyline drawn on the ASCII map with highlighted waypoints

## Architecture

```
routeascii/
├── src/
│   ├── main.c                # Entry point, input loop, UI orchestration
│   ├── map.c / map.h         # Layer draw order, viewport math, overlay,
│   │                         # background tick loop for OSM fetches
│   ├── render.c / render.h   # Scanline polygon fill, CS clipping,
│   │                         # projection, place markers w/ collision
│   ├── subpixel.c / .h       # Unicode braille 2x4-per-cell canvas for
│   │                         # subpixel line rendering
│   ├── geodata.c / .h        # Natural Earth GeoJSON loader (cJSON),
│   │                         # LOD selection, on-demand file bootstrap
│   ├── osm.c / osm.h         # Overpass API tile fetcher, disk cache,
│   │                         # OSM way classification (roads/waterways)
│   ├── route.c / route.h     # OSRM API calls, polyline decoding
│   ├── geo.c / geo.h         # Nominatim geocoding, Mercator math
│   ├── ui.c / ui.h           # Curses UI panels, status bar, dialogs
│   └── http.c / http.h       # libcurl wrapper for HTTP requests
├── data/
│   ├── ne_*.geojson          # Natural Earth (auto-downloaded)
│   └── osm_cache/13/X/Y.json # Overpass tile cache (auto-populated)
├── Makefile.win            # Build for Windows (PDCurses + libcurl)
├── CMakeLists.txt          # CMake cross-platform build
└── README.md
```

## Rendering pipeline

Each frame, for the current `MapView` (center lat/lon + zoom):

1. **Begin frame** — reset the braille subpixel canvas (2×4 dots per cell) that
   accumulates all stroked line work for this frame.
2. **Clear** to water (space + blue/cyan color pair).
3. **Graticule** — 10°/30° lat-lon grid on the water (low zooms only). Drawn
   before land so land fills overwrite it.
4. **Land polygons** — Natural Earth `ne_*_land.geojson`, scanline-filled
   (even-odd rule, with hole support from multi-ring polygons).
5. **Lakes** — re-filled as water to punch holes back through the land.
6. **Rivers** — linestrings stroked into the braille canvas in dim cyan.
7. **Admin-1** (state/province) boundaries — dotted.
8. **Admin-0** (country) boundaries — bold.
9. **Coastlines** — stroked into the braille canvas.
10. **OSM overlay** (zoom ≥ 9) — Overpass-fetched waterbodies, waterways, and
    road networks (major vs. minor) stroked into the braille canvas. Tile
    requests are queued by `osm_request_viewport()`; the background
    `map_tick()` loop fetches one z=13 tile per input timeout.
11. **OSRM route polyline** — bold `*` in the route color pair.
12. **End frame** — encode the braille canvas to UTF-8 `U+2800..U+28FF` glyphs
    and blit onto the curses window on top of the fills from steps 4–5.
13. **A/B markers + populated places** — drawn *after* the braille flush so
    their text glyphs sit on top of coastlines and route lines. Cities are
    sorted by Natural Earth `scalerank` and labeled with a collision-avoiding
    occupancy bitmap (tries right / left / below the marker before giving up).

All polygon and line features are bbox-culled against the viewport before
projection. A `needed_passes()` bitmask decides whether ±360° antimeridian
lon-offset passes are actually required for the current viewport — most views
only run a single pass. Line segments are clipped with Cohen-Sutherland before
rasterization so off-screen features near the viewport don't loop Bresenham
over millions of pixels.

## Data

### Natural Earth (base map)

On first run (or when a new LOD is needed), `geodata.c` downloads the required
Natural Earth files from `github.com/nvkelso/natural-earth-vector` into a
`data/` directory next to the executable. LODs are loaded lazily:

| Zoom   | LOD   | Size (approx) | Loaded                          |
|--------|-------|---------------|---------------------------------|
| 2–4    | 110m  | ~1 MB         | at startup                      |
| 5–8    | 50m   | ~10 MB        | first time the user zooms to 5+ |
| 9–16   | 10m   | ~100 MB       | not auto-loaded (too slow)      |

Natural Earth is public domain.

### OpenStreetMap (high-zoom detail)

At zoom 9 and above, the base map alone looks empty — Natural Earth has no
roads and only coarse waterways. `osm.c` fills this in by querying the
**Overpass API** (`overpass-api.de/api/interpreter`) for real OSM ways in
z=13 slippy tiles (~4 km squares at the equator) covering the current
viewport. Each Overpass response is cached to disk at
`data/osm_cache/13/{x}/{y}.json` and parsed into road / waterway / water
polygon layers.

Tiles are fetched **in the background** — one tile per `getch()` timeout via
`map_tick()` — so panning and zooming stay responsive while new detail
populates. The viewport request is capped at 32 tiles per frame to avoid
runaway Overpass requests on world-scale pans.

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

## Convert *osm.pbf to usable data

1. Download Windows tilemaker from https://github.com/systemed/tilemaker/releases
2. Run ```java -Xmx128g -jar planetiler.jar --osm-path=us-260408.osm.pbf --output=data/us.mbtiles --download --force```

## License

MIT