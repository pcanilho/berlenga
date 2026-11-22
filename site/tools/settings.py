"""Build site/data/settings.json for the Settings page.

Reads the fork's project and Environment settings from doc/classes, and every
feature line's settings from site/content/features. Run from anywhere:
    python3 site/tools/settings.py
"""
import json, re, pathlib
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
FEAT = ROOT / 'site/content/features'

def clean(t):
    t = re.sub(r'\[(?:member|method|constant|enum|signal|param|class) ([^\]]+)\]', r'`\1`', t or '')
    t = re.sub(r'\[code\](.*?)\[/code\]', r'`\1`', t)
    t = re.sub(r'\[([A-Z]\w+)\]', r'\1', t)
    t = re.sub(r'\[/?[a-z]+[^\]]*\]', '', t)
    t = ' '.join(t.split())
    m = re.match(r'(.+?[.!?])(\s|$)', t)
    return m.group(1) if m else t

def slug(s):
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')

pages = {}
features = []
def weight(p):
    m = re.search(r'^weight:\s*(\d+)', p.read_text(), re.M)
    return int(m.group(1)) if m else 999

for p in sorted(FEAT.glob('*.md'), key=weight):
    if p.name == '_index.md':
        continue
    text = p.read_text()
    title = re.search(r'^title:\s*"(.+?)"', text, re.M).group(1)
    pages[p.stem] = (title, text)
    topic = ''
    for line in text.split('\n'):
        if line.startswith('## '):
            topic = line[3:].strip()
        m = re.match(r'- \*\*(.+?)\*\*\s*', line)
        if not m:
            continue
        name, rest, par = m.group(1), line[m.end():], ''
        if rest.startswith('('):
            depth, tick = 0, False
            for i, ch in enumerate(rest):
                if ch == '`':
                    tick = not tick
                elif not tick and ch == '(':
                    depth += 1
                elif not tick and ch == ')':
                    depth -= 1
                    if depth == 0:
                        par, rest = rest[1:i], rest[i + 1:]
                        break
        sets = re.findall(r'`([^`]+)`', par or '')
        what = re.sub(r'\s+', ' ', rest).strip().lstrip(':').strip()
        if what == '.':
            what = ''
        if not sets:
            sets = re.findall(r'`([^`]+)`', what)
        features.append({'name': name, 'settings': sets, 'note': (par or '') if not sets else '',
                         'what': what, 'page': p.stem, 'title': title, 'topic': topic})

def where(key):
    leaf = key.split('/')[-1]
    for stem, (title, text) in pages.items():
        if f'`{key}`' in text or f'`{leaf}`' in text or f'/{leaf}`' in text:
            return {'page': stem, 'title': title}
    return None

def members(xml, keep):
    out = []
    for m in ET.parse(ROOT / 'doc/classes' / xml).getroot().iter('member'):
        n = m.get('name')
        if keep(n):
            row = {'name': n, 'type': m.get('type'), 'default': m.get('default', ''), 'about': clean(m.text)}
            w = where(n)
            if w:
                row.update(w)
            out.append(row)
    return out

data = {
    'project': members('ProjectSettings.xml', lambda n: n.startswith(('rendering/pathtracing/', 'rendering/streamline/'))),
    'environment': members('Environment.xml', lambda n: n.startswith('pathtracing_')),
    'features': features,
}
out = ROOT / 'site/data/settings.json'
out.write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n')
print(out, {k: len(v) for k, v in data.items()})
