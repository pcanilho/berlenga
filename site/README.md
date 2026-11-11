# Docs site

Hugo source for https://pcanilho.github.io/berlenga/, deployed by `.github/workflows/pages.yml` on pushes to main.

- Pages: `content/` (one page per feature area in `content/features/`, ordered by `weight`).
- Images: `docs/media/`, served at `media/`; write `![alt](media/name.jpg)`.
- References: the table in `docs/REFERENCES.md` is shown on the References page.

Preview: `pip install hugo` then `hugo server -s site`.
