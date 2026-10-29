# webmount

Mount a web directory as a read-only Windows drive. Browse it in Explorer, open files in your usual apps, or copy files to a local folder without downloading the entire directory first.

**Windows 10/11 · x64 · HTTP and HTTPS**

## Download and start

1. Download [webmount for Windows x64](https://github.com/PsyChip/webmount/releases/download/v0.2.0/webmount-0.2.0-windows-x64.zip) from [release v0.2.0](https://github.com/PsyChip/webmount/releases/tag/v0.2.0).
2. Extract the ZIP to a folder you intend to keep, such as `C:\Tools\webmount`.
3. Open PowerShell in that folder and mount a directory:

```powershell
.\webmount.exe W: "https://example.com/files/" --detach
```

**Replace `https://example.com/files/` with your server's directory-listing URL.** It must be a browsable file listing, not a normal website, a file-sharing landing page, or a direct link to one file.

The first mount asks for administrator approval to install the bundled filesystem driver. You do not need to install WinFsp separately or keep PowerShell elevated.

Once the mount succeeds, **W:** opens automatically in Explorer. Its label is the hostname from your URL: for example, `https://3.onj.me/phonetones/unzipped/BlackBerry/` becomes **3.onj.me**. Hostnames longer than 32 characters are truncated for the drive label.

To reopen the drive later:

```powershell
explorer.exe W:\
```

`--detach` keeps the drive mounted after you close the terminal. Files are fetched over the network as applications read them. You can copy files **from** the drive, but cannot save changes **to** it.

## Everyday commands

Run these commands from the folder containing `webmount.exe`. If you add that folder to your user `PATH`, you can use `webmount` instead of `.\webmount.exe` from any folder.

### Let webmount choose a drive letter

```powershell
.\webmount.exe "https://example.com/files/" --detach
```

The assigned letter is printed after mounting. Automatic selection uses the first free letter from **D:** through **Z:**. To choose your own, put a free letter before the URL, as in the W: example above. Existing drives are never replaced.

### Watch activity in the terminal

Leave off `--detach` to keep the mount in the foreground:

```powershell
.\webmount.exe W: "https://example.com/files/"
```

Keep this terminal open while using the drive. Activity appears as a rolling, timestamped log: new listing, download, cache, and error events append new lines instead of replacing earlier entries. Scroll back through the terminal to review previous activity. Redirected output uses the same line-by-line log. Press **Ctrl+C** to unmount.

### Restore a drive when you sign in

```powershell
.\webmount.exe W: "https://example.com/files/" --persist
```

`--persist` mounts in the background and remembers the drive for your next Windows sign-in. You do not need to add `--detach`.

- Keep the executable in the same folder. If you move it, run the command again from its new location.
- Without an explicit letter, webmount remembers the letter it actually assigned.
- If the network is unavailable or the letter is busy, restoration retries transient failures for up to five minutes. It does not take over another drive.
- A failed restoration keeps the saved entry for a later sign-in. To retry now, run the same `--persist` command again once the letter is free.

Persistence is per user and starts after sign-in, not before it as a Windows service. The initial `--persist` command opens the ready drive in Explorer; automatic sign-in restoration does not open an Explorer window.

### Disconnect a drive

```powershell
.\webmount.exe --eject W:
```

This also removes W:'s saved startup entry, even if the drive is currently offline. To disconnect all of your webmount drives and remove all of your saved entries:

```powershell
.\webmount.exe --eject
```

### Show command help

```powershell
.\webmount.exe --help
```

Running without arguments also shows help. `--background` is an alias for `--detach`.

## Which servers work?

webmount automatically recognizes these directory listings:

| Listing format | Supported servers |
| --- | --- |
| HTML | Apache, nginx, IIS, lighttpd, LiteSpeed, nginx fancyindex |
| JSON | Caddy, nginx |
| XML | nginx |

Use the directory's URL, preferably with an explicit `https://` prefix. Bare hostnames default to unencrypted HTTP.

Login pages, embedded usernames/passwords, WebDAV, and arbitrary web pages are not supported. Remove sorting/filter query parameters and `#fragment` suffixes from the URL you pass to webmount. HTTPS connections cannot redirect down to HTTP; after the initial directory resolves, redirects must stay inside the mounted directory tree.

## What to expect

- **Read-only:** open and copy remote files; save edited copies to a local drive. Uploading, deleting, and renaming are not supported.
- **Drive capacity:** Explorer may show “0 bytes free of 0 bytes.” The web directory has no reported disk capacity; this does not mean its files are empty. webmount does not invent a capacity or change Explorer's global display settings.
- **Network required:** this is not an offline mirror. Opening or seeking through a file can trigger downloads.
- **Large files:** the server must support HTTP byte-range requests for files larger than 64 MiB. Without range support, webmount can fetch whole files up to 64 MiB. The server must expose an exact file size.
- **Refresh delay:** directory listings and file metadata are cached for 60 seconds. Sizes shown in listings can be approximate until a file is opened.
- **Browsing:** webmount prefetches immediate child directory listings, not every folder recursively or every file's contents.
- **Memory:** the file-data cache is 64 MiB. This is not a limit on total process memory. A directory response is limited to 16 MiB and 100,000 accepted entries.
- **Filenames:** names Windows cannot represent are escaped. Names that differ only by case receive suffixes to keep them distinct.
- **Changing files:** when the server supplies ETag or Last-Modified metadata, webmount uses it to detect changes during reads. Servers without those validators cannot guarantee a consistent view of a file being updated.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| Drive letter is already in use | Choose another letter, or omit it for automatic selection. |
| Page is not a supported directory listing | Open the URL in a browser and check that it lists files. Use the actual directory URL rather than a login or sharing page. |
| A large file fails to open or read | Check that the server provides file sizes and supports byte-range requests. Without ranges, the whole-file limit is 64 MiB. |
| Remote file changed while it was open | Close and reopen the file to fetch current metadata. |
| A saved drive does not return after sign-in | Check connectivity, the saved drive letter, and that `webmount.exe` has not moved. Then rerun the `--persist` command if the drive is not mounted. |
| Startup command is too long | Use a shorter executable path or directory URL. Windows limits the saved startup command to 260 characters; webmount requires it to be shorter than that limit. |
| Driver setup was cancelled | Retry the mount and approve the Windows administrator prompt, or ask an administrator to install the runtime. |

For connection or listing problems, mount without `--detach` to see activity and errors in the terminal.

## Driver setup and removal

The bundled driver normally installs automatically on first use. To install it ahead of time:

```powershell
.\webmount.exe --install
```

To remove webmount's driver and private runtime:

```powershell
.\webmount.exe --uninstall
```

Installation and removal request administrator approval. Uninstall removes your saved startup entries and stops webmount mounts across users before removing the shared runtime. It does not remove other users' saved startup entries or a separately installed copy of WinFsp. Close applications using the drives first; locked files may require a Windows restart to finish removal.

After uninstall completes, you can delete the extracted webmount folder.

<details>
<summary>Power-user detail: startup registrations</summary>

Saved commands are stored in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` under names such as `WebMount-W`. Use `--eject W:` to remove an entry and disconnect its drive together.

</details>

<details>
<summary>Build from source</summary>

Use CMake 3.24 or newer and Visual Studio 2022 with the Desktop development with C++ workload, MSVC, and a Windows SDK. From the source directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is `build/bin/Release/webmount.exe`. For a Debug build, use `--config Debug`; the output is `build/bin/Debug/webmount.exe`.

The project uses C++20 and the static MSVC runtime. WinFsp headers and signed x64 runtime binaries are included under `vendor/winfsp/`; no separate WinFsp SDK installation is required. The DLL and driver are embedded into the executable.

</details>

## License and credits

Curated by Psychip. webmount's original source and project documentation are licensed under **GPL-3.0-only**. See [LICENSE](LICENSE). The software is provided without warranty.

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos

WinFsp repository: <https://github.com/winfsp/winfsp>

WinFsp retains its [upstream license and additional permissions](vendor/winfsp/License.txt). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for source links and redistribution details. If you redistribute the executable, include the licenses and notices and provide access to corresponding source; the executable alone does not contain the license text.
