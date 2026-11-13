#!/usr/bin/env python3
"""Assemble loadable Chromium and Firefox extension directories."""
from pathlib import Path
import shutil
import argparse

root = Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=root.parent / 'build' / 'browser')
args=parser.parse_args()
for browser in ('chromium', 'firefox'):
    output = args.output / browser
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / browser / 'manifest.json', output / 'manifest.json')
    for name in ('background.js', 'content.js', 'popup.js', 'popup.html', 'popup.css'):
        shutil.copyfile(root / name, output / name)
    shutil.copytree(root / 'icons', output / 'icons', dirs_exist_ok=True)
    shutil.copytree(root / 'fonts', output / 'fonts', dirs_exist_ok=True)
    print(output)
