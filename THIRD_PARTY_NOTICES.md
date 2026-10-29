# Third-party notices

## WinFsp

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos

Repository: <https://github.com/winfsp/winfsp>

### License

WinFsp is licensed under GPLv3 with the additional FLOSS permissions stated in [vendor/winfsp/License.txt](vendor/winfsp/License.txt). Preserve that file and the notices in the vendored headers. webmount's GPL-3.0-only declaration does not replace WinFsp's upstream terms or remove its additional permissions.

The FLOSS exception specifically permits linking with the platform DLL and distributing unmodified upstream installer releases, subject to its conditions. webmount instead embeds the DLL and SYS files separately; do not treat the installer exception as permission to omit GPL source-distribution requirements for those files.

### Version and source

- Upstream release: [WinFsp 2025, tag v2.1](https://github.com/winfsp/winfsp/releases/tag/v2.1).
- Runtime file version: `2.1.25156.ddca7bd`.
- Release source commit: `ddca7bd5481857a65ba552f643b8776fd070836f`.
- Pinned source tree: <https://github.com/winfsp/winfsp/tree/ddca7bd5481857a65ba552f643b8776fd070836f>.
- Pinned source archive: <https://github.com/winfsp/winfsp/archive/ddca7bd5481857a65ba552f643b8776fd070836f.zip>.
- Upstream build instructions: <https://github.com/winfsp/winfsp/blob/ddca7bd5481857a65ba552f643b8776fd070836f/doc/WinFsp-Building.asciidoc>.
- Upstream installer: <https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi>.
- Upstream-published installer SHA-256: `073A70E00F77423E34BED98B86E600DEF93393BA5822204FAC57A29324DB9F7A`.

The upstream release tag resolves to the commit above, matching the commit suffix in both local runtime file versions. The pinned upstream README was accessible during preparation. The vendored binaries were not rebuilt or byte-compared with files extracted from the upstream installer; the version relationship and local signature checks are the provenance evidence recorded here, not a reproducible-build claim.

### Bundled files

| Path | Purpose |
| --- | --- |
| `vendor/winfsp/include/winfsp/winfsp.h` | User-mode API declarations |
| `vendor/winfsp/include/winfsp/fsctl.h` | Filesystem control declarations |
| `vendor/winfsp/winfsp-x64.dll` | x64 user-mode runtime |
| `vendor/winfsp/winfsp-x64.sys` | x64 filesystem driver |
| `vendor/winfsp/License.txt` | Upstream license and additional permissions |

Local binary fingerprints:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `winfsp-x64.dll` | 198376 | `08D7389B8D030770A4DE108A60A86047D5CDD00B9ECAFC6DD66E957BC51C2437` |
| `winfsp-x64.sys` | 168600 | `03553FFFACD362F4A9A08C00B4F236A82354A183BC6028494FB32055386E13C9` |

Windows Authenticode checks on the preparation host reported both signatures valid on 2026-10-29 local time. The DLL signer was NAVIMATICS LLC; the SYS signer was Microsoft Windows Hardware Compatibility Publisher. Signature validity is time- and host-dependent and does not establish a byte-for-byte match to the upstream installer.

### Redistribution

When publishing this repository with its vendored binaries, keep these source directions alongside them. When distributing a built `webmount.exe`, include `LICENSE`, `README.md`, this notice, and `vendor/winfsp/License.txt` with it. The executable currently embeds the WinFsp DLL and SYS payloads but not the license text.

Provide recipients equivalent, no-charge access to the corresponding webmount source for the exact distributed version, including its build configuration, and the corresponding WinFsp source linked above. Put those directions next to the binary download. Under GPLv3 section 6(d), the source may be on another server, but the distributor remains responsible for its continued availability. Preserve or mirror the matching source if upstream access is no longer sufficient.

This preparation pass does not create a binary release package or a source mirror.
