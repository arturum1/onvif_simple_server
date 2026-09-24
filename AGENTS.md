# AGENTS.md

ONVIF Profile S/T server in plain C99 for resource-constrained embedded devices. No gsoap/libxml: XML responses are assembled from template files. GPLv3.

## Binaries & build

One Makefile builds three programs:
- `onvif_simple_server` — the ONVIF "server"; runs as a *CGI*, not a daemon.
- `onvif_notify_server` — event daemon (monitors event input files, sends Notify).
- `wsd_simple_server` — WS-Discovery daemon (UDP multicast on :3702).

Build (`make` builds all three):
- `make test` — the only unit tests: builds/runs `test/test_utils` with the host compiler, **no crypto library needed** (uses stub headers in `test/mbedtls/` + `test/stubs.c`). Tests `gen_uuid_v5_mac` in `utils.c`. Run after any change to `utils.c`.
- Crypto backend selected at compile time via env: default libtomcrypt, else `HAVE_MBEDTLS=1` or `HAVE_WOLFSSL=1` exported. `USE_ZLIB=1` compresses templates (`*.xml.gz`, read via `gzip_d`). Optional `STRIP=` env. `Makefile.static` embeds all libs built under `extras/`.
- `extras/build.sh` is the full end-to-end example (builds lighttpd + mbedtls, stages everything into `_install/`).

Deps (dynamic builds): `-lz -ljson-c -lpthread -lrt` plus one crypto lib. Do not add new dependencies.

## CGI dispatch (the server is one binary, six services)

`onvif_simple_server` picks the service from the **basename of the CGI name** (or last argv): one of `device_service`, `media_service`, `media2_service`, `ptz_service`, `events_service`, `deviceio_service`. In production the httpd exposes it as symlinks into `${document-root}/onvif/` (see `extras/build.sh`). Requires `REQUEST_METHOD=POST`; reads the SOAP body from stdin, writes response to stdout.

A URL like `http://host/onvif/device_service` must point at a file literally named `device_service`.

## XML templates (the core pattern)

Every response — including SOAP faults and auth errors — is built by `cat(out, "service_files/Method.xml", n, ...)` in `utils.c`. Template paths are **relative to the CGI working directory**, so `*_service_files/`, `generic_files/` must be reachable from the CGI CWD (staged under `/usr/local/www/onvif/`). Moving/renaming these dirs breaks every response.

`cat()` reads a template line-by-line and does plain string substitution against varargs pairs `(token, value)`, in order, per line. Tokens look like `%MANUFACTURER%` (NOT printf `%s`). Notes:
- Missing template returns a SOAP `ter:ActionNotSupported` fault ("Optional Action Not Implemented").
- Substitutions run into a fixed `MAX_CAT_LEN` (2048) line buffer and are dropped on overflow; once a `%TOKEN%` was replaced it is replaced again if the value itself matches a later token. Keep values short and avoid `%` in them.
- Typical handler: `size = cat(NULL, tmpl, n, ...)` to size, `output_http_headers(size)`, then `cat("stdout", tmpl, n, ...)` to emit.

To add a method: add `<Service>_files/<Method>.xml`, then dispatch on `strcasecmp(method, "Name")` in the matching `*_service.c` (see `onvif_simple_server.c` around lines 500-730 for the dispatch table). Unimplemented methods return a fault, not an empty response.

## Config quirks

- INI-style `key=value` file; **line order matters, don't reorder sections** (README warns). JSON config also supported: pass a `.json` path (or the server falls back to `/etc/onvif_simple_server.json` if the `.conf` is absent).
- `device_uuid` is a deterministic UUID v5 derived from the MAC (`gen_uuid_v5_mac`, utils.c) — stable across reboots.
- `adv_fault_if_unknown` / `adv_fault_if_set` / `adv_synology_nvr` are client-compat toggles; flip them if a client can't connect, per README.

## Events: shared memory between two binaries

`onvif_simple_server` and `onvif_notify_server` are separate processes communicating through POSIX shared memory + a semaphore (`create_shared_memory` / `sem_memory_wait` in `utils.c`). The layout in `utils.h` (`shm_t`, `subscription_shm_t`, `event_shm_t`) is a cross-binary ABI — both programs must be rebuilt together when it changes. Limits: `MAX_SUBSCRIPTIONS 8`, `MAX_EVENTS 8`; events + relays total cannot exceed 8.

`events=` config: 1 = PullPoint only, 2 = Base (push) Subscription, 3 = both.

## subprocess / PTZ gotchas

- `move_*`, relay, and other fire-and-forget commands run via `system()` (`/bin/sh -c`). Any command that writes to **stdout corrupts the CGI response** — external commands must redirect stdout to `/dev/null` (README).
- Read-back commands (`get_presets`, `get_position`) use `spawn_capture(cmd, buf, len, timeout_sec)` (utils.c), which captures stdout and SIGKILLs the child after the timeout. Use `spawn_capture` — not raw `popen` — for any new command that reads output.
- PTZ zoom is optional: templates come in plain + `_nozoom` variants (e.g. `GetNodes.xml` / `GetNodes_nozoom.xml`), selected at runtime via `ptz_supports_zoom()` (`ptz_node.zoom_enable`). Keep both variants in sync. Non-zoom entries are plain `stub` templates and don't matter, but the choice is made at runtime, so both must exist for any PTZ template.

## wsd_simple_server specifics

- Requires `-x XADDR` (use `%s` for the detected IP); IPv6 is on a single dual-stack `AF_INET6` socket with `IPV6_V6ONLY=0` on `:::3702`, IPv4-mapped senders get the `%s` xaddr. If the IPv6 multicast join fails it degrades to IPv4-only — that's expected on boxes without IPv6.
- Self-contained: does not share state with the other two binaries.

## Coding conventions

- C99, `-Os -fPIC`, header comment is the GPLv3 boilerplate. No comments unless needed.
- Recent hardening work replaces unbounded `sprintf`/`popen` with bounded equivalents / `spawn_capture` and return proper ONVIF faults — follow that precedent for new code.
- `fault.c` centralizes fault responses (`send_fault`, `send_action_not_supported_fault`, ...) — reuse over hand-rolling XML.