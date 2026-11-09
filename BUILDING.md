# Building the Otakio engine

This branch (`otakio`) is [lpp-vita](https://github.com/Rinnegatamante/lpp-vita) as forked by
[Creckeryop](https://github.com/Creckeryop/lpp-vita) for NOBORU (commit `0ab66ae`), plus the changes that the
[Otakio](https://github.com/Garryth69/otakio) manga reader needs:

| Commit | Change |
|---|---|
| Build with the current VitaSDK and LuaJIT 2.1 | toolchain and library updates |
| Network: TLS up to 1.3, no forced User-Agent, exact response size | modern HTTPS, correct text responses |
| Images: native WebP decoding and System.convertImage | WebP pages and covers, "Save image" re-encoding |

The resulting `eboot.bin` ships inside the Otakio VPK (GPL-3.0, same as lpp-vita).

## Automatic build (GitHub Actions)

`.github/workflows/build.yml` builds the engine on every push to `otakio` with the same toolchain as the published
engine: the VitaSDK autobuild of 2025-03-11 (gcc 10.3, checked by SHA-256), the vdpm packages listed in the workflow, libcurl 8.17.0 rebuilt
against OpenSSL 1.1.1, and NOBORU's libvita2d at a fixed commit. Each run keeps `eboot_unsafe.bin`, `eboot_safe.bin`, `SHA256SUMS` and
`build-info.txt` (toolchain and library versions) as an artifact for 90 days. A tag named `engine-*` also publishes
them as a release.

The build fails if libcurl is not built against OpenSSL 1.1.1 (TLS 1.3).

## Requirements

- Linux or WSL2 (tested on Ubuntu 24.04) with `git`, `make`, `cmake`, `autoconf`, `libtool`, `python3`.
- [VitaSDK](https://vitasdk.org) in `$VITASDK` (tested with the 2025-03 build, gcc 10.3).
- vdpm packages: `libsndfile speexdsp mpg123 opus opusfile luajit libwebp lame flac libvorbis bzip2 zstd`.

## Steps

1. **OpenSSL 1.1.1 and libcurl for TLS 1.3.** The SDK's libcurl is built against OpenSSL 1.0.2 (TLS 1.2 at most).
   - Install the `openssl` 1.1.1 package from vdpm.
   - Rebuild `curl` with the [vitasdk/packages](https://github.com/vitasdk/packages) recipe against it, then install
     it into `$VITASDK/arm-vita-eabi`.
2. **libvita2d (NOBORU fork).**
   ```sh
   git clone https://github.com/Creckeryop/libvita2d ../libvita2d
   make -C ../libvita2d/libvita2d
   ```
   The Makefile looks for it in `../libvita2d/libvita2d`. Override it with `make V2D_DIR=/path/to/libvita2d`.
3. **The engine.**
   ```sh
   make -j8
   ```
   The output is `eboot_unsafe.bin`. Otakio uses it as `source/eboot.bin` (unsafe = full kernel/user access, like NOBORU).

## Notes

- `source/include/ssl_stubs.c` maps `UI_OpenSSL` to `UI_null`. The SDK's OpenSSL has no console UI, and libcurl only
  references it for client-certificate prompts.
- `source/include/dl_stubs.c` provides `dlopen`/`dlsym` stubs. LuaJIT references them, but the Vita has no dynamic loader.
- Lua scripts must set their own `User-Agent`: the engine no longer forces a browser one.
