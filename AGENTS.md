# AGENTS.md

Guidance for AI agents and new contributors working in this repository.

## What this project is

A single-executable Qt 6 Widgets desktop tool that opens a `.torrent` file in a
real libtorrent-rasterbar session for a fixed run time, collects every IPv4
peer endpoint it sees, and shows them in a sortable table (IP, port, "Have"
= the share of the torrent a peer reported when last connected (`progress_ppm`
from `peer_info`, empty for peers that never connected), sources, IPinfo
country/city/provider/host). Right-click, Ctrl+C or the Copy Selected button
copies the selected rows; the tab title and status line show total and
"connected so far" counts. Two checkboxes beside the buttons
(`export/sources`, `export/ipinfo`) decide what follows the endpoint on
copied/saved lines; the IPinfo part is the table cells as shown ("RU, Moscow,
AS8580 MTS PJSC, host"), not key=value. `options/ipinfo_lookup` decides
whether lookups run at all. An application-wide `eventFilter` clears the
table selection on any mouse press in the main window outside the table
(except on Copy Selected); no cell tooltips, by request. Nothing is written to disk on its own:
Copy Peers puts the table on the clipboard as text in its current sort order,
Save Peers writes the same text to a file chosen in a dialog (last folder
remembered in `paths/peers_dir`). "Extra trackers" (default: four public UDP
trackers, `DEFAULT_EXTRA_TRACKERS`, key `options/extra_trackers`) are added
to every torrent's `torrent_info` in tier 1 before it is added to the session. It is a C++ port
of an earlier Python script (`libtorrent_seen_peer_collector_v2_nodeprecated.py`,
not in this repo).

Peer sources, each tagged in the output:

| Tag                       | Where it comes from                                              |
|---------------------------|------------------------------------------------------------------|
| `connected/get_peer_info` | `torrent_handle::get_peer_info()` every poll                     |
| `tracker-direct-http`     | Hand-rolled HTTP/HTTPS announce done by this app, bypassing libtorrent |
| `tracker-direct-udp`      | Same over UDP (BEP 15): connect + announce on a `QUdpSocket`          |
| `peer-connect-in/out`     | `peer_connect_alert`, handshake completed                        |
| `peer-connect-failed`     | `peer_disconnected_alert` with `op == operation_t::connect`      |
| `peer-disconnected`       | any other `peer_disconnected_alert`                              |
| `peer-incoming`           | `incoming_connection_alert`, pre-handshake                       |
| `peer-error` / `peer-banned` / `peer-blocked` | the matching alert types                     |
| `peer-alert`              | IPv4:port regex-scraped from any other peer-category alert text  |

Optional per-IP enrichment via the IPinfo REST API (`ipinfo.io/<ip>/json`).

## Layout

```
libtorrent_peer_collector_qt.cpp   # everything: helpers, threads, main window, main()
libtorrent_peer_collector_qt.pro   # qmake project
scripts/linux/     build.sh rebuild.sh clean.sh deploy.sh
scripts/windows/   build.bat rebuild.bat clean.bat deploy.bat mingw_env.bat
docs/build-requirements.html       # which packages are needed and how to install them
docs/kde-dialog-warning.html       # the kf.kio.widgets.kdirmodel terminal noise: cause and the mute in main()
.gitignore                         # build/, build-*/, deploy/, qmake output, dotfiles
```

There is intentionally one translation unit. Both `Q_OBJECT` classes live in
the `.cpp`, so the file ends with `#include "libtorrent_peer_collector_qt.moc"`.
Keep that line last if you add code.

qmake is the only build system. There is no CMake file.

## Build

Toolchain on the dev machine: Qt 6.10, libtorrent-rasterbar 2.0.12 (via
pkg-config), C++17, Ubuntu 26.04. Package names for other distros and the
Windows/MSYS2 route are in `docs/build-requirements.html`.

```
scripts/linux/build.sh [release|debug]   # out-of-tree into build/ or build-debug/
scripts/linux/rebuild.sh                 # clean.sh then build.sh
scripts/linux/clean.sh                   # removes build/, build-*/, deploy/, qmake output
scripts/linux/deploy.sh [-y] [DEST]      # copy the binary to deploy/ (or DEST)
scripts/linux/deploy.sh --install [PFX]  # install to PFX/bin + .desktop, default ~/.local
```

The Windows scripts mirror these and add `windeployqt` plus copying the
libtorrent and OpenSSL DLLs. They were written against the pattern in
`qt-p2p_filter_generator` and have not been run on Windows from this repo.

The scripts re-run qmake on every build, so a changed `.pro` is picked up.
Build output (`build/`, `build-*/`, `deploy/`, `Makefile`, `*.o`, `moc_*`,
the binary) is gitignored. There are no tests, no CI, and no README.

## Code map (by section in the .cpp)

1. **Utility helpers** (`ipPortToText`, `sortedPeerList`, `buildPeerRows`,
   `parseIpInfoFields`, `pruneSelfPeers`, …). Pure functions over
   `std::set<QString>` peers and `std::map<QString, std::set<QString>>` peer
   sources. The worker sends `PeerRows` (`QList<PeerRow>` with endpoint,
   sources, ipinfo, active; registered with `qRegisterMetaType` in `main`)
   to the GUI; the GUI formats text lines as
   `IP:port<pad># source, source; ipinfo: …` with the `#` column aligned to
   longest endpoint + 5 (`peersAsText`).
2. **Direct tracker announce.** `DirectAnnounceContext` holds the run's peer
   id and key plus the set of trackers that already got `started`. The peer
   id (`makePeerId`, libtorrent fingerprint prefix + random) is also given to
   libtorrent as `settings_pack::peer_fingerprint` (20 bytes = whole id), so
   the session and the direct announce are one client to trackers.
   HTTP/HTTPS: BEP 3 URL built by hand, blocking `QNetworkAccessManager` GET
   inside a local `QEventLoop` (15 s), compact `5:peers` parsed by a minimal
   bencode reader, not a general one. UDP: BEP 15 connect + announce on a
   blocking `QUdpSocket`, two attempts of 5 s, IPv4 only, no connection id
   caching. `directAnnounceAllTrackers` dispatches by scheme and sends
   `started` once per tracker, then no event; `directStopAllTrackers` sends
   `stopped` at the end with short timeouts. All trackers of a round are
   announced concurrently (`runTrackerJobsInParallel`, one `QThread` each),
   and the poll loop runs the whole round on a further thread, merging
   `roundPeers`/`roundSources`/`roundLogs` when it has finished, so a dead
   tracker never stalls polling. `replied` means the tracker itself
   answered (any HTTP status counts, DNS failure does not); only those get
   `stopped`. Verified against public UDP trackers on 2026-09-21: eight
   trackers in sequence took 16 s, in parallel 10 s (one dead UDP tracker),
   `stopped` to five trackers 0.56 s.
3. **IPinfo lookup.** `fetchIpInfoText` is the same blocking GET pattern,
   4 s timeout. Handles both legacy (`country`, `org`) and Lite
   (`country_code`, `asn{}` object) response shapes. `IpInfoLookupThread`
   runs those lookups off the poll loop: a queue of IPs, a result cache, and
   log lines, all behind one mutex. The worker enqueues every poll, takes a
   cache snapshot when writing output, and drains the log lines itself.
4. **ASN prefix lookup.** `AsnPrefixThread` (Q_OBJECT, one signal `done`)
   asks RIPEstat's Data API: network-info (IP -> origin AS), as-overview
   (holder, only when the table gave no provider), rir-stats-country (only
   when the table gave no country), announced-prefixes (IPv4 only, sorted).
   `formatAsnPrefixList` writes the filter format: `# AS, country, city,
   holder` header, then `CIDR<pad># same, address-count` lines. Started from
   the table's context menu; hints are the row's Country/City/Provider cells
   (provider stripped of `ASnnnn `). Dialog is non-modal with Copy/Save,
   Save uses `paths/peers_dir`. Threads are tracked in `asnThreads` and
   joined in `~MainWindow`. Verified live 2026-09-22 against AS8580 and
   AS12389.
5. **Alert classification.** `classifyPeerAlert` maps alert types to an
   endpoint and a source tag with `alert_cast`. `shouldLogAlert` decides what
   reaches the Log tab (error/tracker/dht/status plus error/ban/block peer
   alerts; connect/disconnect are too frequent to log).
6. **`PeerCollectorThread : QThread`.** All libtorrent work happens in `run()`.
   Communicates with the GUI only through signals (`logMessage`,
   `peerCountChanged`, `peersPreviewChanged`, `progressChanged`,
   `finishedStatus`). Stop is cooperative via `std::atomic<bool>`.
7. **`MainWindow : QMainWindow`.** Form of options, Start/Stop, Log and Peers
   tabs, progress bar. Settings identity is `CONFIG_FOLDER_NAME` /
   `APP_NAME` (`myutils` / `LibtorrentPeerCollectorQt`), the same scheme as
   `qt-p2p_filter_generator`; `main()` registers the same names on the
   application so a bare `QSettings()` resolves to the same store. Keys are
   grouped `paths/`, `options/`, `ui/` (window geometry, help geometry, last
   tab). The IPinfo token is deliberately never saved. Help button and F1 open a non-modal tabbed `QDialog` built by
   `showHelpDialog` / `addHelpPage`, same pattern as the sibling
   `qt-p2p_filter_generator` and `qt-web_selector` projects. Keep the help
   text in step with the options and tags it describes.
8. **`main()`.**

## Runtime behaviour worth knowing

- The session is built with `session_flags_t{}` (no default plugins) and
  `ut_metadata`, `smart_ban` and, only when PEX is checked, `ut_pex` are
  attached through `add_torrent_params::extensions`. That is how the PEX
  checkbox works; there is no settings_pack key for it in 2.0.
- Torrent data is saved to a `QTemporaryDir` with sparse storage. "Minimal
  download" keeps exactly one piece wanted (`low_priority`, rotating to the
  next on `piece_finished_alert`) with `download_rate_limit` 1024 B/s. Marking
  every piece `dont_download` makes the torrent finished and libtorrent then
  opens no outgoing connections at all, so real peers are never contacted
  (verified 2026-09-21: 0 established peers in 75 s vs seeds connecting in 4 s
  with one piece wanted). `close_redundant_connections` is off so seeds are
  not dropped before their bitfield is read. `get_peer_info` entries with the
  `connecting`/`handshake` flags are tagged `connecting`, not counted as
  connected; established peers get `activePeers` and the best `progress_ppm`
  seen (seed flag = 1000000). In addition `ProgressTorrentPlugin` /
  `ProgressPeerPlugin` (libtorrent peer plugin, attached via
  `add_torrent_params::extensions`) record every handshake, bitfield,
  have_all/have_none and have on libtorrent's network thread into a
  mutex-guarded `PeerProgressBook`; the worker merges it each poll (tag
  `connected/handshake`), so peers that leave within a poll interval still
  get their share recorded. Shown in the "Have" column, green dot when a
  connected peer never reported a figure, red cross and red IP for rows with
  `peer-blocked`/`peer-banned` (libtorrent's own bans, sort key -2).
- Poll loop: first iteration runs immediately, then sleeps `poll interval`
  (min 200 ms). Tracker and DHT forced reannounces have a hard floor of 30 s.
  The peers tab refreshes every 3 s and once more at the end; the worker never
  touches the file system except for the temporary save path.
- The direct announce presents itself as a leecher (`left=<total size>`) with
  a `-QTPC01-` peer id that is fixed for the run. Trackers therefore list this
  machine as a peer for the torrent while it runs, and drop it after
  `stopped`.
- Stop is honoured within about 200 ms wherever the worker is: the poll
  sleep runs in 100 ms slices, the HTTP announce polls the cancel flag on a
  200 ms timer inside its event loop, the UDP exchange waits in 200 ms
  slices, and the tracker loop checks between trackers. The only
  uninterruptible wait left is the blocking DNS lookup for a UDP tracker.
  After Stop, `stopped` goes only to trackers that replied earlier (3 s /
  2 s timeouts), the IPinfo queue is abandoned (`ipinfo=pending`), then the
  final save. A normal end of run waits for the IPinfo queue to drain.
  `closeEvent` waits at most 3 s for the thread.
- While a run is active the Torrent and Options group boxes are disabled
  (`setInputsEnabled`); every field in them is read once at Start. Buttons
  and the Copy/Save checkboxes stay enabled. `collectionFinished` must not
  clear `worker`: `finishedStatus` arrives before `run()` returns and the
  session teardown after it takes 1-2 s; the `QPointer` nulls itself when
  the thread self-deletes, and the destructor waits on it until then.
- IPv6 peers are dropped everywhere by design (`ipPortToText` returns empty for
  non-v4).
- The machine's own addresses are pruned from the peer set every poll and
  before the final save: all local IPv4 interface addresses plus whatever
  `external_ip_alert` reports. Trackers echo the announcing peer back, and
  without this the list contained the collector itself.
- `peer-blocked` entries with no filter configured are libtorrent's own
  session-level `ban_ip` of DHT snoopers (see `peer_connection.cpp`, the
  `verify_secret_id` check): hosts that connect asking for a decoy info hash
  libtorrent planted in DHT traffic. Not peers of the torrent.

## Known gaps and likely-bug areas

Check these before assuming the code does what its log messages claim.

- **Alert mask.** The worker sets `alert_mask` to error, connect, peer,
  ip_block, tracker, dht and status (libtorrent 2.0 defaults to error only).
  Note `peer_connect_alert` and `peer_disconnected_alert` are in the
  `connect` category, not `peer`. tracker/dht alerts carry URLs and counts,
  not addresses, so they only feed the log. Do not add `peer_log` unless you
  also tighten `shouldLogAlert`.
- Only peer-category alerts that `classifyPeerAlert` does not handle are
  regex-scraped, so a tracker URL with a raw IP is never mistaken for a peer.

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
  and update the `.pro`) so moc picks it up. `IpInfoLookupThread` has no
  signals and no `Q_OBJECT` on purpose.
- If you change an option, a tag or the output format, update the Help dialog
  text and `docs/build-requirements.html` if packages are involved.
- If you add a libtorrent setting, verify it exists in 2.0.x
  (`/usr/include/libtorrent/settings_pack.hpp`). Several 1.x names are gone.
- The peers table is the output. `peersAsText` is the only text formatter;
  Copy and Save both use it, in the table's current sort order.
- There is no automated test harness. Verify by building and running against a
  well-seeded public torrent for a short run time.
