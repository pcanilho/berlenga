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
document.querySelectorAll('.content img').forEach((img) => {
  if (img.closest('a')) return;
  img.classList.add('zoomable');
  img.tabIndex = 0;
  const open = () => {
    boxImg.src = img.currentSrc || img.src;
    boxImg.alt = img.alt;
    const cap = img.closest('p')?.nextElementSibling;
    boxCap.textContent = cap && cap.querySelector('em:only-child') ? cap.textContent : img.alt;
    box.showModal();
  };
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
