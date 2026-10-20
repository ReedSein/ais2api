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
history. The browser must expose the supported fetch path and snapshot adapter;
XHR or changed application internals are reported as `browser_incompatible`.
There is no invented request template or snapshot fallback.

Before each request the bridge obtains a fresh page-service snapshot of the
content hash. This anti-abuse ticket is separate from part-level thoughtSignature
metadata handled by the wire codec. Request encoding uses the live captured
template. Credentials and required request headers remain inside the page.

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
