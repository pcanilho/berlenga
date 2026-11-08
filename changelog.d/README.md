# Changelog entries

Each pull request adds one file here, `changelog.d/<short-name>.md`, with its entry:

```
### Fixed
- What changes for someone using the engine, in one line (#123).
```

Kinds: Added, Changed, Fixed, Removed. A file can hold several kinds and lines. The
release workflow publishes the files as the release notes, moves their entries into
CHANGELOG.md under the release's heading and deletes them. A separate file per pull
request means two pull requests never edit the same lines, so a release made while a
pull request is open can't misplace its entry.
