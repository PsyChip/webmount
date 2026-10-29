# webmount

webmount mounts a web directory as a read-only Windows drive. Browse files in
Explorer, open them in your apps, or copy them to your PC. Files download as you
read them.

[Download for Windows x64](https://github.com/PsyChip/webmount/releases/download/v0.2.0/webmount-0.2.0-windows-x64.zip),
extract the ZIP, and open PowerShell in that folder. Requires Windows 10 or 11.

```powershell
.\webmount.exe W: "https://example.com/files/" --detach
```

Replace the example URL with a directory listing. The first mount asks for
administrator approval to install the bundled driver. No separate WinFsp
installation is needed.

When ready, the drive opens in Explorer. Its label is the site's hostname:
`https://3.onj.me/phonetones/unzipped/BlackBerry/` becomes `3.onj.me`.
Labels are limited to 32 characters.

## Commands

| Command | Action |
| --- | --- |
| `webmount <url>` | Mount on the first free drive letter, D: through Z: |
| `webmount W: <url>` | Mount on W: |
| `webmount W: <url> --detach` | Keep the drive mounted after closing the terminal |
| `webmount W: <url> --persist` | Mount in the background and restore at sign-in |
| `webmount --eject W:` | Disconnect W: and remove its saved startup entry |
| `webmount --eject` | Disconnect all your webmount drives and remove saved entries |
| `webmount --help` | Show help |

Use `webmount` if its folder is on your `PATH`; otherwise use `.\webmount.exe`
from the extracted folder. Quote URLs and choose an unused drive letter.

Without `--detach`, the terminal shows a rolling, timestamped log. Keep it open
while using the drive; **Ctrl+C** unmounts it. `--background` is an alias for
`--detach`. Running without arguments shows help.

## Persistence

```powershell
.\webmount.exe W: "https://example.com/files/" --persist
```

`--persist` includes background mode. Keep the executable at the same path.
If you move it, eject the drive and register it again from the new location.

Saved drives return after your Windows sign-in without opening Explorer.
Restoration retries temporary failures for up to five minutes and never replaces
an occupied drive. Failed attempts keep the saved entry for the next sign-in.
Use `--eject W:` to remove it, even when the drive is offline.

## Servers

| Format | Supported listings |
| --- | --- |
| HTML | Apache, nginx, IIS, lighttpd, LiteSpeed, nginx fancyindex |
| JSON | Caddy, nginx |
| XML | nginx |

Detection is automatic. Use a directory-listing URL, not a login page or a link
to a single file. WebDAV and embedded usernames/passwords are not supported.
Remove query parameters and `#fragments` from the supplied URL.

Use `https://` for encrypted connections. Bare hostnames use HTTP. HTTPS cannot
redirect to HTTP; later redirects must stay inside the mounted directory tree.

## Limits

- Read-only. Save edited files to a local drive; uploads, deletes, and renames
  are not supported.
- Network access is required. This is not an offline copy.
- Files larger than 64 MiB need server support for HTTP byte ranges. Without
  ranges, whole files up to 64 MiB can be read. The server must report file sizes.
- Listings and file metadata are cached for 60 seconds. Listed sizes may be
  approximate until a file is opened.
- Browsing prefetches child listings, not every folder or file. The file-data
  cache is 64 MiB; total memory use can be higher. Each listing is limited to
  16 MiB and 100,000 entries.
- Names Windows cannot use are escaped. Names that differ only by case get
  suffixes so both remain accessible.
- Explorer may show "0 bytes free of 0 bytes." The server does not report disk
  capacity; this does not mean the files are empty.

If a file changes while open, close and reopen it. Change detection depends on
metadata supplied by the server. For connection errors, run without `--detach`
to see the log. If persistence rejects a long command, use a shorter executable
path or URL.

## Driver

The driver installs on first use. You can also install or remove it explicitly:

```powershell
.\webmount.exe --install
.\webmount.exe --uninstall
```

Both commands request administrator approval. Uninstall stops webmount drives
across users and removes your saved entries. Other users' saved entries and a
separate WinFsp installation are left alone. Close apps using the drives first;
locked files may require a restart. You can then delete the webmount folder.

## Build

Requires CMake 3.24+, Visual Studio 2022 C++ tools, and a Windows SDK.
WinFsp headers and runtime binaries are included.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/bin/Release/webmount.exe`. Uses C++20 and the static MSVC runtime.

## License

webmount is licensed under **GPL-3.0-only**. See [LICENSE](LICENSE).

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos

WinFsp repository: <https://github.com/winfsp/winfsp>

See [WinFsp's license](vendor/winfsp/License.txt) and
[third-party notices](THIRD_PARTY_NOTICES.md) for its terms and source links.
Redistributed binaries must include the licenses and notices and provide access
to corresponding source.

---

Curated by Psychip — October 2026.
