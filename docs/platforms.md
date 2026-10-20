# Build, package, and validate each platform

The delivery targets are Windows 10/11 x64 with MSVC, Ubuntu 24.04 x86_64 with GCC
or Clang, and an ARM64 Termux host with Clang. CMake requires 3.25 or newer and
C++20. A desktop build or GitHub Actions job does not certify Android behavior.

The service executable is C++. Node and Python are optional development test
tools. Current official Termux proot-distro separately depends on Python and
python-pip; these belong to its browser-container management tool, not the
service implementation. The official package recipe is the source for this
distinction. [Termux proot-distro recipe](https://github.com/termux/termux-packages/blob/master/packages/proot-distro/build.sh)

## Ubuntu 24.04 x86_64

Install compiler and build prerequisites using the OS package manager:

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang cmake ninja-build git curl zip unzip tar pkg-config autoconf automake libtool
sh scripts/build.sh linux --package
```

The script fetches the vcpkg commit in `vcpkg.json`, configures, builds, runs
CTest, installs under `build/linux/install`, and writes a TGZ under
`build/linux/packages`. New dependency tools live in `build/deps`; the script
does not update an existing checkout to another commit. To reuse an already
prepared tool checkout, set `VCPKG_ROOT` to that exact pinned revision. Dependency
downloads still require network access or populated vcpkg caches.

Choose another compiler or sanitizer build in a distinct build directory:

```sh
CC=clang CXX=clang++ BUILD_TYPE=RelWithDebInfo sh scripts/build.sh linux --sanitizers --build-dir build/linux-clang-asan
CC=clang CXX=clang++ BUILD_TYPE=RelWithDebInfo sh scripts/build.sh linux --fuzzer --build-dir build/linux-fuzz
build/linux-fuzz/ais2api_fuzz_wire -max_total_time=30 -max_len=65536
```

`AIS2API_BUILD_FUZZER=ON` is optional and requires desktop Linux Clang plus
`AIS2API_SANITIZE=ON`. It instruments the core and links the wire-parser harness
with LLVM libFuzzer. Sanitizer and short fuzz smoke runs are evidence for the
exercised inputs, not proof that no memory errors exist.

## Windows x64

Install Visual Studio C++ build tools (including the Windows SDK), Git, CMake,
and Ninja. CMake, CTest and Ninja must be on PATH. From PowerShell:

```powershell
./scripts/build.ps1 -Package
```

The script imports Microsoft's installed Developer PowerShell when necessary,
uses `x64-windows-static`, checks the vcpkg baseline, builds and runs CTest.
The installation is `build/windows/install`; the ZIP is under
`build/windows/packages`. Use `-VcpkgRoot PATH` to reuse an existing pinned
checkout, or `-BuildDirectory PATH` for a separate build. It does not reset an
existing dependency checkout. `/MT` is configured for the project and the
selected vcpkg triplet so the package does not depend on a separately installed
MSVC redistributable for project code.

## Install and archive layout

Both native desktop builds support the same direct CMake operations:

```sh
cmake --install build/linux --prefix out/linux
cmake --build build/linux --target package
```

Use `build/windows` for the Windows build tree. Archives include `bin/ais2api`
(or `.exe`), `bin/browser/bridge.js`, `bin/runtime-manifest.json`, schemas, license
and this platform guide. Desktop vcpkg dependency copyright files and a package
SHA-256 sidecar are included. They exclude `.tools`, source analysis copies, test
executables, account data, browser downloads and configuration secrets. CPack
uses the explicit CMake install list; ZIP/TGZ support comes from its archive
generator. [CPack Archive documentation](https://cmake.org/cmake/help/latest/cpack_gen/archive.html)

`python3 scripts/check-package.py PATH_TO_ARCHIVE` verifies required runtime
assets, dependency licenses, SHA-256 and exclusion of known private/build paths
without extracting the package. This helper requires Python 3.11+ for development.

These are runtime archives, not browser bundles. Run the application's runtime
setup separately. The Linux executable still depends on the target OS ABI;
Ubuntu-built artifacts are not claimed to run on older glibc systems, musl, or
Android. Check dependencies with `ldd bin/ais2api` on the destination.

## ARM64 Termux host

Run the native build outside proot and keep the project in Termux private
storage. The dependency-install flag explicitly changes shared Termux packages:

```sh
sh scripts/build.sh termux --install-deps --jobs 1
```

Without `--install-deps`, the script uses already installed dependencies. The
official package names are `clang`, `cmake`, `ninja`, `git`, `curl`, `pkg-config`,
`boost`, `libcurl`, `openssl`, `libarchive` and `libsqlite`. These include the
headers needed for native CMake builds; do not substitute desktop `*-dev` package
names. Official recipes: [Clang subpackage](https://github.com/termux/termux-packages/blob/master/packages/libllvm/clang.subpackage.sh),
[Boost](https://github.com/termux/termux-packages/blob/master/packages/boost/build.sh),
[libcurl](https://github.com/termux/termux-packages/blob/master/packages/libcurl/build.sh),
[libarchive](https://github.com/termux/termux-packages/blob/master/packages/libarchive/build.sh),
[libsqlite](https://github.com/termux/termux-packages/blob/master/packages/libsqlite/build.sh).

Tests prepare Catch2 3.16.0 from its official source archive, verifying the same
SHA-512 used by the pinned vcpkg port. Production-only builds can use
`--skip-tests`. Catch2 preparation uses `build/deps`; no vcpkg Android cross-build
is presented as a native Termux test. [Catch2 release](https://github.com/catchorg/Catch2/releases/tag/v3.16.0)

The browser runs in Ubuntu userland while the C++ service remains on the Termux
host. The current official proot-distro interface uses OCI images and named
containers. The following demonstrates syntax; application setup should create
its own managed browser container:

```sh
pkg install proot-distro
proot-distro install ubuntu:24.04 --name example-browser
proot-distro login example-browser -- /bin/true
# Syntax example only; application setup creates its actual binding launcher:
proot-distro login example-browser --bind /absolute/host/path:/absolute/guest/path -- /bin/true
```

Use `--name` rather than deprecated `--override-alias`. Current rootfs layout is
`$PREFIX/var/lib/proot-distro/containers/NAME/rootfs`; the official tool migrates
legacy `installed-rootfs/NAME` installations at login. Do not use a directory
layout probe as a substitute for a successful container login.
[Official proot-distro command and storage reference](https://github.com/termux/proot-distro/blob/master/README.md)

Termux acceptance must run on an actual ARM64 Android device: native compile and
CTest; runtime setup and browser `--version`; account probe with an authorized
test account; a real text and multimodal generation; incremental streaming,
consumer disconnect and cancellation; account switching; service restart;
interrupted download/setup recovery; and background survival under that device's
battery policy. Retain platform/tool versions, commands, exit codes and redacted
results. This work adds no claim that these device or account checks have run.

## Automated coverage and limits

`.github/workflows/build.yml` defines Ubuntu 24.04 GCC and Clang builds with
ASan/UBSan both enabled and disabled, a short Clang fuzz smoke, a Windows MSVC
build, and Node bridge syntax/behavior checks. Desktop jobs run the isolated
CLI/IPC/HTTP smoke harness without Google credentials and archive the installed
runtime. The workflow has no path filter that could skip script-only changes.

Workflow definitions are not successful remote runs. Real Google login, private
RPC compatibility, browser downloads and Android process lifecycle remain
separate acceptance checks; no Termux job or badge is fabricated here.
