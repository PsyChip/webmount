# webmount

webmount is a read-only HTTP directory mounter for Windows. `webmount` exposes
remote files as a drive; file data is downloaded as applications read it.

```powershell
webmount "https://example.com/files/"
webmount W: "https://example.com/files/" --detach
```

Use a directory-listing URL. Without a letter, webmount selects the first free
letter from D: through Z:. Explicit letters must be free.

Manual mounts open in Explorer when ready. The drive label is the URL's hostname,
limited to 32 characters. Foreground mounts keep a rolling log; Ctrl+C unmounts
the drive.

## Download

[Windows x64 release](https://github.com/PsyChip/webmount/releases/download/v0.2.0/webmount-0.2.0-windows-x64.zip)

Extract the ZIP and add its folder to `PATH`, or run `.\webmount.exe` from that
folder. Requires Windows 10 or 11.

The first mount installs the bundled WinFsp runtime with administrator approval.
Normal mounting does not require an elevated terminal.

## Supported listings

| Format | Servers |
|---|---|
| HTML | Apache, nginx, IIS, lighttpd, LiteSpeed, nginx fancyindex |
| JSON | Caddy, nginx |
| XML | nginx |

Detection is automatic. Login pages, WebDAV, direct file links, and embedded
credentials are not supported. Supply the directory URL without query parameters
or fragments. Bare hosts use HTTP; use `https://` for HTTPS.

## Commands

- `--detach` runs the mount in the background. `--background` is an alias.
- `--persist` mounts in the background and saves the drive for sign-in.
- `--eject W:` unmounts W: and removes its saved startup entry.
- `--eject` unmounts all drives owned by the current user and removes saved entries.
- `--install` installs the private WinFsp runtime.
- `--uninstall` removes the private runtime and the current user's saved entries.
- `--help` prints usage. Running without arguments does the same.

Install and uninstall request administrator approval. Uninstall stops webmount
mounts across users, but leaves other users' saved entries and separately
installed WinFsp alone. Locked runtime files may require a restart.

## Persistence

```powershell
webmount W: "https://example.com/files/" --persist
```

`--persist` includes background mode and saves the assigned letter. Keep the
executable at its registered path. After moving it, eject and register the mount
again from the new location.

Saved mounts start after Windows sign-in without opening Explorer. Temporary
failures are retried for up to five minutes; occupied drives are never replaced.
Failed restoration keeps the registration. `--eject` removes saved entries even
when their drives are offline.

## File access

The drive is read-only. Files can be opened or copied locally, but not uploaded,
renamed, or deleted. Network access is required.

Listings and file metadata expire after 60 seconds. Browsing prefetches immediate
child listings, not the entire tree. The file-data cache is 64 MiB; total process
memory can be higher. Listings are limited to 16 MiB and 100,000 entries.

Files larger than 64 MiB require HTTP byte-range support. Without it, whole files
up to 64 MiB are supported. The server must supply exact file sizes. If a remote
file changes while open, reopen it; change detection depends on server metadata.

Names Windows cannot represent are escaped. Case-insensitive name collisions get
suffixes. Explorer may show "0 bytes free of 0 bytes" because the directory has
no reported disk capacity.

## Build

Requires CMake 3.24+, Visual Studio 2022 C++ tools, and a Windows SDK.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is written to `build/bin/Release/webmount.exe`. webmount uses
C++20 and links the MSVC runtime statically. WinFsp headers and runtime binaries
are included.

## Command-line reference

```powershell
webmount W: "https://example.com/files/"
webmount W: "https://example.com/files/" --detach
webmount W: "https://example.com/files/" --persist
webmount --eject W:
webmount --eject
webmount --install
webmount --uninstall
webmount --help
```

## License

webmount is licensed under GPL-3.0-only. See [LICENSE](LICENSE).

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos

<https://github.com/winfsp/winfsp>

See [WinFsp's license](vendor/winfsp/License.txt) and
[third-party notices](THIRD_PARTY_NOTICES.md) for terms and corresponding source.
Include the licenses, notices, and source access when redistributing binaries.

---

*Created by [@PsyChip](mailto:root@psychip.net) — October 2026*
