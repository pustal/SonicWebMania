# SonicWebMania

Sonic Mania for LG webOS TVs: the [RSDKv5](https://github.com/RSDKModding/RSDKv5-Decompilation)
and [Sonic Mania](https://github.com/RSDKModding/Sonic-Mania-Decompilation) decompilations,
built as a **native** webOS app (SDL2), with a few TV-friendly extras:

- **Bring your own data, from your phone.** On first launch the TV shows an address and
  a PIN. Open it on any phone or computer on the same network and upload the `Data.rsdk`
  from your own copy of the game. No USB stick or SSH needed.
- **Save transfer.** The same page lists your saves for download, and lets you upload
  saves from another device.
- **Plays with the remote.** Controllers work as usual; the remote is mapped too (below).
- **Back opens a small menu** at any time: Resume, Restart game, Close app.
- **Light on storage.** Uploads are streamed straight to disk, free space is checked
  before anything is written, and the engine's log is off.

No game assets are included or downloaded by this project.

## Remote control

Hold the remote sideways, like a classic pad:

| Button | Game |
|---|---|
| 2 / 8 / 6 / 4 | Left / Right / Up / Down |
| Red | A (jump) |
| Green | B |
| Yellow | X |
| Blue | Y |
| 0 | Start / Pause |
| Arrow keys, OK | D-pad, Start (unchanged) |
| Back (or Exit) | Port menu |

In the port menu, use 6/4 or the arrows to move, OK/0/Red to choose, Back/Green to resume.
On a controller, the Guide or View/Back button opens the menu; A chooses, B resumes.
The mapping lives in `src/port/RemoteInput.cpp` (`kMappings`).

## Transfer page

- Address: `http://<tv-ip>:47801` (shown on the TV; if that port is taken, the next free
  one up to 47810 is used and shown instead).
- Every API call needs the 4-digit PIN shown on the TV; a new PIN is generated each launch.
- Only devices on private networks (192.168.x.x, 10.x.x.x, 172.16-31.x.x) are answered.
- While the game is running, uploaded files are **staged** and applied the next time the
  game starts (Back > Restart game). The engine never sees a file change under it.
- The Mania Plus content is **off by default** and can be enabled there (see Licence).

## Disk space

Measured from the release build (`cmake --build build-webos --target webos-package`
prints these figures every time):

| What | Size |
|---|---|
| IPK download | ~2.1 MiB |
| Installed app (binary + bundled SDL2) | ~4.5 MiB |
| Your `Data.rsdk` | the size of your file |
| Saves and settings | well under 1 MiB (time-attack replays add a little) |
| Headroom required during an upload | 32 MiB, on top of the file being uploaded |

So the total is roughly **your Data.rsdk + 5 MiB**, and an upload needs
`Data.rsdk + 32 MiB` free while it runs. Replacing the data while the game runs keeps
both copies until the next start, so that needs room for two.

Data lives in the app's own folder (`$HOME/data` on the TV).

## Building

The webOS toolchain is Linux x86_64 only. On a Mac, build inside a Linux VM or a
container (for example `docker run -it -v "$PWD":/src ubuntu:24.04`).

```sh
git clone https://github.com/pustal/SonicWebMania.git
cd SonicWebMania
# build-webos.sh fetches the submodules and applies the engine patch itself.

# Toolchain: https://github.com/openlgtv/buildroot-nc4/releases
#   arm-webos-linux-gnueabi_sdk-buildroot.tar.gz, then run its relocate-sdk.sh
export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
npm install -g @webos-tools/cli    # for ares-package / ares-install

scripts/build-webos.sh             # -> dist/org.sonicwebmania.app_<version>_arm.ipk
```

Install with `ares-install dist/*.ipk` (or `cmake --build build-webos --target webos-install`,
with `-DSWM_ARES_DEVICE=<name>` to pick the TV), or with webOS Dev Manager.
Developer Mode or a rooted TV is required, as for any homebrew app.

### Desktop build (for testing)

```sh
sudo apt install cmake ninja-build libsdl2-dev
git submodule update --init && git -C external/RSDKv5-Decompilation submodule update --init
scripts/apply-patches.sh
cmake -B build -G Ninja && cmake --build build
./build/rsdk/SonicWebMania          # data in ~/.local/share/SonicWebMania (or $SWM_DATA_DIR)
```

Escape opens the port menu on a keyboard.

## How it is put together

```
src/main.cpp                launcher: storage, transfer server, setup screen, then the engine
src/port/
  WebOSPortHooks.h          the C interface the engine calls (5 functions)
  PortRuntime.*             implements the hooks: Back menu, notifications, Plus opt-in
  RemoteInput.*             remote button mapping and menu navigation
  SetupScreen.*             first-run screen (address, PIN, free space, progress)
  TransferServer.*          HTTP server (cpp-httplib) for data and saves
  Storage.*                 data folder, free space, staged uploads, log trimming
  PortConfig.*              port.ini (Plus opt-in)
  BitmapFont.* TextRenderer.cpp   built-in 8x8 font, so text works before any game data exists
web/index.html              the transfer page, embedded into the binary at build time
patches/rsdk/               the engine patch (hook call sites in the SDL2 device + the Plus gate), applied by scripts/apply-patches.sh
cmake/RSDKPlatform-webOS.cmake   RSDK platform file (SDL2 backend, our main, static libstdc++)
cmake/SDL2WebOSBackport.cmake    bundles webosbrew's SDL-webOS 2.30 (remote keys, Back/Exit delivery)
external/                   submodules: engine, game, cpp-httplib, libogg, libtheora
```

## Licence

See `LICENSE.md`. In short: this is a non-commercial project built on the RSDKv5 and
Sonic Mania decompilations by Rubberduckycooly and chuliRMG; their licences apply to
the whole repository. Builds must keep the Plus content off by default, may not be sold
or used for profit, and may not include game assets.

Sonic Mania is a trademark of SEGA. This project is not affiliated with SEGA.
