# Local client map artwork

The dashboard can show continent and outdoor zone maps extracted from an installed 3.3.5a client. It draws the
player icons last, above terrain, labels and the grid. Pan/zoom applies the same world-coordinate transform to both.
Eastern Kingdoms, Kalimdor, Outland and Northrend remain selectable even without resident bots. Choose a named zone,
the whole continent, or automatic selection based on the selected/first bot. Draenei starting islands use their own
zone artwork despite sharing server map 530 with Outland. Dungeon floors without calibrated artwork use coordinates.

Extraction is optional and separate from the dependency-free bridge. With authorization to prepare local assets:

```sh
python3 -m venv /srv/observatory/map-tools
/srv/observatory/map-tools/bin/pip install -r apps/observatory/requirements-map-extractor.txt
/srv/observatory/map-tools/bin/python apps/observatory/extract_maps.py \
  --client /path/to/World-of-Warcraft \
  --dbc /srv/observatory/data/dbc/WorldMapArea.dbc \
  --output /srv/observatory/map-art
python3 apps/observatory/bridge.py --spool /srv/observatory/runs/run-001 \
  --token-file /srv/observatory/token --maps /srv/observatory/map-art
```

On a host without `pip` or `python3-venv` the first line fails with `ensurepip is not available`; create the
environment with `python3 -m venv --without-pip` and bootstrap pip into it from `https://bootstrap.pypa.io/get-pip.py`
rather than installing system packages or borrowing an unrelated project's virtualenv.

Use a new output directory. The extractor reads MPQs without modifying them, respects patch precedence, decodes BLP
textures to PNG, and records coordinates from the server's DBC. `WorldMapOverlay.dbc` must be beside WorldMapArea.dbc;
its exploration details supply roads, terrain and place labels. The artwork depicts the complete map regardless of
what the bot has explored. It does not grant exploration XP or modify gameplay. The texture layout is the client's
1002×668 map frame with 256-pixel base tiles and cropped edge/detail tiles. High-resolution texture replacements
use the same display geometry.

The local client used for validation produced 76 complete maps. A different client may supply fewer; incomplete
base maps are omitted and coordinate rendering remains available. Extracted artwork belongs to the game client
(World of Warcraft / Blizzard Entertainment); keep it outside Git. The source repository contains only extraction
and rendering code. The bridge serves the generated PNGs behind the existing token; it never serves MPQ archives.

Refresh the dashboard after installing this update. **Artwork** toggles the background; **Fit map** frames the map
and **Fit bots** zooms to the visible cohort. Blue player icons and their selected-bot labels remain above the artwork.
