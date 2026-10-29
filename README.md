# webmount

Mount an HTTP directory listing as a read-only Windows drive. Browse remote files in Explorer or open them from applications without downloading the entire directory first.

Version 0.1.0. Curated by Psychip.

## Requirements

- Windows 10 or Windows 11, x64.
- An HTTP or HTTPS server exposing a supported directory listing.
- Administrator approval to install or remove the bundled WinFsp runtime. Normal mounting runs as the current user.

Supported listing formats are Apache/nginx HTML, IIS, lighttpd, LiteSpeed, nginx fancyindex, Caddy JSON, and nginx JSON/XML. Format detection is automatic. Arbitrary web pages, authentication flows, and WebDAV are not supported.

## Build

Use CMake 3.24 or newer and Visual Studio 2022 with the Desktop development with C++ workload, MSVC, and a Windows SDK. Run these commands from the project root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is `build/bin/Release/webmount.exe`. For Debug, use `--config Debug`; the executable is then under `build/bin/Debug/`.

The project uses C++20 and the static MSVC runtime. WinFsp headers and signed x64 runtime binaries are included under `vendor/winfsp/`; no separate WinFsp SDK installation or dependency download is required to build webmount. The DLL and driver are embedded into the executable as resources.

These commands describe the build configuration. This publication-preparation pass did not build or test the project.

## Usage

The URL below is a placeholder; replace it with a directory listing served by your host.

```powershell
.\build\bin\Release\webmount.exe --help
.\build\bin\Release\webmount.exe "https://example.com/files/"
.\build\bin\Release\webmount.exe W: "https://example.com/files/"
.\build\bin\Release\webmount.exe W: "https://example.com/files/" --detach
.\build\bin\Release\webmount.exe W: "https://example.com/files/" --persist
.\build\bin\Release\webmount.exe --eject W:
.\build\bin\Release\webmount.exe --eject
```

- Without a drive letter, webmount selects the first free letter from D: through Z:. Explicit letters A: through Z: must be free.
- Foreground mounts stay active until Ctrl+C or an eject request. The console displays file and directory activity.
- `--detach` runs the mount in the background. `--background` is an alias.
- `--persist` implies background mode and saves the actual assigned letter for restoration after Windows sign-in.
- `--eject W:` ejects that user's W: mount and removes its saved startup entry, even if the drive is offline.
- `--eject` ejects all mounts owned by the current user and removes that user's saved entries.
- No arguments prints help.

Use an explicit `https://` URL when transport encryption is required. Bare hostnames default to HTTP. Embedded credentials, URL fragments, and query parameters on the supplied directory URL are rejected. HTTPS-to-HTTP redirects are rejected; after initial directory resolution, redirects must remain within the mounted directory tree.

## Runtime installation

The first mount requests UAC approval if webmount's private runtime is absent. Setup can also be invoked explicitly:

```powershell
.\build\bin\Release\webmount.exe --install
.\build\bin\Release\webmount.exe --uninstall
```

The current private runtime location is `%ProgramW6432%\amount\SxS\sxs.amount-2.1.25156\bin`, and its driver service is `WinFsp+amount-2.1.25156`. These are existing implementation identifiers, not a separate application to install.

Setup verifies the embedded DLL and driver signatures. Existing runtime files are not silently overwritten. A separately installed WinFsp installation is not used or removed.

Uninstall clears the invoking user's saved mounts, stops webmount mounts across users before removing the machine-wide private runtime, and may require a restart to remove locked files. It does not clear other users' startup registrations.

## Persistent mounts

Saved commands live under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` as `WebMount-<letter>`. They launch after that user signs in, not as a Windows service.

Keep `webmount.exe` at its registered location, or register persistence again after moving it. The startup command must fit the Windows Run limit; long executable paths or URLs can prevent registration. Restoration retries transient failures for up to five minutes and never replaces an occupied drive. Failed restoration retains the registration for a later sign-in.

## Limits and behavior

- Read-only: no writes, uploads, deletes, or renames.
- The root listing is fetched before mounting. Browsing prefetches immediate child directory listings, never the entire tree recursively.
- Directory listings and file metadata are cached for 60 seconds.
- The file-data cache is 64 MiB, with 1 MiB range reads. This is not a total process-memory limit.
- Servers without byte-range support must provide whole files of at most 64 MiB. Exact file sizes must be available through HTTP metadata.
- Directory responses are limited to 16 MiB and 100,000 accepted entries.
- Listing sizes may be approximate until exact metadata is fetched when a file is opened.
- Windows-invalid names are escaped; case-insensitive name collisions receive suffixes.
- ETag or Last-Modified validators are used when available. Servers without validators cannot guarantee a consistent snapshot of changing files.
- Mounts require network access and are not an offline mirror.

## License and third-party software

webmount's original source and project documentation are licensed under the GNU General Public License, version 3 only: **GPL-3.0-only**. See [LICENSE](LICENSE). No permission to choose a later GPL version is granted for webmount. The software is provided without warranty.

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos

WinFsp repository: <https://github.com/winfsp/winfsp>

WinFsp retains its own license and notices in [vendor/winfsp/License.txt](vendor/winfsp/License.txt). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the pinned upstream source, bundled binary hashes, and redistribution requirements. Binary distributions must include the license files and notices and provide access to corresponding source; the executable alone does not contain the license text.
