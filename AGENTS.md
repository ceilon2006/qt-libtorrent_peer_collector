# AGENTS.md

Guidance for AI agents and new contributors working in this repository.

## What this project is

A single-executable Qt 6 Widgets desktop tool that opens a `.torrent` file in a
real libtorrent-rasterbar session for a fixed run time, collects every IPv4
peer endpoint it sees, and writes them to a `peers.txt` file. It is a C++ port
of an earlier Python script (`libtorrent_seen_peer_collector_v2_nodeprecated.py`,
not in this repo).

Peer sources, each tagged in the output:

| Tag                       | Where it comes from                                              |
|---------------------------|------------------------------------------------------------------|
| `connected/get_peer_info` | `torrent_handle::get_peer_info()` every poll                     |
| `tracker-direct-http`     | Hand-rolled HTTP/HTTPS announce done by this app, bypassing libtorrent |
| `tracker-alert`, `dht-alert`, `pex-alert`, `peer-alert`, `alert` | IPv4:port strings regex-scraped from `alert::message()` text |

Optional per-IP enrichment via the IPinfo REST API (`ipinfo.io/<ip>/json`).

## Layout

```
libtorrent_peer_collector_qt.cpp   # everything: helpers, worker thread, main window, main()
libtorrent_peer_collector_qt.pro   # qmake project
.gitignore                         # build output, dotfiles (except .qtcreator, .gitignore)
```

There is intentionally one translation unit. Both `Q_OBJECT` classes live in
the `.cpp`, so the file ends with `#include "libtorrent_peer_collector_qt.moc"`.
Keep that line last if you add code.

The header comment mentions a `CMakeLists_libtorrent_peer_collector_qt.txt`.
That file does not exist. qmake is the only working build.

## Build

Toolchain on the dev machine: Qt 6.10, libtorrent-rasterbar 2.0.12 (via
pkg-config), C++17, Linux.

```
qmake6 libtorrent_peer_collector_qt.pro
make
./libtorrent_peer_collector_qt
```

Build artifacts (`Makefile`, `*.o`, `moc_*`, the binary) are gitignored. Do not
commit them. There are no tests, no CI, and no README.

## Code map (by section in the .cpp)

1. **Utility helpers** (`ipPortToText`, `sortedPeerList`, `buildPeerOutputLines`,
   `savePeersToFile`, …). Pure functions over `std::set<QString>` peers and
   `std::map<QString, std::set<QString>>` peer sources. Output lines are
   `IP:port<pad># source, source; ipinfo: …` with the `#` column aligned to
   longest endpoint + 5.
2. **Direct HTTP tracker announce.** Builds a BEP 3 announce URL by hand, does a
   blocking `QNetworkAccessManager` GET inside a local `QEventLoop` (15 s
   timeout), and parses only the compact `5:peers` byte string with a minimal
   bencode reader. UDP trackers are skipped. Not a general bencode parser.
3. **IPinfo lookup.** Same blocking GET pattern, 4 s timeout, results cached
   per IP in memory for the run. Handles both legacy (`country`, `org`) and
   Lite (`country_code`, `asn{}` object) response shapes.
4. **`PeerCollectorThread : QThread`.** All libtorrent work happens in `run()`.
   Communicates with the GUI only through signals (`logMessage`,
   `peerCountChanged`, `peersPreviewChanged`, `progressChanged`,
   `finishedStatus`). Stop is cooperative via `std::atomic<bool>`.
5. **`MainWindow : QMainWindow`.** Form of options, Start/Stop, Log and Peers
   tabs, progress bar. Persists options with `QSettings("IRT",
   "LibtorrentPeerCollectorCppQt")`. The IPinfo token is deliberately never
   saved.
6. **`main()`.**

## Runtime behaviour worth knowing

- Torrent data is saved to a `QTemporaryDir` with sparse storage. With
  "no download" checked, all files and pieces are set to `dont_download`, but
  the session still connects to peers (that is the point).
- Poll loop: first iteration runs immediately, then sleeps `poll interval`
  (min 200 ms). Tracker and DHT forced reannounces have a hard floor of 30 s.
  Output file is rewritten every 10 s and at the end. Preview tab refreshes
  every 3 s.
- Every direct tracker announce sends `event=started` with a freshly random
  `-QTPC01-` peer id and `left=<total size>`, so trackers see a new leecher on
  each reannounce. Be mindful of this if you lower intervals.
- The worker's poll cycle can block for a long time: up to 15 s per HTTP
  tracker plus up to 10 IPinfo lookups × 4 s per cycle. "Stop" only takes
  effect between cycles, and `closeEvent` waits at most 3 s.
- IPv6 peers are dropped everywhere by design (`ipPortToText` returns empty for
  non-v4).

## Known gaps and likely-bug areas

Check these before assuming the code does what its log messages claim.

- **Alert mask.** The worker sets `alert_mask` to error, peer, tracker, dht
  and status (libtorrent 2.0 defaults to error only). Only the `peer`
  category produces alert messages containing endpoints, so `peer-alert` is
  the tag that actually gains peers. `tracker-alert` and `dht-alert` messages
  carry URLs and counts, not addresses. A tracker with a raw-IP URL will be
  scraped as a "peer" from tracker alerts. Do not add `peer_log` unless you
  also stop echoing every "peer" alert to the Log tab.
- **PEX checkbox is cosmetic.** `enablePex` is only logged. In libtorrent 2.0
  PEX is the `ut_pex` plugin; disabling it means constructing the session
  without default plugins (`session_params` flags) and adding only the ones
  you want, not a `settings_pack` key.
- `buildPeerSummaryText` computes `countPeersWithSourcePrefix(peer_sources, "")`
  and immediately discards it. Dead code, harmless.
- Listen port spin box allows 0, which yields `listen_interfaces = 0.0.0.0:0`.
- `directAnnounceAllHttpTrackers` is called twice on startup (once before the
  loop, once in the first poll iteration).
- Source tagging of alerts is by substring on the lowercased message
  (`"tracker"`, `"dht"`, `"pex"`, `"peer"`), so classification is heuristic.

## Conventions

- Tabs for indentation. Opening brace on the same line for control flow, on
  its own line for functions and classes.
- Local variables are declared at the top of the function, C-style, often
  uninitialised, then assigned.
- `/* … */` block comments, including single-line ones. Almost no `//`.
- Free helpers are `static` and take output params by pointer
  (`QString *error`, `QStringList *log_lines`) with null checks.
- libtorrent calls that may throw are wrapped in `try { … } catch (...) {}`
  and either logged or silently ignored. Follow that pattern rather than
  letting exceptions escape `run()`.
- Use the `lt::` namespace alias for libtorrent.
- GUI updates only from `MainWindow` slots; the worker must never touch widgets.

## When changing things

- Any new `Q_OBJECT` class must stay in this `.cpp` (or you must add a header
  and update the `.pro`) so moc picks it up.
- If you add a libtorrent setting, verify it exists in 2.0.x
  (`/usr/include/libtorrent/settings_pack.hpp`). Several 1.x names are gone.
- If you touch the output format, keep `savePeersToFile` and the preview tab
  in sync; both go through `buildPeerOutputLines`.
- There is no automated test harness. Verify by building and running against a
  well-seeded public torrent for a short run time.
