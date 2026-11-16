"""Build site/data/whatsnew.json for the What's new page.

Compares the feature lines in site/content/features between releases (git
tags build-<sha>): a bold feature name that appears is new, one whose line
text changed is updated. The first entry covers main since the latest release.
Release tags and dates come from site/data/releases.json (written by the docs
workflow from the GitHub API) or, without it, from the local build-* tags.
Needs the tags' history (fetch-depth 0). Run from anywhere:
    python3 site/tools/whatsnew.py
"""
import json, re, subprocess, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
FEAT = 'site/content/features'
OUT = ROOT / 'site/data/whatsnew.json'
KEEP = 12

def git(*a):
    return subprocess.run(['git', '-C', str(ROOT), *a], capture_output=True, text=True).stdout

def releases():
    p = ROOT / 'site/data/releases.json'
    if p.exists():
        try:
            rel = json.loads(p.read_text())
            out = [(r['tag_name'], (r.get('published_at') or '')[:10]) for r in rel
                   if r.get('tag_name', '').startswith('build-') and not r.get('draft')]
            if out:
                return out
        except (ValueError, KeyError, TypeError):
            pass
    rows = git('for-each-ref', '--sort=-creatordate', '--format=%(refname:short) %(creatordate:short)', 'refs/tags/build-*')
    return [tuple(l.split()) for l in rows.splitlines() if l.strip()]

LINE = re.compile(r'^(\s*)- \*\*(.+?)\*\*(.*)$')

_blobs = {}

def features(ref):
    """{(page, name): (title, text)} for the feature lines at ref (None = working tree), or None if the site did not exist."""
    if ref:
        rows = [l.split(None, 3) for l in git('ls-tree', f'{ref}:{FEAT}').splitlines()]
        if not rows:
            return None
        files = [(r[3], r[2]) for r in rows if len(r) == 4 and r[1] == 'blob']
    else:
        files = [(p.name, None) for p in (ROOT / FEAT).glob('*.md')]
    out = {}
    for f, blob in files:
        if not f.endswith('.md') or f == '_index.md':
            continue
        if blob:
            # Same blob in several releases: read it once (a partial clone fetches it on demand).
            if blob not in _blobs:
                _blobs[blob] = git('cat-file', '-p', blob)
            text = _blobs[blob]
        else:
            text = (ROOT / FEAT / f).read_text()
        m = re.search(r'^title:\s*"(.+?)"', text, re.M)
        title = m.group(1) if m else f[:-3]
        for line in text.split('\n'):
            m = LINE.match(line)
            if m and not m.group(1):
                out[(f[:-3], m.group(2).strip())] = (title, ' '.join(m.group(3).split()))
    return out

def diff(old, new):
    added, changed = [], []
    for k, (title, text) in new.items():
        e = {'page': k[0], 'title': title, 'name': k[1]}
        if k not in old:
            added.append(e)
        elif old[k][1] != text:
            changed.append(e)
    return added, changed

def main():
    rel = [r for r in releases() if git('rev-parse', '-q', '--verify', r[0] + '^{commit}').strip()]
    entries = []
    head = features(None)
    newer, newer_ref = head, None
    for tag, date in rel[:KEEP + 1]:
        old = features(tag)
        if old is None:
            break
        added, changed = diff(old, newer)
        entries.append({'tag': newer_ref, 'date': newer_date if newer_ref else '', 'since': tag,
                        'new': added, 'changed': changed})
        newer, newer_ref, newer_date = old, tag, date
    entries = [e for e in entries if e['new'] or e['changed']][:KEEP]
    OUT.write_text(json.dumps(entries, indent=1) + '\n')
    print(f'{OUT.relative_to(ROOT)}: {len(entries)} entries')

if __name__ == '__main__':
    main()
