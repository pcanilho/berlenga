const root = document.documentElement;
const modes = ['system', 'light', 'dark'];
const themeBtn = document.querySelector('[data-theme-toggle]');
function currentMode() { return root.dataset.theme || 'system'; }
function applyMode(m) {
  if (m === 'system') delete root.dataset.theme; else root.dataset.theme = m;
  try { m === 'system' ? localStorage.removeItem('theme') : localStorage.setItem('theme', m); } catch (e) {}
  themeBtn.dataset.mode = m;
  themeBtn.setAttribute('aria-label', 'Theme: ' + m);
  themeBtn.title = 'Theme: ' + m + ' (click to change)';
}
applyMode(currentMode());
themeBtn.addEventListener('click', () => applyMode(modes[(modes.indexOf(currentMode()) + 1) % 3]));

// Mobile menu
const menuBtn = document.querySelector('[data-menu]');
const scrim = document.querySelector('[data-scrim]');
function setMenu(open) {
  document.body.classList.toggle('nav-open', open);
  menuBtn.setAttribute('aria-expanded', String(open));
}
menuBtn.addEventListener('click', () => setMenu(!document.body.classList.contains('nav-open')));
scrim.addEventListener('click', () => setMenu(false));

// Keep the current page visible in the sidebar
document.querySelector('.side a.on')?.scrollIntoView({ block: 'center' });

// Image lightbox
const box = document.querySelector('[data-lightbox]');
const boxImg = box.querySelector('img');
const boxCap = box.querySelector('p');
function openBox(img) {
  boxImg.src = img.currentSrc || img.src;
  boxImg.alt = img.alt;
  const host = img.closest('.pair') || img;
  const cap = host.closest('p')?.nextElementSibling || host.nextElementSibling;
  boxCap.textContent = cap && cap.querySelector && cap.querySelector('em:only-child') ? cap.textContent : img.alt;
  box.showModal();
}
document.querySelectorAll('.content img').forEach((img) => {
  if (img.closest('a')) return;
  img.classList.add('zoomable');
  img.tabIndex = 0;
  const open = () => openBox(img);
  img.addEventListener('click', open);
  img.addEventListener('keydown', (e) => { if (e.key === 'Enter') open(); });
});
box.addEventListener('click', () => box.close());

// Search (Pagefind, loaded on first use)
const searchBox = document.querySelector('[data-searchbox]');
const base = document.currentScript?.dataset.pagefind || document.querySelector('script[data-pagefind]').dataset.pagefind;
let ui;
async function openSearch() {
  if (!ui) {
    await new Promise((res, rej) => {
      const s = document.createElement('script');
      s.src = base + 'pagefind-ui.js';
      s.onload = res; s.onerror = rej;
      document.head.appendChild(s);
    }).catch(() => null);
    if (window.PagefindUI) {
      ui = new window.PagefindUI({ element: '#pagefind', showSubResults: true, showImages: false, resetStyles: false, autofocus: true });
    } else {
      document.getElementById('pagefind').textContent = 'Search is available on the published site.';
      ui = true;
    }
  }
  searchBox.showModal();
  searchBox.querySelector('input')?.focus();
}
document.querySelector('[data-search]').addEventListener('click', openSearch);
searchBox.addEventListener('click', (e) => { if (e.target === searchBox) searchBox.close(); });
document.addEventListener('keydown', (e) => {
  const typing = /input|textarea|select/i.test(document.activeElement?.tagName || '');
  if ((e.key === '/' && !typing) || (e.key.toLowerCase() === 'k' && (e.metaKey || e.ctrlKey))) {
    e.preventDefault();
    openSearch();
  }
});

// Before/after pairs: find images made of two equal halves split by a plain
// vertical bar, and offer a drag slider in place of side by side.
function findSplit(img) {
  const w = img.naturalWidth, h = img.naturalHeight;
  if (!w || w / h < 1.6) return null;
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  const g = c.getContext('2d', { willReadFrequently: true });
  g.drawImage(img, 0, 0);
  let d;
  try { d = g.getImageData(0, 0, w, h).data; } catch (e) { return null; }
  const step = Math.max(1, Math.floor(h / 160));
  const flat = (x) => {
    let s = 0, s2 = 0, n = 0;
    for (let y = 0; y < h; y += step) {
      const i = (y * w + x) * 4, v = d[i] * .3 + d[i + 1] * .59 + d[i + 2] * .11;
      s += v; s2 += v * v; n++;
    }
    const m = s / n;
    return Math.sqrt(Math.max(0, s2 / n - m * m)) < 4;
  };
  const lo = Math.floor(w * .44), hi = Math.ceil(w * .56);
  let a = -1, b = -1;
  for (let x = lo; x <= hi; x++) {
    if (flat(x)) { if (a < 0) a = x; b = x; } else if (a >= 0 && b - a < 40) break;
  }
  if (a < 0) return null;
  const left = a, right = w - b - 1;
  if (Math.abs(left - right) > w * 0.02) return null;
  // Reject three-panel images: no other full-height bar inside either half.
  for (const [p, q] of [[0.22, 0.42], [0.58, 0.78]]) {
    for (let x = Math.floor(w * p); x <= Math.ceil(w * q); x++) if (flat(x) && flat(x + 1)) return null;
  }
  const half = Math.min(left, right);
  // Baked labels: white text with a dark outline near each half's top-left.
  const lum = (x, y) => { const i = (y * w + x) * 4; return d[i] * .3 + d[i + 1] * .59 + d[i + 2] * .11; };
  const label = (x0) => {
    const X = Math.floor(half * 0.7), Y = Math.floor(h * 0.2);
    const pts = [];
    for (let y = 2; y < Y - 2; y++) for (let x = x0 + 6; x < x0 + X - 2; x++) {
      if (lum(x, y) < 228) continue;
      if (Math.min(lum(x - 2, y), lum(x + 2, y), lum(x, y - 2), lum(x, y + 2)) > 40) continue;
      pts.push([x, y]);
    }
    if (pts.length < 12) return null;
    // Keep the first line of text only: rows near the top-most hit, and
    // columns joined to the left-most hit without a wide gap.
    const y1 = Math.min(...pts.map((q) => q[1]));
    const line = pts.filter((q) => q[1] <= y1 + h * 0.08).sort((p, q) => p[0] - q[0]);
    const x1 = line[0][0], gap = Math.max(8, half * 0.04);
    let x2 = x1;
    for (const q of line) { if (q[0] - x2 > gap) break; x2 = Math.max(x2, q[0]); }
    const y2 = Math.max(...line.filter((q) => q[0] <= x2).map((q) => q[1]));
    // Labels drawn on a solid box: grow to the end of the box.
    const my = Math.round((y1 + y2) / 2);
    while (x2 + 1 < x0 + half - 1 && lum(x2 + 1, my) < 30 && Math.abs(lum(x2 + 1, my) - lum(x2 + 1, Math.max(0, y1 - 1))) < 12) x2++;
    const pad = Math.max(4, Math.round(h * 0.01));
    const cw = Math.min(x2 + pad, x0 + half) - x0, ch = Math.min(h, y2 + pad);
    if (x1 - x0 > half * 0.25 || cw > half * 0.95 || ch > h * 0.16) return null;
    return { x: 3, y: 0, w: cw - 3, h: ch };
  };
  return { a, b, w, h, labels: [label(0), label(b + 1)] };
}

function makeCompare(img, split) {
  const { a, b, w, h } = split;
  const half = Math.min(a, w - b - 1);
  const fig = document.createElement('div');
  fig.className = 'compare';
  fig.style.aspectRatio = `${half} / ${h}`;
  const src = img.currentSrc || img.src;
  const layer = (x0) => {
    const el = document.createElement('div');
    el.className = 'compare-layer';
    el.style.backgroundImage = `url("${src}")`;
    el.style.backgroundSize = `${(w / half) * 100}% 100%`;
    el.style.backgroundPosition = `${(x0 / (w - half)) * 100}% 0`;
    return el;
  };
  const after = layer(b + 1), before = layer(0), base = layer(0);
  before.classList.add('before');
  // Hide the right image's own label so it never peeks out beside the handle;
  // the left image shows through there and both labels are drawn as chips.
  const R = split.labels[1];
  if (R) {
    const lw = Math.min(100, ((R.x + R.w) / half) * 100 + 5), lh = Math.min(100, ((R.y + R.h) / h) * 100 + 2);
    after.style.clipPath = `polygon(${lw}% 0, 100% 0, 100% 100%, 0 100%, 0 ${lh}%, ${lw}% ${lh}%)`;
  }
  const handle = document.createElement('div');
  handle.className = 'compare-handle';
  handle.innerHTML = '<span aria-hidden="true">‹ ›</span>';
  const range = document.createElement('input');
  range.type = 'range'; range.min = 0; range.max = 100; range.value = 50; range.step = 0.5;
  range.className = 'compare-range';
  range.setAttribute('aria-label', 'Compare: ' + img.alt);
  fig.append(base, after, before, handle, range);
  split.labels.forEach((L, k) => {
    if (!L) return;
    const x0 = k ? b + 1 : 0;
    const chip = document.createElement('div');
    const crowded = split.labels[0] && split.labels[1] && (split.labels[0].w + split.labels[1].w) > half * 0.9;
    chip.className = 'compare-label ' + (k ? 'right' : 'left') + (k && crowded ? ' low' : '');
    chip.style.width = (L.w / half) * 100 + '%';
    chip.style.aspectRatio = `${L.w} / ${L.h}`;
    chip.style.top = (L.y / h) * 100 + '%';
    chip.style.backgroundImage = `url("${src}")`;
    chip.style.backgroundSize = `${(w / L.w) * 100}% ${(h / L.h) * 100}%`;
    chip.style.backgroundPosition = `${((x0 + L.x) / (w - L.w)) * 100}% ${(L.y / Math.max(1, h - L.h)) * 100}%`;
    fig.append(chip);
  });
  const set = (p) => { fig.style.setProperty('--pos', p + '%'); };
  set(50);
  range.addEventListener('input', () => set(range.value));
  const fromPointer = (e) => {
    const r = fig.getBoundingClientRect();
    const p = Math.min(100, Math.max(0, ((e.clientX - r.left) / r.width) * 100));
    range.value = p; set(p);
  };
  let drag = false;
  fig.addEventListener('pointerdown', (e) => { drag = true; fig.setPointerCapture(e.pointerId); fromPointer(e); });
  fig.addEventListener('pointermove', (e) => { if (drag || e.pointerType === 'mouse') fromPointer(e); });
  fig.addEventListener('pointerup', () => { drag = false; });
  return fig;
}

const pairMode = (() => { try { return localStorage.getItem('pairs') || 'slider'; } catch (e) { return 'slider'; } })();
function setupPair(img) {
  const split = findSplit(img);
  if (!split) return;
  const wrap = document.createElement('div');
  wrap.className = 'pair';
  img.replaceWith(wrap);
  const cmp = makeCompare(img, split);
  const bar = document.createElement('div');
  bar.className = 'pair-bar';
  bar.innerHTML = '<button type="button" data-v="slider">Slider</button><button type="button" data-v="side">Side by side</button><button type="button" data-v="zoom" aria-label="Enlarge">⤢</button>';
  wrap.append(cmp, img, bar);
  const show = (m) => {
    wrap.dataset.mode = m;
    bar.querySelectorAll('[data-v]').forEach((x) => x.setAttribute('aria-pressed', String(x.dataset.v === m)));
  };
  show(pairMode);
  bar.addEventListener('click', (e) => {
    const v = e.target.closest('[data-v]')?.dataset.v;
    if (!v) return;
    if (v === 'zoom') { openBox(img); return; }
    show(v);
    try { localStorage.setItem('pairs', v); } catch (err) {}
    document.querySelectorAll('.pair').forEach((p) => { if (p !== wrap) { p.dataset.mode = v; p.querySelectorAll('[data-v]').forEach((x) => x.setAttribute('aria-pressed', String(x.dataset.v === v))); } });
  });
}
document.querySelectorAll('.content img.zoomable').forEach((img) => {
  if (img.complete && img.naturalWidth) setupPair(img);
  else img.addEventListener('load', () => setupPair(img), { once: true });
});
