#!/usr/bin/env python3
"""Render a runtime worldserver or module config from its .dist template plus overlay files.

Overlays use the same `Key = value` syntax as the config files, so the tracked example configs are overlays
too. A key the template defines replaces that assignment in place, keeping the template's comments and
order; a key the template lacks is appended under a marked section. Later overlays win, and `--set` wins
last. Secrets belong in an overlay outside every repository. `derive` prints the overlay that turns a
template into an existing hand-edited config, for adopting it; `--check` proves a render is equivalent to it.
"""
import argparse
from pathlib import Path
import re
import sys

ASSIGNMENT = re.compile(r'^\s*([A-Za-z0-9_.]+)\s*=\s*(.*?)\s*$')
SECRET_HINT = re.compile(r'DatabaseInfo|Password|Token|Secret', re.IGNORECASE)
APPENDED = '# Settings from overlays that the template does not define'


def parse(text):
    """Ordered Key -> value of every assignment; a later duplicate wins, as the server's reader does."""
    values = {}
    for line in text.splitlines():
        match = ASSIGNMENT.match(line)
        if match and not line.lstrip().startswith('#'):
            values[match.group(1)] = match.group(2)
    return values


def render(template, overlays, settings=()):
    """Return the rendered text and the keys that were appended because the template lacks them."""
    merged = {}
    for overlay in overlays:
        merged.update(overlay)
    for key, value in settings:
        merged[key] = value
    lines = []
    consumed = set()
    for line in template.splitlines():
        match = ASSIGNMENT.match(line)
        if match and not line.lstrip().startswith('#') and match.group(1) in merged:
            key = match.group(1)
            line = f'{key} = {merged[key]}'
            consumed.add(key)
        lines.append(line)
    appended = [key for key in merged if key not in consumed]
    if appended:
        lines += ['', '#', APPENDED, '#', ''] + [f'{key} = {merged[key]}' for key in appended]
    return '\n'.join(lines) + '\n', appended


def derive(template, current, overlays=()):
    """Overlay that turns the template plus the given overlays into the current config: every setting whose
    value differs from that baseline, including keys the baseline lacks."""
    baseline, _ = render(template, overlays)
    defaults = parse(baseline)
    return {key: value for key, value in parse(current).items() if defaults.get(key) != value}


def redact(key, value):
    return '(hidden)' if SECRET_HINT.search(key) else value


def setting(text):
    key, separator, value = text.partition('=')
    if not separator or not key.strip():
        raise argparse.ArgumentTypeError('expected KEY=VALUE')
    return key.strip(), value.strip()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    renderer = commands.add_parser('render', help='write a runtime config from a template and overlays')
    renderer.add_argument('--template', type=Path, required=True, help='the installed .conf.dist file')
    renderer.add_argument('--overlay', type=Path, action='append', default=[],
                          help='Key = value file applied in order; later files win')
    renderer.add_argument('--set', type=setting, action='append', default=[], metavar='KEY=VALUE',
                          help='single setting applied after every overlay')
    renderer.add_argument('--output', type=Path, help='file to write; stdout when omitted')
    renderer.add_argument('--check', type=Path,
                          help='existing config that the render must be equivalent to; differences fail')
    deriver = commands.add_parser('derive', help='print the overlay that turns a template into a config')
    deriver.add_argument('--template', type=Path, required=True)
    deriver.add_argument('--current', type=Path, required=True)
    deriver.add_argument('--overlay', type=Path, action='append', default=[],
                         help='overlays already in the baseline, e.g. the tracked example config')
    args = parser.parse_args(argv)

    overlays = [parse(path.read_text()) for path in args.overlay]
    if args.command == 'derive':
        for key, value in derive(args.template.read_text(), args.current.read_text(), overlays).items():
            print(f'{key} = {value}')
        return 0

    text, appended = render(args.template.read_text(), overlays, args.set)
    if args.check:
        expected, actual = parse(args.check.read_text()), parse(text)
        differences = sorted(set(expected) | set(actual))
        differences = [key for key in differences if expected.get(key) != actual.get(key)]
        for key in differences:
            print(f'{key}: current {redact(key, expected.get(key, "(unset)"))}'
                  f' -> rendered {redact(key, actual.get(key, "(unset)"))}', file=sys.stderr)
        if differences:
            print(f'{len(differences)} setting(s) differ from {args.check}', file=sys.stderr)
            return 1
    if args.output:
        args.output.write_text(text)
        print(f'{args.output}: {len(parse(text))} settings, {len(appended)} appended'
              + (f' ({", ".join(appended)})' if appended else ''), file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
