# Install on PS5

Build the application locally, then transfer one folder to your console. You supply the game image and author texture pack. This repository does not distribute game files, generated game code or precompiled application packages.

## 1. Prepare your inputs

- Your own legally obtained `NPJB40001` image with English translation v6.3.0. The assistant never downloads an image, translation patch or extracted game files. It checks the disc identity and supported executable before building.
- The [Enhanced HD texture pack from its authors](https://github.com/HolographicWings/Monster-Hunter-Portable-3rd-HD-Pack-Enhanced), extracted with `NPJB40001/textures.ini` present. Keep the original download and source directory. The assistant prepares mappings locally; eight supplemental resource maps are obtained directly from [David-vz's project](https://github.com/David-vz/MonsterHunterPortable3rdHDRemake) with pinned checksums. Texture authors retain their rights and terms. Missing mappings use the original game textures. An already prepared pack is verified and reused without regeneration.
- About **40 GiB free**, plus twice the ISO size and three times the texture directory size. Build time depends on the machine. Use a writable checkout and keep the input ISO and texture directory outside the selected output.
- On PS5: your supported jailbreak environment already running, [kstuff-lite v1.11](https://github.com/EchoStretch/kstuff-lite/releases/tag/v1.11), an ELF loader and a title launcher/file manager.

## 2. Set up the build computer

The entry point uses Bash 3.2-compatible syntax and Python 3.9+. Compilation runs with LLVM 18 on Linux amd64. All dependencies are public source checkouts or source downloads pinned in `dependencies.lock.json`; retain that file.

### Linux or WSL

Use a checkout path without spaces or shell metacharacters (for example `~/mh3rd-ps5`); the upstream native Make recipes require this. Your ISO and texture input paths may contain spaces. Use Ubuntu 24.04 amd64, or equivalent packages providing these tools:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git python3 \
  python3-yaml python3-ply python3-packaging python3-pyelftools pkg-config \
  clang-18 lld-18 llvm-18-dev libclang-18-dev libclang-cpp18-dev \
  libclc-18 libclc-18-dev libllvmspirvlib-18-dev glslang-tools spirv-tools \
  bison flex rsync xz-utils zlib1g-dev libvulkan-dev libelf-dev \
  libexpat1-dev libzstd-dev ca-certificates
```

In WSL use Linux paths such as `/mnt/c/...`, not `C:\...`. A checkout on the WSL Linux filesystem is faster and preserves POSIX modes. Windows mounts may expose synthetic modes; the assistant warns about this, and the console permission step is always required.

### macOS

Install Bash 3.2+, Python 3.9+ and Docker with Linux amd64 support. The same command automatically builds the public toolchain container from `tools/Containerfile` on macOS. Apple Silicon requires amd64 emulation.

Linux users may select the container with `--container`. Use separate checkouts for native and container builds: CMake caches contain absolute paths. The container mounts your ISO and texture inputs read-only. Its build context excludes private inputs and outputs.

## 3. Generate the installation

From this repository's root:

```sh
bash tools/prepare_install.sh \
  --iso "/path/to/your/NPJB40001-v6.3.0.iso" \
  --textures "/path/to/texture-pack/NPJB40001"
```

The assistant checks requirements, free space, image identity and texture inputs **before creating build outputs**. It then obtains the pinned dependencies, prepares your game files and textures, compiles the application and permission payload, and assembles `out/data/install/`.

The first build may take a long time, especially with two compiler jobs or Windows-mounted storage. Logs and symbols remain under `out/data/`. The source checkouts in `upstream/` and `deps/` are ignored. All extracted/generated game content remains under ignored `out/data/`. Original inputs are read-only throughout preparation.

Options:

- `--preflight-only`: check without downloads, builds or output writes. For the container backend, the toolchain image must already exist; otherwise this check explains the missing requirement without building it.
- `--resume`: continue a failed build using matching sources and inputs. Completed files are verified; CMake continues partial compilation. Existing different outputs are preserved and rejected. For a completed installation, resume verifies the folder and makes no changes to it.
- `--output out/data/my-install`: choose another private transfer folder. Build directories are reserved. Different sources or inputs need a new output folder.

Repeat the original command with `--resume` after an interruption. A failed source checksum, incompatible resource or conflicting file stops preparation with an error. Inspect the reported log; do not transfer partial output. The folder is ready only after **“Local installation hashes/layout PASS”** and **“Install folder ready”**.

```text
out/data/install/
  PPSA99373/
    eboot.bin
    sce_module/libc.prx
    sce_sys/
    yakumo/game/game.iso
    yakumo/game/EBOOT.ELF
    yakumo/textures/NPJB40001/
    yakumo/saves/ms0/PSP/SAVEDATA/
    yakumo/assets/
  mhp3rd-ps5.elf
  README.md
  LICENSE
  LICENSES/
```

This is one private handoff folder. The ISO and textures are included only in your local folder for transfer and are never published by the project. No ZIP is produced. Keep your author texture download separately. Preserve existing saves, settings, logs and caches when copying later builds.

New settings default to **x6 internal resolution, 60-FPS presentation and HD textures enabled**. The game simulation runs at 30 FPS. L3+R3 opens web settings; touchpad toggles HUD, R2 maps Select, and holding L2 enables fast-forward. Existing saved preferences are retained.

## 4. Transfer over USB or FTP

Keep the game closed during transfer and permission preparation.

1. Copy the **contents** of `out/data/install/` to `/data/homebrew/`. The title must be `/data/homebrew/PPSA99373/`; `mhp3rd-ps5.elf` and the licenses stay outside that title folder.
2. For USB, use a console file manager that can copy from the drive into `/data/homebrew/`. For FTP, run [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) with your ELF loader and connect to the console address, normally port `2121` (use its actual configured port).
3. Copy or merge **without deletion**. Preserve the installed ISO, textures, saves, settings, logs, caches and unknown files. Disable mirror, synchronize-with-delete and cleanup options. Wait until **all transfers have finished**.

Exact console locations:

| Content | Path |
| --- | --- |
| Game image | `/data/homebrew/PPSA99373/yakumo/game/game.iso` |
| Textures | `/data/homebrew/PPSA99373/yakumo/textures/NPJB40001/` |
| Savedata root | `/data/homebrew/PPSA99373/yakumo/saves/ms0/PSP/SAVEDATA/` |
| Game save | `ULJM05800` inside that savedata root |
| Saved quest data | `ULJM05800QST` inside that savedata root |

Restore existing `ULJM05800` and `ULJM05800QST` folders there if needed, by merging without deleting other data.

## 5. Apply permissions and launch

Run `/data/homebrew/mhp3rd-ps5.elf` with the ELF loader **after transfer finishes**. Wait for its success notification:

> Permissions applied … Launch the game now.

Only then launch `MHP3rd` from the title launcher. If the payload reports an error or no confirmed success, do not launch yet; keep the files and inspect the error. Repeat this permission step after every installation/update that copies files.

The payload prepares permissions only inside `PPSA99373`, preserving its game files and user data.

## 6. Enjoy the hunt!

You are ready to play. Press **L3 + R3** to open the [in-game web settings](README.md#in-game-web-settings). Save your changes and restart the game to apply them.

Your progress is stored in `ULJM05800`, with saved quest data in `ULJM05800QST`. Keep these folders when updating the game or moving to another installation.

Online play has been reported working with PPSSPP on PC and Android. Other PS5 and PPSSPP setups are expected to work together.
