#!/usr/bin/env python3
"""Validate the user-requested70-feature ledger; incomplete entries fail by default."""
import argparse
import json
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-incomplete',action='store_true')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
features=json.loads((root/'docs/features.json').read_text())['features']
assert [x['id'] for x in features]==list(range(1,71)), 'Feature IDs must cover1-70 exactly'
assert len({x['name'] for x in features})==70
missing=[]
for row in features:
    if row['status']!='verified': missing.append(row)
    else:
        assert row['implementation'] and row['tests'] and row['evidence'], f"Missing proof for{row['id']}"
        for path in row['implementation']+row['tests']:
            assert (root/path.split(':')[0]).is_file(), f'Missing evidence file{path}'
print(f'{70-len(missing)}/70 feature entries verified')
for row in missing: print(f"{row['id']:02d} {row['status']}: {row['name']}")
if missing and not args.allow_incomplete: raise SystemExit(1)
