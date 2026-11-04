#!/usr/bin/env python3
"""Build the public camera catalogue using only repository data and stdlib."""
import argparse
from html import escape
import json
from pathlib import Path
import re
import shutil

from profiles import ROOT, load


def html(value):
    return escape(str(value), quote=True)


def page(title, body, repository, version, prefix=''):
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html(title)} · Camera firmware</title>
<link rel="icon" href="{prefix}assets/favicon.svg">
<link rel="stylesheet" href="{prefix}assets/style.css">
<script src="{prefix}assets/app.js" defer></script></head>
<body><a class="skip" href="#main">Skip to content</a>
<header class="topbar"><div class="container">
<a class="brand" href="{prefix}index.html">Camera firmware</a>
<nav aria-label="Main navigation"><a href="{prefix}index.html">Cameras</a>
<a href="{prefix}guide.html">Build guide</a>
<a href="{html(repository)}">Source</a><span class="version">v{html(version)}</span></nav>
</div></header><main id="main">{body}</main>
<footer><div class="container"><span>Camera firmware · v{html(version)}</span>
<a href="{html(repository)}">View repository</a></div></footer>
<div id="toast" class="toast" role="status" hidden></div></body></html>
'''


def photo(metadata, ptz, prefix=''):
    return f'''<div class="illustration product-photo">
<img src="{prefix}{html(metadata['image'])}" alt="{html(metadata['image_alt'])}">
<span class="kind">{'Pan and tilt' if ptz else 'Fixed view'}</span></div>'''


def specs(profile):
    board, soc = profile['board'], profile['soc']
    values = [('Chip', soc['chip']), ('Sensor', board['sensor'].upper()),
              ('CPU', soc['cpu']), ('Flash', f"{board['flash_bytes'] // 1048576} MiB")]
    return '<dl class="specs">' + ''.join(
        f'<div><dt>{html(key)}</dt><dd>{html(value)}</dd></div>' for key, value in values
    ) + '</dl>'


def config_box(board):
    name = html(board['id'])
    return f'''<div class="config-box"><div><small>Configuration</small><code>{name}</code></div>
<button class="copy" type="button" data-copy="{name}" aria-label="Copy configuration {name}">Copy</button></div>'''


def build(repository, output=ROOT / 'output/site'):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) or any(
        part in ('.', '..') for part in repository.split('/')
    ):
        raise ValueError('Repository must be an owner/name pair')
    repo_url = 'https://github.com/' + repository
    metadata = json.loads((ROOT / 'site/metadata.json').read_text())
    if metadata.get('schema') != 1:
        raise ValueError('Unsupported site metadata schema')
    profiles = [load(path) for path in sorted((ROOT / 'boards').glob('*.json'))]
    if not profiles:
        raise ValueError('No camera profiles found')
    for profile in profiles:
        entry = metadata['cameras'][profile['board']['id']]
        image = (ROOT / 'site' / entry['image']).resolve()
        if not image.is_relative_to((ROOT / 'site/assets').resolve()) or not image.is_file():
            raise ValueError('Camera image must exist inside site/assets')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / 'site/assets', output / 'assets', dirs_exist_ok=True)
    version = profiles[0]['project']['version']
    cards = []
    for profile in profiles:
        board, soc = profile['board'], profile['soc']
        entry = metadata['cameras'][board['id']]
        ptz = board['capabilities']['ptz']
        search = ' '.join([board['name'], board['id'], soc['id'], soc['chip'],
                           board['sensor'], entry['summary']]).lower()
        tags = ''.join(f'<span class="tag">{html(key.replace("_", " "))}</span>'
                       for key, enabled in board['capabilities'].items() if enabled)
        cards.append(f'''<article class="camera-card" data-camera="{html(board['id'])}"
data-search="{html(search)}" data-chip="{html(soc['id'])}" data-ptz="{str(ptz).lower()}">
{photo(entry, ptz)}<div class="card-content"><div class="maker">LSC Smart Connect</div>
<h2>{html(board['name'])}</h2><p class="description">{html(entry['summary'])}</p>
{specs(profile)}{config_box(board)}<div class="features">{tags}</div>
<div class="card-footer"><span class="status">Test candidate · boot evidence required</span>
<a class="detail-link" href="cameras/{html(board['id'])}.html">Camera details →</a></div></div></article>''')
        commands = (f"python3 tools/toolchain.py --board {board['id']}\n"
                    f"./compile-sd.sh --config {board['id']}")
        rows = ''.join(f'<tr><th scope="row">{html(name)}</th><td><code>{html(json.dumps(value, sort_keys=True))}</code></td></tr>'
                       for name, value in board['gpio'].items())
        detail = f'''<section class="hero container">
<div class="breadcrumb"><a href="../index.html">Cameras</a> / {html(board['name'])}</div>
<div class="detail-hero"><div><div class="eyebrow">Camera profile</div>
<h1>{html(board['name'])}</h1><p class="description">{html(entry['summary'])}</p></div>
{photo(entry, ptz, '../')}</div></section>
<div class="container"><div class="callout"><strong>Identify the hardware first.</strong> {html(entry['identification'])}</div>
<div class="detail-grid"><section class="panel"><h2>Hardware</h2>{specs(profile)}{config_box(board)}</section>
<section class="panel"><h2>Firmware support</h2><p>{html(entry['backend_note'])}</p>
<p>Builds produce test candidates. Installing a release requires exact-image boot qualification.</p></section>
<section class="panel full"><h2>Build this configuration</h2>
<p>First prepare the original firmware inputs and signing key using the build guide. Toolchain installation uses the bundled archive offline.</p>
<pre><code>{html(commands)}</code></pre><a class="button" href="../guide.html">Build guide</a></section>
<section class="panel full"><h2>GPIO configuration</h2><div class="table-scroll"><table>
<thead><tr><th>Feature</th><th>Profile settings</th></tr></thead><tbody>{rows}</tbody></table></div>
<div class="links"><a class="button" href="{repo_url}/blob/HEAD/boards/{html(board['id'])}.json">Board profile</a></div></section>
</div></div>'''
        camera_dir = output / 'cameras'
        camera_dir.mkdir(exist_ok=True)
        (camera_dir / (board['id'] + '.html')).write_text(
            page(board['name'], detail, repo_url, version, '../'), encoding='utf-8')
    chips = {p['soc']['id']: p['soc']['chip'] for p in profiles}
    options = ''.join(f'<option value="{html(key)}">{html(value)}</option>' for key, value in sorted(chips.items()))
    catalogue = f'''<section class="hero container"><div class="eyebrow">Supported hardware</div>
<h1>Find your camera.</h1><p>Explore camera configurations and build firmware for your verified hardware revision.</p></section>
<div class="container"><div class="callout"><strong>Check the chip and sensor revision.</strong> The product name or enclosure alone does not establish compatibility.</div>
<form class="filters" role="search"><div class="search"><label for="search">Search cameras</label>
<input id="search" type="search" placeholder="Camera, chip or sensor"></div>
<div><label for="chip">Chip</label><select id="chip"><option value="">All chips</option>{options}</select></div>
<div class="toggle"><input id="ptz-only" type="checkbox"><label for="ptz-only">Pan and tilt only</label></div></form>
<div class="results-bar"><span id="result-count" role="status">{len(profiles)} cameras</span>
<button id="clear-filters" class="text-button" type="button">Clear filters</button></div>
<div class="cards">{''.join(cards)}</div><div id="empty" class="empty" hidden><h2>No matching cameras</h2><p>Try clearing the filters.</p></div>
<section class="guide-banner"><div><h2>Build your firmware</h2><p>Prepare original inputs, keep your signing key, and compile a candidate.</p></div>
<a class="button primary" href="guide.html">Open build guide</a></section></div>'''
    (output / 'index.html').write_text(page('Camera catalogue', catalogue, repo_url, version), encoding='utf-8')
    guide = f'''<article class="article"><div class="eyebrow">Getting started</div><h1>Build guide</h1>
<p>Use Linux and Python 3.12 or newer. Clone the repository and follow its README for dependencies and commands.</p>
<h2>1. Identify your hardware</h2><p>Match the chip, sensor and original firmware revision to a camera profile.</p>
<h2>2. Prepare the build</h2><p>Install the bundled toolchain offline, import the required original firmware files, and create or reuse your camera signing key. Keep the private key backed up.</p>
<h2>3. Compile and qualify</h2><p>Build an SD or web update candidate for the selected configuration. Follow the update documentation before installing it. Compiling does not flash the camera.</p>
<div class="links"><a class="button primary" href="{repo_url}/blob/HEAD/README.md">Build instructions</a>
<a class="button" href="{repo_url}/blob/HEAD/docs/INPUTS.md">Original inputs and keys</a>
<a class="button" href="{repo_url}/blob/HEAD/docs/UPDATES.md">Updates and qualification</a></div></article>'''
    (output / 'guide.html').write_text(page('Build guide', guide, repo_url, version), encoding='utf-8')
    (output / '.nojekyll').touch()
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', required=True, help='GitHub owner/repository')
    parser.add_argument('--output', type=Path, default=ROOT / 'output/site')
    args = parser.parse_args()
    print('Built public catalogue:', build(args.repository, args.output))


if __name__ == '__main__':
    main()
