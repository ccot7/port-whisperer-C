# Changelog

## [1.0.1] - 2026-04-05

### Fixed
- BATCH CWD RESOLUTION (macOS only) - ie: >ports ps --all

## [1.0.0] - 2026-04-05

### Added
- Initial bare-metal C implementation
- Linux support: reads `/proc` directly (no external tools except `lsof`)
- macOS support: uses `ps`, `lsof`, `git`
- Port table with box-drawing, ANSI colours, and status dots
- Framework detection: Next.js, Vite, React, Angular, Svelte, Nuxt, Remix, Astro,
  Express, Fastify, NestJS, Django, FastAPI, Flask, Rails, Go, Rust, Gradle, Maven
- Docker port mapping via `docker ps`
- `ports ps` — dev process overview with collapsed Docker row
- `ports <number>` — single-port detail view with kill prompt
- `ports watch` — real-time port change monitor (2s polling)
- `ports clean` — kill orphaned/zombie dev processes
- `ports --all` — include system/desktop processes
- Makefile with `all`, `clean`, `install`, `uninstall` targets
- README, LICENSE, .gitignore
