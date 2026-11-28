# How FPKGi+ works

This page covers how FPKGi+ handles downloads, installs, updates and secure connections. For setup and everyday use, see the [README](../README.md).

FPKGi+ does not include any games or PKGs. Apart from the built-in list of open-source homebrew, everything described here acts on content lists and files that you supply.

## Downloads

### Resuming

Downloads resume with HTTP byte ranges, and a partial download is only continued from the same version of the remote file. The server's `ETag` and/or `Last-Modified` and the file size are saved with the resume data (`<file>.pkg.part.meta` for one connection, inside `<file>.pkg.part.chunks` for several). They are sent back as `If-Range` with every range request and compared with every answer.

If the file changed on the server, before a resume or during a download, the partial data is discarded and the download starts again. Old and new bytes are never mixed, and the finished entry says the file was restarted. Some cases cannot detect a same-size replacement:

- A weak `ETag` is not sent in `If-Range` (`Last-Modified` is used instead), but it is still compared.
- A server that sends neither validator is resumed on the file size alone.
- Mirrors of one file that report different validators are treated as a change, which costs a restart rather than risking a mixed file.

### Free space

Before a download starts or resumes, FPKGi+ checks the free space at its destination. The space needed is the size reported by the server, less the bytes already saved by every connection, plus a 16 MB reserve. If that does not fit, the download stops with **Not enough free space**, showing how much is still to download, how much is free and how much to free. The partial download is kept, and **Retry** continues it.

If free space cannot be read, the download is never blocked; Downloads shows **free space not reported**. This happens with a USB drive reached through GoldHEN's FTP server, or a console answer that does not look right. Console free space is read with the kernel's `statfs`, using the kernel's own structure layout, and an unexpected structure version counts as unknown.

### The queue

Queued, paused and running downloads are saved in order to `download-queue.json`, with each title's details and destination, and written atomically. On the next launch:

- Each entry is checked (address, title, destination, expected file name). Unusable entries are dropped, and an unreadable file is set aside as `download-queue.json.bad`.
- Queued downloads continue once the catalogue has loaded. Paused ones stay paused.
- A download that was running when the app closed or crashed comes back **Paused**, labelled *Interrupted when FPKGi+ closed*. **Resume** continues it. (With the background downloader, below, it simply continues.)
- A download whose USB drive is not connected is **Paused**, labelled *USB drive N is not connected*. Nothing is written to the empty mount point, and the download is never moved to the console or another drive.

Failed and stopped downloads are not saved in the queue, but their partial files remain listed in Downloads and can be continued from there.

### The background downloader

From 1.01 the queue runs in a console background service, `FPKP00002` (`source/service/`), so downloads continue when FPKGi+ is closed. It is FPKGi+'s own download code without the interface. FPKGi+ copies it from its package to `/system/vsh/app/FPKP00002` (the system partition is made writable for the copy and then read-only again), with the `param.sfo` of the console's own background music service, which marks it as a background service. It is replaced whenever FPKGi+'s package carries a different build, and started with the console's launcher when FPKGi+ opens.

The service stays in its sandbox: in it `/data` and the network work, and USB drives are reached through GoldHEN's FTP server. (A test build that took it out of the sandbox with the credential helper turned the console off when FPKGi+ was closed during a download.)

The app and the service share `/data/FPKGiPlus/service/`:

- `commands/`: the app's requests (download, pause, resume, stop, clear, delete), one file each, applied in order and then removed.
- `state.json`: the service's list and progress, rewritten about once a second while downloading (every 5 seconds otherwise), with a heartbeat and the downloads that finished.
- `handled`: the last finished download the app has dealt with, so each is installed or reported once.

The service owns `download-queue.json`. A download cut off by a stop or a crash is queued again when it comes back, rather than paused. It sends a console notification when a download finishes or fails.

Rest mode suspends the service a few seconds after the console goes to sleep, as it does every app and background service (`sceLncUtilBlockAppSuspend`, which defers an ordinary app suspend, does not prevent it), so downloads pause and continue when the console wakes. The service notices a wake as a jump in the clock between its checks, and notices rest mode as no user on screen. Notifications wait while there is no user on screen and for a minute after one is back, because the console's interface is still starting then. A download that fails on a connection error (the network drops as the console sleeps and wakes) is tried again up to 5 times, 30 seconds apart; a full disk, a missing file (404) or a file that is not a PKG is reported at once. Installing stays with the app: it installs a finished download when it is open, or the next time it opens. FPKGi+ asks the service to stop (and waits for it) before replacing or removing it.

### Destinations

Console downloads go to `/data/pkg/`, and USB downloads go to the selected drive's root. Custom paths are not offered. Changing the destination only affects new downloads; queued and paused jobs keep the destination they were queued with, including across restarts. In USB mode, FPKGi+ waits for a drive instead of writing to console storage.

### Stopping and deleting

**Stop** keeps the partial download. **Downloads > Square > Delete file**, or the title's **Options > Delete partial download**, removes the PKG, partial files, resume data and queue entry together. Files still in use by a transfer or an install are protected. A PKG whose size does not match is never offered as complete and cannot be installed.

## Installing

Downloaded PKGs stay at their destination. Install them from the title's page or the Downloads list, automatically with **Install after downloading**, or with the console's own Package Installer.

- For local installs, FPKGi+ uses the storage-registration flow from Itemzflow. The console checks the PKG's title ID and derives its content ID, type and size.
- The console's install service rejects `/data/pkg/...` as a content URL, so FPKGi+ passes the same file as `/user/data/pkg/...`. USB paths are passed unchanged.
- Downloads and installs have separate states. A finished transfer is **Downloaded**. A running install shows **Installing** with progress, taken from the console's copied-byte totals every 1.5 seconds. A verified install is **Installed**, and Cross on that row becomes **Reinstall**.
- A failed install keeps the PKG and shows **Downloaded - installation failed; PKG kept**, so it can be installed again without downloading.

### Verifying an install

A copy finishing on its own never counts as installed: FPKGi+ waits for the expected version to be registered with the console. If that has not happened after five minutes of app run time (carried across restarts), the title shows **Install needs attention**. **Resolve installation** then offers three choices:

- **Check again** restarts the wait.
- **Stop waiting** asks the console to release FPKGi+'s install task, so **Install** can be retried.
- **Forget install task** is for a task the console will not release. Use it only after the task's entry is gone from **Notifications > Downloads**.

None of these deletes the PKG. A stalled install is still checked every 15 seconds, so a late registration completes normally.

FPKGi+ only stops and releases its own install tasks; it never adopts or removes tasks belonging to other apps or to Sony. If a console reports `0x80990086`, an earlier failed task is still registered: remove it from **PS4 Notifications > Downloads**.

### Deleting the PKG after installing

With **Delete PKG after successful install** on, the PKG is deleted only after the copy is complete, the matching version is installed and the install task is released. Failed, uncertain or stalled installs keep the PKG. This also applies to installs you start yourself.

### Installed status

A title counts as installed only when an installed PKG is actually present, and, when the service is available, registered with the console. Empty leftover folders do not count. Status refreshes every four seconds, including after removals made outside FPKGi+.

**Options > Delete installed game/app** asks for confirmation, uninstalls through the console's AppInstUtil service and keeps any downloaded PKG. FPKGi+ itself and system titles cannot be deleted. **Delete downloaded PKG** removes the file and keeps the installed title.

## Launching

Titles launch through the console's `LncUtil` service. With **Skip launcher preflight checks** on (the default), the launch and system-update checks are skipped for launches from FPKGi+. This does not stop Sony's background update downloader or a game's own online checks. To stop those, turn off **PS4 Settings > System > Automatic Downloads > Application Update Files**, and block PlayStation update servers on your network or disconnect before launching. FPKGi+ never changes the console's network settings.

## Homebrew

- The built-in Homebrew list (`assets/homebrew-list.json` in the package) is used when there is no local `HOMEBREW.json` and no Homebrew URL. Every entry links to the project's own GitHub release.
- Homebrew details (description, maintainer credit, repository) ship with the app and work offline. Content lists and `metadata.json` can override them.
- **Triangle > Homebrew category** filters by **All**, **Utilities**, **Media**, **Emulators** or **Games**. Entries without a category appear under **Other** and **All**.
- **Latest release** is the publication date of the project's latest GitHub release. It is cached and refreshed every six hours.
- **Check for updates** reads the installed version from the installed app or patch `param.sfo`, including extended storage. It then reads the latest release PKG's title ID and version with small byte-range requests. **Download update** is offered only for a newer package with the same title ID. Equal versions show **Already on the latest version**, and a failed check never claims the app is up to date or queues a download.

## App updates

About 30 seconds after starting, FPKGi+ asks GitHub for the latest release of `jw-notes/FPKGiPLUS-PS4` and compares its tag with the running version. **Settings > Software update** shows the result, and selecting it checks again straight away. When a newer release has a `.pkg` attached, **Download** saves it to `/data/pkg/FPKGiPlus-update.pkg`. The file is accepted only if its size matches the release and its SHA-256 matches the digest GitHub publishes for the file; otherwise it is deleted.

If the download host's certificate cannot be verified on older firmware, the file is fetched again without that check. It is still accepted only if it matches the SHA-256 obtained from api.github.com over a verified connection.

FPKGi+ never installs its own package. Every install path (local installs, installs from a link, and moving a PKG into GoldHEN's install folder) refuses a package whose title ID, or whose header content ID, is FPKGi+'s (`FPKP00001`). Close the app and install the update from **Debug Settings > Game > Package Installer**.

## Game details, screenshots and trailers

When a title is opened, its details, screenshots and trailers are looked up on the PlayStation Store in the background:

1. **By title ID**, in the store of the title's region first (EUR: the UK store, USA: the US store, JAP: Japan, ASIA: Hong Kong), then the others. Add-ons and themes listed under the same title ID are skipped unless their name matches.
2. **By name**, when the title ID is not listed (a disc release's ID, or a product withdrawn from sale). The name must match. Only release words may differ, such as *Remastered*, *Edition*, *Definitive*, *Game of the Year*, *PS4* or a year, so *The Last of Us* finds *The Last of Us Remastered* but never *Part II*. A game sold as one PS4 and PS5 product and listed only for PS5 uses that listing.

What is found is saved in `store-metadata.json` and works offline. Screenshots are downloaded once and kept at 1920×1080 in `Screenshots/`. Sony's picture and video servers are used over plain HTTP: they are public files, and this keeps the console's TLS out of it. Trailers are never saved. They stream (HLS, up to 720p) while you watch.

If the store has nothing for a PS1, PS2 or PSP title, its in-game screenshot and title screen come from [libretro-thumbnails](https://github.com/libretro-thumbnails), matched by name and region. Its list of names is cached in `OpenMedia/` for 30 days. No other title is given pictures from anywhere else: a wrong picture is worse than none.

A lookup that fails because the network or the store is unavailable is tried again the next time the title is opened. If the store refuses requests (HTTP 403 or 429), automatic lookups pause for 10 minutes. Use **Options > Find game info** to search the store yourself and pick the right game. Homebrew is not looked up. Fields in `metadata.json` take priority over the store's.

### Trailers

Trailers play with [MPV](https://mpv.io/) and [FFmpeg](https://ffmpeg.org/), decoding in software, as in [JellyFin \[PS\]](https://github.com/jw-notes/JellyFin-PS4). The console's own video player only opens a web address as a local file, so it cannot stream them. A player is made for each trailer and closed when you leave it or go to the home screen, so nothing of it stays in the app between trailers. SDL2's audio driver (MPV's sound output) is started once and never shut down, because starting it a second time crashed the app.

## Archive.org sign-in

**Settings > Archive.org account > Sign in** uses Archive.org's login service, as used by its [official client](https://github.com/jjjake/internetarchive). Passwords are never saved. The session cookies are only sent over HTTPS (port 443) to archive.org and its subdomains, including mirror redirects. Sign-in does not override item restrictions, and a 401 or 403 after signing in can mean the session expired or the account cannot access the file. Logs record the sign-in stages and HTTP status, never email addresses, passwords, cookies or response bodies.

## Secure connections

Certificate and hostname checks are always on. At startup, FPKGi+ loads the Mozilla CA bundle shipped in the package (`assets/cacert.pem`, MPL 2.0) into the PS4 HTTP library. Older PS4 firmware cannot follow some certificate chains, so three fixed compatibility files are also loaded:

| File | Why | Expires |
| --- | --- | --- |
| `godaddy-secure-g2.pem` | Archive.org's GoDaddy G2 chain | 2031-05-03 |
| `letsencrypt-gen-y.pem` | GitHub release downloads (Let's Encrypt ISRG Root YR / YR1) | Intermediates 2028-09-02 |
| `digicert-playstation.pem` | The PlayStation Store's catalogue API (GeoTrust TLS RSA CA G1 under DigiCert Global Root G2) | GeoTrust G1 2027-11-02 |

Each file is pinned by fingerprint and checked by `tools/check_tls_assets.py` at build time. Where each certificate came from and what it is trusted for is recorded in `licenses/*-SOURCE.txt` in the package. Certificates are never fetched or accepted at run time to recover from a TLS error. When a connection fails verification, the log records the SSL error, verification flags and console time.

## USB drives

USB support comes from [Librarian](https://github.com/jw-notes/Librarian). With one drive connected, it is used automatically; with several, you choose. Reaching USB drives from the app's sandbox needs GoldHEN 2.3 or later. On HEN setups without GoldHEN's SDK, FPKGi+ uses the libjbc credential helper adapted from Itemzflow. The helper uses the HEN's existing kernel-call interface and does not install an exploit. Its source, changes and licence ship in the package.
