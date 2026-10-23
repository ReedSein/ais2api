# Native browser gateway

The C++ process launches the configured Chromium/CloakBrowser executable directly.
On Termux that executable is the setup-generated launcher into the project's
proot environment. There is no Python, Node, Playwright or external CDP service.

Every account has a permanent browser fingerprint. When an account is first
stored (CLI import, management import or verified Cookie import) it receives a
CloakBrowser seed in 10000..99999; re-imports and credential updates keep it, and
older stores assign one to each account on first open. The browser is launched
with `--fingerprint=<seed>`, `--fingerprint-platform` (`macos` on macOS hosts,
otherwise `windows` for every account), `--fingerprint-timezone` and
`--fingerprint-locale`. Timezone and locale follow this computer's region
settings unless the account overrides them (IANA zone names and BCP 47 tags): on
Windows the user locale and the system zone, on POSIX `TZ`, `/etc/localtime`,
`/etc/timezone` and `LC_ALL`/`LC_MESSAGES`/`LANG`, on Android/Termux the system
properties; detection falls back to `UTC` and `en-US`. CloakBrowser applies the
locale to `navigator.language(s)`, `Intl` and `Accept-Language`. Without a seed
CloakBrowser picks a random device on every launch, and without timezone/locale
it presents UTC + en-US. `--force-webrtc-ip-handling-policy=disable_non_proxied_udp`
keeps WebRTC from sending UDP outside the configured proxy, so ICE candidates
cannot reveal the real address. Other Chromium builds ignore the fingerprint
switches. Because CloakBrowser applies fingerprints per process, using an account
whose identity differs from the running browser's relaunches the browser;
accounts that share an identity reuse it. The management API and WebUI show the
fingerprint, allow region overrides and regenerate the seed on request; changes
take effect the next time the account is used. Diagnostics report the running
identity as `fingerprint`.

The gateway uses a private random profile and reads only its DevToolsActivePort.
It connects Boost.Beast to the loopback browser websocket, creates one incognito
BrowserContext, installs storage-state cookies, creates a target in that context
and attaches using flattened CDP sessions. Switching accounts disposes the old
context first. Cookies and page snapshots are never written to diagnostic logs.
Windows descendants belong to a kill-on-close Job Object. POSIX launches own a
new process group; shutdown targets only that group. Runtime profiles are removed
after shutdown. Imported credentials remain in the Store and are not erased.

Google rotates session cookies (for example `__Secure-1PSIDTS`) while a page is
in use. After a new account context passes identity verification, and at most
every five minutes after a successful generation, the gateway reads that
context's cookies and writes them back to the account. The browser's Google
cookies replace the stored Google set; stored cookies for other domains and
partitioned cookies the browser does not export are kept. An export without a
SAPISID-family and a SID-family cookie is never saved. The update is a
compare-and-swap against the credentials the context was built from, so a
concurrent import or deletion always wins; identity, expiry, cooldowns and quota
counters are not changed. The written-back state becomes the context's baseline,
so it does not trigger a rebuild. Diagnostics report the last result as
`cookieRefresh` (`saved`, `unchanged`, `failed` or `not_attempted`); cookie
values are never logged.

The bridge is original JavaScript in `browser/bridge.js`, embedded as bytes into
the executable by CMake. Generation always uses the embedded copy; stale adjacent
scripts and developer source directories cannot override it. Packages retain
`browser/bridge.js` as a reference asset. Diagnostics expose `bridgeSource=embedded`,
the embedded SHA-256 and the current adapter phase; browser failures log those
safe fields without raw JavaScript exceptions or page data. It discovers a live
MakerSuite snapshot adapter, observes its real service argument, and captures a
real GenerateContent request from the page. A small bootstrap prompt is inserted
and Run is clicked. Captured bootstrap generation and prompt-save fetches receive
empty local responses to avoid sending the bootstrap generation or saving chat
history. Bootstrap best-effort enables Temporary chat and stops/restores the
draft when complete or failed. Both fetch and XHR capture are supported; changed
application internals are reported as `browser_incompatible`.
There is no invented request template or snapshot fallback.

Before each request the bridge obtains a fresh page-service snapshot of the
content hash. This anti-abuse ticket is separate from part-level thoughtSignature
metadata handled by the wire codec. Request encoding uses the live captured
template. Hashing uses the final encoded contents, including expanded tool-result
attachments; code and tool parts contribute empty fields to the single-space
join. Generation and model discovery rebuild SAPISIDHASH, SAPISID1PHASH and
SAPISID3PHASH with a fresh timestamp from the selected page's CDP signing-cookie
cache, including HttpOnly cookies. SAPISID takes priority over __Secure-1PAPISID;
missing 1P/3P values fall back to the primary value. The cache refreshes on missing
or expired signing cookies and after 60 seconds, and is cleared when its account
context is disposed or the browser closes. Valid cached cookies can survive a
temporary refresh failure; cancellation propagates. If native signing is
unavailable, only that page's captured Authorization is used as a fallback.
Missing both sources fails locally. Every send sets the verified account slot
and rejects conflicting captured slots. Signing cookies and headers remain in
memory and are never returned by diagnostics or written to logs.

Generation and model discovery also query GetAiStudioBenefitTier using the live
RPC endpoint and current account authorization. The bounded query has an
eight-second timeout and 8 KiB response limit; its per-page cache lasts ten
minutes, with a thirty-second retry delay after failure. Captured tier headers
are removed. Only a verified plus/pro/ultra result adds TIER0/TIER1/TIER2;
free and unknown omit the header. Each generation records the tier used at send
time, so later cache expiry cannot change error classification. An unknown-tier
403 ends the request without cooling down or rotating healthy accounts.

Streaming uses fetch's BYOB reader with a fresh 32 KiB buffer. Only one base64
chunk can be pending. C++ decodes the chunk and incrementally parses depth-three
wire records. The EventSink must return only after downstream consumption; only
then does C++ ACK the sequence and permit the next browser read. It never builds
or persists the complete raw response. Unsupported BYOB implementations fail
explicitly. Cancellation, generation and idle deadlines abort the page fetch;
CDP operations also have bounded timeouts and cancellation checks. Streaming
requests use the idle deadline and may run longer than generation_seconds while
bytes continue arriving; nonstreaming generation also has the total deadline.
HTTP-200 google.rpc.Status error envelopes are classified before chunk decoding:
RESOURCE_EXHAUSTED, PERMISSION_DENIED and UNAUTHENTICATED map to 429, 403 and 401.

Failed attempts that delivered no content follow one policy
(`include/ais2api/retry_policy.hpp`), within max_attempts:

- Recognized ambiguous GenerateContent routing 404s retry the same account,
  rebuilding the body and snapshot in its verified context. One empty 204 on
  the first attempt may retry once. Ordinary 404s and other client errors,
  region blocks and generation/idle deadlines return without touching accounts.
- Upstream 5xx and incomplete transport retry the same account after a short
  pause (0.5 s, 1 s, ...) without any cooldown.
- An expired or mismatched BotGuard snapshot is reported as PERMISSION_DENIED.
  A 403 therefore first rebuilds the account's page session in place (new
  context, cookies, page, BotGuard and request template; the browser process
  stays) and retries the same account. Only a repeated 403 cools the model; an
  unknown-tier 403 then returns without cooling.
- RPC UNAUTHENTICATED, unfinished identity and signing failures likewise rebuild
  once; a repeat sets a five-minute account-wide `auth` cooldown. A different
  Google account behind the cookies (`identity_mismatch`) sets that cooldown
  directly. Only a sign-in redirect that persists on a fresh session, or an
  account without cookies, marks the account expired.
- Browser incompatibility, disconnection, CDP timeouts and launch failures
  restart the browser once and retry the same account, without cooldown.
- Each request performs at most one rebuild or restart. Switching to another
  account always starts a new session; the browser process is relaunched only
  when the next account's fingerprint differs from the running one.
Responses require a non-unspecified finish reason as well as complete transport
framing. Delivered content is never regenerated after failure. Generation quota
cooldowns are model-scoped: minute limits wait at least sixty seconds without
increasing daily quota counters; other 429s use 60/60/300/300/600 seconds, reset
their counters at Pacific midnight and preserve drip counters after success.
Retry-After can extend the delay up to one day. Known-tier permission failures
that persist after a session rebuild temporarily cool the affected model for five
minutes. Credential replacement
does not clear outstanding quota cooldowns. Existing explicit global cooldowns
and management reset operations retain their meanings.
Non-200 replies retain only status, parsed Retry-After and at most 8 KiB of error
text; malformed stream classification retains at most a 16 KiB prefix. None of
this error text is logged or returned to clients. A sink that
blocks must itself observe its HTTP request's cancellation/deadline: the gateway
cannot interrupt arbitrary caller code while it executes.

Probe validates page readiness, login/region redirects, and expected account
email when supplied. Missing or ambiguous email cannot satisfy an expected email.
HTTP/browser readiness does not claim that a model generation succeeded.

Model discovery calls the actual ListModels RPC in the browser with credentials
captured from live page requests. Only returned model records are advertised with
source `live`; successful results have a ten-minute account-scoped cache (source
`cache`). Explicit configured_models can be returned as source `configured` when
no account is available or discovery fails. No catalog or API key is invented.
Real Google/Termux
validation is still required; local unit tests cannot certify upstream UI hooks,
cookies, regional availability or Android background survival.
