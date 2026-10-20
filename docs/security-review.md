# Security regression review

This review is limited to the four fixes requested during implementation. Tests added in `tests/test_limits.cpp` use local files and loopback sockets; none of them download media, install packages, launch Chromium, or contact Google.

## Automated regressions prepared

- An existing partial archive larger than the locked archive size must be rejected with `413/download_limit`, and removed, before network access.
- A complete partial archive with the expected SHA-256 must promote to the download cache without network access.
- Promotion must also replace an invalid previous cache on Windows. This specifically exercises the complete-partial branch, rather than the completed-network-download branch.
- Thirty-two incomplete HTTP headers may occupy the configured fixed connection slots. The next accepted connection must close, and closing the original peers must release all slots. Status is observed through the independent local control endpoint.
- A request containing an existing inline image and another remote image near the cumulative request limit must fail with `413/media_limit` before fetching the second image. A private loopback URL intentionally distinguishes budget rejection from URL validation; the assertion requires `media_limit`.

These five regressions passed in the Windows MSVC full suite on 2026-10-20. They remain offline/local checks; the dynamic-network and real-Termux limitations below still apply.

## Static verification and remaining limits

The Termux launcher is now inside the selected `chromium-<version>-<platform>` directory, rather than a shared runtime-level script. `setup_runtime` checks the executable before updating `config.browser_executable`, and records the previous launcher when changing versions. Thus a new-version verification failure does not overwrite the old-version launcher or switch the stored configuration. This is static review only: no real Termux/proot installation or upgrade-failure test was performed.

Media preparation now keeps a cumulative serialized request budget and derives each remote download allowance from the remaining budget. The offline test exercises refusal before an additional fetch. Actual cumulative expansion across several successful HTTPS downloads was not tested because production currently has no injectable media-fetch seam and the review does not use external network fixtures.

The connection cap is acquired immediately after accept and before the header-read coroutine. The live counter is released by the coroutine-owned connection slot. The test checks observable admission and release through real loopback connections.

The downloader now rejects oversized partial files, caps write callbacks including resumed bytes, and has a total 1800-second timeout. Streaming download overflow remains statically reviewed; a local TLS server fixture would be required to exercise the callback without external network access.

A Windows-specific cache-promotion issue was identified: the complete-partial fast path originally renamed onto an existing invalid destination, whereas the completed-download path removed the destination first. The replacement-cache regression above must pass before considering this edge case resolved.
