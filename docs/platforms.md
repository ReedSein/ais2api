# Build, package, and validate each platform

The delivery targets are Windows 10/11 x64 with MSVC, Ubuntu 24.04 and Debian 13
x86_64 with GCC or Clang, and an ARM64 Termux host with Clang. CMake requires 3.25 or newer and
C++20. A desktop build or GitHub Actions job does not certify Android behavior.

The service executable is C++. Node and Python are optional development test
tools. Current official Termux proot-distro separately depends on Python and
python-pip; these belong to its browser-container management tool, not the
service implementation. The official package recipe is the source for this
distinction. [Termux proot-distro recipe](https://github.com/termux/termux-packages/blob/master/packages/proot-distro/build.sh)

## Ubuntu 24.04 / Debian 13 x86_64

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

### WSL 2 with Debian 13

From Windows, enter the distribution using `wsl.exe -d Debian`. Use the Linux
compiler and executable inside this shell. Keep the checkout, build/dependency
trees and service data in the Linux filesystem, for example `~/src/ais2api`,
rather than `/mnt/c` or `/mnt/d`. This also gives the owner-only data directory
and Unix control socket normal Linux permissions. Microsoft recommends native
Linux storage for Linux build performance:
[WSL filesystem guidance](https://learn.microsoft.com/en-us/windows/wsl/interop).

Run the prerequisite commands above, then build and check the installed runtime:

```sh
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  sh scripts/build.sh linux --package --jobs 2
python3 tests/cli_smoke.py --exe build/linux/install/bin/ais2api
python3 scripts/check-package.py build/linux/packages/*.tar.gz
```

The build command scopes `PATH` to Linux tools. WSL's inherited Windows paths
can make repeated CMake tool searches expensive; no global shell or WSL setting
is changed. Add any custom Linux toolchain directory if needed.

Python is only needed for these development checks. For normal use:

```sh
build/linux/install/bin/ais2api init
build/linux/install/bin/ais2api setup --yes --headless=true
build/linux/install/bin/ais2api doctor
```

Set `api_key` in the generated `config.json`; `init` does not generate or print a
key. Run the service as the regular Linux
user; `setup --yes` invokes `sudo` for the listed system packages. It requires a
working sudo authentication session or an interactive terminal. Debian 13 and
Ubuntu 24.04 use the `t64` browser library package names; other distribution IDs
or versions are rejected rather than guessed. Headless browser preparation does
not require WSLg, a desktop session, or Windows Chrome. It verifies local CDP and
isolated context creation, without logging into Google. Account import and real
generation remain separate steps. A Debian-built executable targets Debian's
glibc ABI and does not replace an Ubuntu release artifact.

For a real browser check through the C++ gateway without package installation or
Google access, explicitly run the hidden integration test with a prepared browser:

```sh
AIS2API_TEST_BROWSER=/absolute/path/to/chrome \
  build/linux/ais2api_tests '[.browser-runtime]'
```

It uses a temporary profile, verifies CDP and an isolated context, and checks
process/profile cleanup. It fails if the executable is missing. Normal CTest
runs exclude this test and remain independent of installed browsers.

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

Each archive root also has a one-click launcher: `start.sh` (executable) in the
Linux and Termux `.tar.gz` archives, and `start.cmd` in the Windows `.zip`.
The launcher leaves an already running service alone. Otherwise it runs
`init` and prints hints for an empty `api_key` or the default management
password, and runs `setup` when `doctor` reports no browser. `start.sh` asks
before running `setup --yes`, because that step can install system packages
(it refuses in a non-interactive shell). Windows setup only downloads the
browser, so `start.cmd` runs it without asking. The service then runs with
`serve` in that terminal until Ctrl+C. `--background` uses `start` instead,
and any other arguments (for example `--port 18452`) are passed to every
command. `start.cmd` switches the console to UTF-8 (code page 65001) so
Chinese text displays on GBK systems, and restores the previous code page on
exit. Under 65001, cmd misreads batch files that contain multi-byte text, so
`start.cmd` stays ASCII with CRLF line endings; its messages live in
`share/ais2api/start-messages.txt`. The package checker enforces both. On
Windows, `init` and `start` each wait about five seconds for the local control
pipe when the service is stopped, so the first launch takes a few seconds
longer.

The default archives contain the backend. Optional `Browser-Local` archives
also contain a verified browser. The Linux executable still depends on the target OS ABI;
Ubuntu-built artifacts are not claimed to run on older glibc systems, musl, or
Android. Check dependencies with `ldd bin/ais2api` on the destination.

### Portable data and local browser packages

The executable directory owns `config.json` and `data/` by default. Downloads,
SQLite, setup journals, service logs and browser profiles live under `data/`.
Persisted local paths are relative to the configuration file, so move the entire
folder together. Explicit external paths continue to work. An unwritable program
directory fails rather than redirecting into a user profile. Browser children
receive private HOME/XDG/AppData/temp/cache/log paths; the parent environment is
unchanged. System package managers, OS services and Termux/proot container state
remain managed by their respective tools.

Browser bundles are for your own local/internal use under the separate
[CloakBrowser binary license](https://github.com/CloakHQ/CloakBrowser/blob/main/BINARY-LICENSE.md).
Do not upload a bundled browser as an unrestricted public release. The Python
staging/checking helpers are development tools, not runtime dependencies.

Start from the original archive whose SHA-256 is locked in `runtime-manifest.json`:

```sh
python3 scripts/browser-bundle.py stage \
  --archive /path/to/cloakbrowser-linux-x64.tar.gz --platform linux-x64 \
  --output build/browser-bundle-linux \
  --license-file licenses/CloakBrowser-BINARY-LICENSE.md
sh scripts/build.sh linux --package --browser-bundle build/browser-bundle-linux
python3 scripts/check-package.py --with-browser build/linux/packages/*Browser-Local.tar.gz
```

Windows uses `--platform windows-x64` with the official ZIP, then:

```powershell
./scripts/build.ps1 -Package -BrowserBundleDirectory build/browser-bundle-windows
python scripts/check-package.py --with-browser build/windows/packages/*Browser-Local.zip
```

Use a separate build directory when keeping both variants. CMake's equivalent is
`-DAIS2API_BROWSER_BUNDLE_DIR=/absolute/clean/staged/runtime`. Configure/install
validate its full inventory; CPack includes only the explicit browser directory
and adds `-Browser-Local` to the archive name. Existing profiles/configs/download
caches are rejected, and no runtime database or API key is packaged.
`init` and `start` auto-detect the locked browser beside the executable; `setup`
can verify every file and prepare system libraries. Bundling does not eliminate
Linux OS dependencies or perform Google login.

## ARM64 Termux host

Run the native build outside proot and keep the project in Termux private
storage. The dependency-install flag explicitly changes shared Termux packages:

```sh
pkg install boost-headers
CXXFLAGS=-fexperimental-library sh scripts/build.sh termux --install-deps --build-dir build/termux-native --jobs 1
```

Without `--install-deps`, the script uses already installed dependencies. The
official package names are `clang`, `cmake`, `ninja`, `git`, `curl`, `pkg-config`,
`boost`, `boost-headers`, `libcurl`, `openssl`, `libarchive` and `libsqlite`. Install
`boost-headers` explicitly because the native script's dependency list does not
include that split package. These include the
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

The command above starts a new build tree so CMake reads `CXXFLAGS`. With the
Termux Clang 21 toolchain, `std::osyncstream` requires
`-fexperimental-library`, including the experimental static archive supplied by
`ndk-sysroot`. For an existing configured tree, set
`-DCMAKE_CXX_FLAGS=-fexperimental-library` through CMake instead; changing the
environment alone does not replace cached flags.

### GitHub Actions Termux backend artifact

`.github/workflows/termux.yml` builds the current checked-out commit on
`ubuntu-24.04-arm` inside the official Termux ARM64 container. The image is pinned
by digest; its rolling apt dependencies are upgraded together and their actual
versions are recorded. This is a recorded build environment, not a complete
dependency snapshot. It reuses `scripts/build.sh termux` and the CMake install
list. Desktop workflow definitions and business logic are shared unchanged.
[Official Termux container](https://github.com/termux/termux-docker/blob/master/README.md)

Like the desktop workflow, the Termux workflow runs on pushes and pull requests
without branch or path filters. It builds the checked-out commit, including the
merge preview used by pull-request jobs. Select **Termux ARM64** in Actions to
run it manually for an intended branch. GitHub requires the workflow file to
exist on the repository's default branch before manual dispatch is available.
[Manual workflow dispatch](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)

The job runs CTest and isolated CLI/portable smoke checks without Google access;
browser bridge and userscript checks run in the desktop workflow's bridge job.
It uploads a `termux-aarch64-COMMIT` artifact containing a
`.tar.gz` backend runtime and its `.sha256` sidecar, retained for 28 days.
Extract into a new directory in Termux private storage; Android shared storage
is not an executable installation directory. Verify before using it:

```sh
sha256sum --check ais2api-termux-aarch64-COMMIT.tar.gz.sha256
python scripts/check-package.py --target termux-aarch64 ais2api-termux-aarch64-COMMIT.tar.gz
```

The Python checker is a development tool from this source checkout. The archive
contains `share/ais2api/termux-build.json` with the source commit, image digest,
package versions, ELF loader and linked-library inventory. Dynamic libraries are
provided separately by the Termux package manager; Android supplies its system
libraries. Package versions describe the build, and do not certify every older
runtime. The archive does not carry configuration, accounts, system libraries or
a browser. Its dependency notices use
`share/licenses/ais2api-dependencies/PACKAGE/copyright`, including LLVM's notice
for the linked experimental static archive.

Some split dynamic-library packages do not carry a copyright file. Their
`dependency_notices` entry explicitly records `external-system-package`; this
applies only to separately installed shared libraries. Notices for embedded
Boost/libc++ code and the experimental static archive remain mandatory.

The checker rejects a wrong CPU or loader, missing runtime files, unreported
dependencies, missing applicable notices, bundled shared libraries and known
private paths. Before replacing an existing installation, verify that the target
device can load the program and exercise it with temporary configuration/data
and a separate local port. A successful container job does not certify Google
requests, the Android browser integration or background survival.

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
build, a Debian 13 container/GCC build, and Node bridge syntax/behavior checks. Desktop jobs run the isolated
CLI/IPC/HTTP smoke harness without Google credentials and archive the installed
runtime as an artifact retained for 28 days. The workflow has no path filter that could skip script-only changes.

Workflow definitions are not successful remote runs. Real Google login, private
RPC compatibility, browser downloads and Android process lifecycle remain
separate acceptance checks. The separate Termux workflow provides container
coverage; device acceptance and successful remote runs require their own results.
The Debian container job is not a WSL installation test; local WSL results are
recorded separately in the validation report.
