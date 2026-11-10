# FDC+ Server Tester

Tests an FDC+ Serial Drive Server without an Altair. The tester pretends to be
an FDC+ Enhanced Floppy Disk Controller configured for drive type 7, the
"serial drive". In that mode the FDC+ has no floppy drives attached: it reads
and writes disk images kept on a PC, which it reaches over its serial port.
The tester talks to the server the same way the FDC+ does: it asks the server
for its status over and over, counts the answers, and can read a track from a
disk to check that it arrives intact.

The tester only reads. It never writes, so it cannot change your disk images.

Works with the [FDC+ Serial Drive Server](https://github.com/deltecent/fdc-sds-fltk)
and the original Windows server by M. Douglas (DeRamp). Runs on Windows, macOS,
and Linux as a single program with nothing to install.

Version 1.0. Copyright © 2026 Deltec Enterprises LLC. MIT License.

## Download

| Platform | Download | Notes |
|---|---|---|
| Windows | [fdcsdstest-windows-x64.zip](https://github.com/deltecent/fdc-sds-test-fltk/releases/latest/download/fdcsdstest-windows-x64.zip) | Windows 10 or later. |
| macOS | [fdcsdstest-macos-universal.tar.gz](https://github.com/deltecent/fdc-sds-test-fltk/releases/latest/download/fdcsdstest-macos-universal.tar.gz) | macOS 11 or later, Apple Silicon and Intel. |
| Linux | [fdcsdstest-linux-x86_64.tar.gz](https://github.com/deltecent/fdc-sds-test-fltk/releases/latest/download/fdcsdstest-linux-x86_64.tar.gz) | Needs a desktop with GTK 3. |

**Windows:** open the zip file, drag `fdcsdstest.exe` to your desktop (or any
folder), and double-click it. If Windows shows "Windows protected your PC",
click **More info**, then **Run anyway**.

**macOS:** the program is not signed, so the first time, right-click it in
Finder and choose **Open**.

**Linux:** your account must be in the `dialout` group to use serial ports
(`sudo usermod -aG dialout $USER`, then log out and back in).

## What you need

- The computer running the **drive server**, with a serial port.
- A **second** computer (or a second serial port on the same computer)
  running this tester.
- A **null-modem** serial cable between the two ports. A plain
  ("straight-through") cable will not work.

## How to use it

**Before you start:** start the drive server, choose its serial port and
speed, and load a disk image in Disk 0.

1. Start the tester.
2. In **Serial port**, choose the port the cable is plugged into. On Windows
   these are named COM1, COM2, COM3, and so on. If you plugged in a USB
   serial adapter after starting the tester, click **Find Ports** first.
3. In **Speed**, choose the same speed the drive server uses. 403.2K is the
   usual FDC+ speed.

### Step 1: Is the server answering?

Click the big green **START** button. The tester asks the server for its
status four times a second.

- **Yes! The server is answering.** Everything is connected correctly. The
  **Answered** count goes up, and the green lights under **Disks loaded**
  show which drives have a disk loaded on the server.
- **No answer.** Nothing is coming back. Check that the server is running,
  that the cable is a null-modem cable and is plugged in at both ends, and
  that you chose the right serial port.
- **Garbled answer.** Something is coming back, but it makes no sense. The
  speed almost always does not match: choose the same speed on both sides.

Click **STOP** to stop, and **Clear** to set the counts back to zero.

The list shows every question and answer, newest at the bottom.

### Step 2: Read a track

1. In **Drive**, choose the drive number the disk is loaded in on the server
   (0 for Disk 0).
2. In **Disk type**, choose the kind of disk image: **8" floppy** (330K
   images), **Minidisk** (75K images), or **8 MB serial disk** (8768K images).
3. In **Track**, choose a track number. Track 0 is the first track.
4. Click **READ TRACK**.

The result appears below the button:

- **Read track ... OK. Checksum good.** The track arrived complete and
  undamaged.
- **No answer.** The server has no disk in that drive, or the disk does not
  have that many tracks (check the disk type).
- **Only part of the track arrived**, or **the checksum is BAD.** The data was
  damaged or cut short on the way. Try a slower speed on both sides.

You can read tracks while Step 1 is running.

### Saved settings

The serial port, speed, drive, disk type, track, and window position are
remembered for next time.

## Building from source

Requires CMake 3.20+, a C++17 compiler, and git (to fetch FLTK, which is built
and linked statically).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

The program is `build/fdcsdstest` (`build/Release/fdcsdstest.exe` with Visual Studio).

To run the tests:

    ctest --test-dir build -C Release --output-on-failure

For a universal macOS binary, add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`
when configuring.

Linux needs the X11/Wayland development packages FLTK uses, e.g. on Debian/Ubuntu:

    sudo apt install build-essential cmake git libx11-dev libxext-dev libxft-dev \
        libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libpango1.0-dev \
        libcairo2-dev libwayland-dev wayland-protocols libxkbcommon-dev \
        libdbus-1-dev libgtk-3-dev

### Releases

GitHub Actions builds Windows, macOS, and Linux on every push to `master` and
every pull request, and runs the tests. To publish a release with downloadable
programs, push a version tag:

    git tag v1.0
    git push origin v1.0

The release appears on the
[Releases](https://github.com/deltecent/fdc-sds-test-fltk/releases) page
with the three download files attached.

### Source layout

| File | Purpose |
|---|---|
| `src/main.cpp` | FLTK user interface |
| `src/client.*` | FDC+ protocol client (runs on its own thread) |
| `src/transport.h` | Serial port interface |
| `src/serial_posix.cpp` | Serial port for Linux and macOS |
| `src/serial_linux.cpp` / `serial_macos.cpp` | Custom baud rates, port list |
| `src/serial_win32.cpp` | Serial port for Windows |
| `tests/client_test.cpp` | Protocol client tests against a fake server |
| `tools/make_icon.py` | Draws `resources/fdcsdstest.ico` (run from the repository root) |

The sources contain no conditional compilation; `CMakeLists.txt` picks the
platform files.
