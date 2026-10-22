# Native browser gateway

The C++ process launches the configured Chromium/CloakBrowser executable directly.
On Termux that executable is the setup-generated launcher into the project's
proot environment. There is no Python, Node, Playwright or external CDP service.

The gateway uses a private random profile and reads only its DevToolsActivePort.
It connects Boost.Beast to the loopback browser websocket, creates one incognito
BrowserContext, installs storage-state cookies, creates a target in that context
and attaches using flattened CDP sessions. Switching accounts disposes the old
context first. Cookies and page snapshots are never written to diagnostic logs.
Windows descendants belong to a kill-on-close Job Object. POSIX launches own a
new process group; shutdown targets only that group. Runtime profiles are removed
after shutdown. Imported credentials remain in the Store and are not erased.

The bridge is original JavaScript in `browser/bridge.js`. CMake must copy it beside
the executable under `browser/`, or define AIS2API_ASSET_DIR. It discovers a live
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

Recognized ambiguous GenerateContent routing 404s retry the same account within
max_attempts, rebuilding the body and snapshot in its verified context. One empty
204 on the first attempt may retry once; ordinary 404s do not rotate accounts.
Responses require a non-unspecified finish reason as well as complete transport
framing. Delivered content is never regenerated after failure. Generation quota
cooldowns are model-scoped: minute limits wait at least sixty seconds without
increasing daily quota counters; other 429s use 60/60/300/300/600 seconds, reset
their counters at Pacific midnight and preserve drip counters after success.
Retry-After can extend the delay up to one day. Known-tier permission failures
temporarily cool the affected model for five minutes. Credential replacement
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
