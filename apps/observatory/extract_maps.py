#!/usr/bin/env python3
"""Extract local client map tiles, calibrated with the server's WorldMapArea.dbc.

Requires Pillow and mpyq. Artwork stays outside the source repository.
"""
import argparse
import io
import json
from pathlib import Path
import struct


def overlays(path):
    data = Path(path).read_bytes()
    magic, count, fields, size, strings_size = struct.unpack_from('<4s4I', data)
    if magic != b'WDBC' or fields != 17 or size != 68 or len(data) != 20 + count * size + strings_size:
        raise ValueError('Expected the 3.3.5a WorldMapOverlay.dbc format')
    strings = data[20 + count * size:]
    result = {}
    for index in range(count):
        values = struct.unpack_from('<17I', data, 20 + index * size)
        name = strings[values[8]:].split(b'\0', 1)[0].decode('utf-8')
        if name and values[9] and values[10]:
            result.setdefault(values[1], []).append({
                'id': values[0], 'name': name, 'width': values[9], 'height': values[10],
                'left': values[11], 'top': values[12]})
    return result


def areas(path):
    data = Path(path).read_bytes()
    magic, count, fields, size, strings_size = struct.unpack_from('<4s4I', data)
    if magic != b'WDBC' or fields != 11 or size != 44 or len(data) != 20 + count * size + strings_size:
        raise ValueError('Expected the 3.3.5a WorldMapArea.dbc format')
    strings = data[20 + count * size:]
    for index in range(count):
        record = data[20 + index * size:20 + (index + 1) * size]
        identifier, map_id, zone, name_offset = struct.unpack_from('<4I', record)
        y1, y2, x1, x2 = struct.unpack_from('<4f', record, 16)
        name = strings[name_offset:].split(b'\0', 1)[0].decode('utf-8')
        if map_id not in (0, 1, 530, 571) or x1 <= x2 or y1 <= y2:
            continue
        yield {'id': identifier, 'map': map_id, 'zone': zone, 'name': name,
               'x1': x1, 'x2': x2, 'y1': y1, 'y2': y2}


def main():
    from mpyq import MPQArchive
    from PIL import Image

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--dbc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--locale', default='enUS')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    directory = args.client / 'Data'
    # Latest patches take precedence, followed by expansion/base assets. Never modify the client.
    candidates = list(directory.glob('*.MPQ')) + list(directory.glob('*.mpq'))
    candidates += list((directory / args.locale).glob('*.MPQ'))
    candidates += list((directory / args.locale).glob('*.mpq'))
    def priority(path):
        name = path.name.lower()
        return ('patch' in name, 'lichking' in name, 'expansion' in name, name)
    archives = []
    for path in sorted(set(candidates), key=priority, reverse=True):
        try:
            archives.append(MPQArchive(str(path), listfile=False))
        except (ValueError, NotImplementedError) as error:
            print(f'Skipping unsupported archive {path.name}: {error}')
    catalog = []
    details = overlays(args.dbc.with_name('WorldMapOverlay.dbc'))
    def read(path):
        return next((data for archive in archives if (data := archive.read_file(path))), None)
    try:
        for area in areas(args.dbc):
            tiles = []
            for tile in range(1, 13):
                name = area['name']
                path = f'Interface\\WorldMap\\{name}\\{name}{tile}.blp'
                content = read(path)
                if content is None:
                    break
                filename = f"{area['id']}-{tile}.png"
                with Image.open(io.BytesIO(content)) as source:
                    source.convert('RGBA').save(args.output / filename)
                tiles.append(filename)
            if len(tiles) == 12:
                layers = []
                for detail in details.get(area['id'], []):
                    columns = (detail['width'] + 255) // 256
                    rows = (detail['height'] + 255) // 256
                    for row in range(rows):
                        for column in range(columns):
                            tile = row * columns + column + 1
                            path = f"Interface\\WorldMap\\{area['name']}\\{detail['name']}{tile}.blp"
                            content = read(path)
                            if content is None:
                                continue
                            filename = f"{area['id']}-{detail['id']}-{tile}.png"
                            with Image.open(io.BytesIO(content)) as source:
                                source.convert('RGBA').save(args.output / filename)
                            width = min(256, detail['width'] - column * 256)
                            height = min(256, detail['height'] - row * 256)
                            layers.append({'file': filename, 'left': detail['left'] + column * 256,
                                           'top': detail['top'] + row * 256, 'width': width, 'height': height,
                                           'cropX': width / max(16, 2 ** (width - 1).bit_length()),
                                           'cropY': height / max(16, 2 ** (height - 1).bit_length())})
                catalog.append({**area, 'tiles': tiles, 'overlays': layers,
                                'width': 1002, 'height': 668, 'tileSize': 256})
        (args.output / 'manifest.json').write_text(json.dumps({'schema': 1, 'areas': catalog}, indent=2) + '\n')
        print(f'Extracted {len(catalog)} calibrated maps into {args.output}')
    finally:
        for archive in archives:
            archive.file.close()


if __name__ == '__main__':
    main()
