# Management API v1

The management base URL is `http://127.0.0.1:18451/api/admin/v1`. The machine-readable contract is [management.openapi.json](../schemas/management.openapi.json). Management controls real local service state; readiness, account probes and diagnostics do not prove that upstream generation succeeds.

An optional [embedded Web console](webui.md) uses these same endpoints from the service root `/`; same-origin access needs no `admin_origins` entry.

## Authentication and browser access

Newly created configurations (`init`, or a first start without a configuration file) use the default management password `admin`. While it is active, the password is accepted only from loopback, RFC 1918 private, link-local and IPv6 unique-local peers (other peers receive `403 admin_default_password_remote`), and every route except `GET /auth/check` and `POST /auth/password` returns `403 admin_password_change_required`. `GET /auth/check` reports `password_change_required`. `POST /auth/password` with `{ "password": "..." }` replaces it: 8–4096 visible ASCII characters, not `admin` and not the current value (`400 admin_password_invalid` otherwise). The change applies immediately, also while restart-only configuration changes are pending, and is saved to config.json; the old password stops working at once. Configuration patches cannot set `admin_key` to `admin`. Existing configuration files keep their stored `admin_key`; you can also set an independent `admin_key` in config.json directly. Empty disables HTTP management. A reverse proxy on the same host makes every client appear local, so replace the default password before exposing the service through one. Send `Authorization: Bearer <admin_key>` on every request, including log streams. Generation API keys and managed generation keys do not grant management access. Secrets are never accepted through a URL query parameter. Configuration and diagnostics redact secrets; account responses omit Cookie and browser storage credentials.

`admin_origins` is an array of exact HTTP(S) origins, such as `http://localhost:5173`. Its default empty value permits only same-origin browser access. Origins cannot contain a path, query, fragment, credentials or wildcard. Cross-origin frontends must explicitly list their origin; CORS preflight supports Authorization, Content-Type and Last-Event-ID. Origin checks supplement Bearer authentication.

```sh
export AIS2API_ADMIN_KEY='your-admin-key'
curl -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  http://127.0.0.1:18451/api/admin/v1/auth/check
curl -X PATCH -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  -H 'Content-Type: application/json' -d '{"limits":{"queue_size":12}}' \
  http://127.0.0.1:18451/api/admin/v1/config
```

## Routes

Paths below are relative to the management base URL. JSON mutation bodies must be objects. Operations that use the browser can return HTTP 202 with an `operation_id`; poll `GET /operations/{id}` for completion or failure.

| Method | Path | Purpose/body |
| --- | --- | --- |
| GET | `/auth/check`, `/version`, `/capabilities` | Authentication (`password_change_required`) and version/capability discovery |
| POST | `/auth/password` | Replace the management password with `{ "password": "..." }`; applies immediately |
| GET | `/status` | Service state, queue, active generation account, `logging_healthy`, `pending_logs` |
| GET, DELETE | `/stats` | Read or reset usage statistics |
| GET | `/accounts` | Credential-free account summaries |
| GET | `/accounts/active` | Preferred account and `active_generation_account_id`; preference is distinct from an in-flight generation |
| POST | `/accounts/import` | Import account input using `{ "input": ..., "name": "optional" }` |
| POST | `/accounts/probe-import` | Verify imported credentials before storing; use the documented Cookie import input format |
| PATCH | `/accounts/{id}` | Update `name` (string), `disabled` (boolean), or both |
| DELETE | `/accounts/{id}` | Remove an account |
| POST | `/accounts/{id}/activate` | Select the preferred account |
| GET | `/accounts/{id}/fingerprint` | Permanent browser fingerprint: `seed`, `platform`, stored `timezone`/`locale` (empty follows the host), `effective_*` and `host_*` values |
| PATCH | `/accounts/{id}/fingerprint` | Override `timezone` (IANA) and/or `locale` (BCP 47); `null` or `""` restores the host default. Applies the next time the account is used |
| POST | `/accounts/{id}/fingerprint/regenerate` | Assign a new seed; Google will see a new device for this account |
| POST | `/accounts/{id}/reset` | Reset account failure/disabled state |
| POST | `/accounts/probe`, `/accounts/{id}/probe`, `/accounts/{id}/detect-email` | Probe all accounts or one account; email detection uses the same browser probe |
| GET | `/rotation` | Rotation and preference state |
| POST | `/rotation/next` | Advance preference; does not imply the browser is prepared |
| POST | `/rotation/clear-cooldown` | Clear cooldown; required `id` and optional exact `model` filter |
| GET | `/models` | Cached model information |
| POST | `/models/refresh` | Refresh through browser maintenance |
| GET | `/keys` | List configured and managed generation key metadata; never secrets |
| POST | `/keys` | Create a managed generation key with `{ "name": "frontend" }`; retain the returned secret immediately |
| PATCH | `/keys/{id}` | Rename a managed key with `{ "name": "new name" }` |
| DELETE | `/keys/{id}` | Revoke a managed generation key |
| PUT | `/keys/config` | Set the single config generation key with `{ "api_key": "..." }` |
| DELETE | `/keys/config` | Clear the config generation key |
| GET, PATCH | `/config` | Show or persist a partial configuration patch |
| POST | `/config/validate` | Validate a partial patch without saving |
| POST | `/config/reload` | Reload file/environment configuration; restart-only differences return 409 |
| GET, DELETE | `/operations/{id}` | Read a maintenance result, or release it after handling |
| GET | `/logs/sources`, `/logs`, `/logs/stream` | Discover sources, read history, stream history plus live updates |
| GET | `/diagnostics` | Redacted diagnostic snapshot |
| POST | `/diagnostics/export` | Return a redacted diagnostic export snapshot |
| POST | `/service/stop` | Stop this local service; returns 202 |

Managed generation keys have independent persisted records. Existing legacy database keys do not automatically become valid managed keys. The local CLI config-key policy remains distinct from HTTP managed-key CRUD.

## Configuration save and restart behavior

`ignore_unsupported_parameters` is a boolean, defaults to false and applies immediately when patched. It controls dropping known unsupported optional generation parameters across Gemini, Chat and Responses; it does not relax configuration, authentication or content validation. See [the compatibility switch](protocol.md#optional-unsupported-parameter-dropping).

`search_model_drop_functions` is a boolean, defaults to true and applies immediately when patched. It selects whether `*-search` models drop custom function declarations to keep Google Search (true) or keep the caller's tools without adding search (false); see [search models](protocol.md#search-models).

`browser_proxy_relay` is a boolean, default false, and requires a restart. When true, `proxy` must be a single `http://` or `socks5://` URL (credentials optional); otherwise the patch is rejected. Browser diagnostics report `proxy.mode` (`none`, `direct` or `relay`) and `proxy.relayError`, the relay's latest upstream failure without credentials.

`browser_idle_seconds` is an integer, 0 (default, off) or 10..86400, and applies immediately when patched. When set, the browser stops after that many seconds without work and the next request starts it again; browser diagnostics then report the status `idle_closed`.

`GET /config` returns the redacted running `config`, its file path and `restart_required`. When a saved change awaits restart, `saved_config` contains the redacted pending configuration. PATCH validates the full resulting configuration before atomically saving it. Unknown fields, wrong types, invalid limits and the display mask `***` as a secret are rejected. Omit unchanged redacted secret fields from a submitted form.

Patches merge only the nested `limits` object. `model_aliases`, `model_defaults`, `configured_models` and `admin_origins` replace their respective complete containers. To delete a model alias, submit a replacement map without it.

Bind address, port, data directory, browser executable, proxy, browser proxy relay, headless mode, generation timeout, stream idle timeout, object bytes and relay bytes require restart. Other settings retain the existing hot reload policy, including request bytes and queue limits. The response reports `saved`, `applied` and `restart_required`. A patch with any restart-only difference is saved as a complete pending configuration and is not partially applied. Later patches use that pending configuration as their base, preserving earlier saved changes. Returning all restart-only settings to their running values allows the complete result to apply immediately. Restart loads the saved configuration; environment overrides still apply during startup/reload.

## Logs and resumable streaming

Omit `source` to read the structured management journal. `GET /logs/sources` discovers raw sources, including `service`, `management` and browser log filenames such as `browser-X.log`. Use the returned source identifiers rather than constructing filesystem paths. To show all logs, enumerate sources, read their complete history, and follow the active sources; refresh the inventory to discover new browser sessions. Journal entries are also mirrored to stderr and can therefore appear again in the raw service source. Foreground `serve` always journals server events, while `service.log` is created by background `start`.

Raw chunks are base64 so arbitrary bytes and split UTF-8 sequences can be transferred without loss. Cursors are opaque and source-specific; never reuse one for another source.

History accepts `cursor` (default `0`) and `limit` (1–500, default 100). A journal batch is bounded to 256 KiB; raw sources transfer up to 48 KiB of bytes per batch and includes `next_cursor` and `has_more`. Reading the journal does not wait for a filesystem fsync. Logs remain available until explicit local deletion. If a raw source is truncated, handle the returned error and reopen from its beginning.

`GET /logs/stream` first replays history, then reads new data every 200 ms when caught up. SSE data events use `event: logs` and `id: <next_cursor>`; idle connections receive heartbeat comments. Reconnect with `Last-Event-ID`, or an explicit `cursor` query parameter (which takes precedence). Rotate or revoke the admin key and existing streams close. A dedicated writer queue preserves admitted logs; when 256 writes are pending HTTP admission pauses until the backlog drains. Monitor `logging_healthy` and `pending_logs` in status.

```sh
curl -N -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  -H 'Last-Event-ID: 0' \
  http://127.0.0.1:18451/api/admin/v1/logs/stream
```

Use authenticated fetch rather than EventSource, which cannot set an Authorization header. This minimal browser reader handles SSE records split across network chunks and preserves the last cursor for reconnection:

```js
async function readLogs(base, adminKey, cursor = "0", onBatch = console.log) {
  const response = await fetch(`${base}/logs/stream`, {
    headers: { Authorization: `Bearer ${adminKey}`, "Last-Event-ID": cursor },
  });
  if (!response.ok) throw new Error(`Logs HTTP ${response.status}`);
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffered = "";
  try {
    for (;;) {
      const { value, done } = await reader.read();
      buffered += done ? decoder.decode() : decoder.decode(value, { stream: true });
      let end;
      while ((end = buffered.indexOf("\n\n")) >= 0) {
        const frame = buffered.slice(0, end); buffered = buffered.slice(end + 2);
        let id, event = "message"; const data = [];
        for (const line of frame.split("\n")) {
          if (line.startsWith("id: ")) id = line.slice(4);
          if (line.startsWith("event: ")) event = line.slice(7);
          if (line.startsWith("data: ")) data.push(line.slice(6));
        }
        if (event === "logs" && data.length) {
          await onBatch(JSON.parse(data.join("\n")));
          if (id !== undefined) cursor = id;
        }
      }
      if (done) return cursor;
    }
  } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
}
```

Persist the cursor only after successfully handling a batch. Reconnect with a bounded delay, retaining that cursor. For a raw source, decode base64 to bytes and feed consecutive chunks to a separate persistent `TextDecoder` with `{stream:true}` if displaying text; do not decode each chunk as an independent string.

## Frontend feature mapping

The [reference frontend](https://github.com/ReedSein/aistudio-api-termux/tree/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/web/src/api) features map to account import/list/edit/delete/activation, browser probes and email detection, rotation and cooldown control, model discovery, usage statistics/reset, generation key CRUD, configuration editing, service status, historical/live logs and redacted diagnostics. Browser actions are asynchronous operations rather than simulated success. The API reports local state honestly: preferred accounts, active generation accounts, cached models, readiness and upstream verification are separate concepts. Configuration uses JSON instead of upstream YAML; diagnostics export is a redacted JSON snapshot and logs are read separately through the complete log APIs. Cookie-group deletion and raw request dumps are not separate features of this account/storage model. This service does not expose an arbitrary filesystem browser, remote shell or account credential export through these routes.

Errors use the service JSON error envelope. Typical statuses are 400 for malformed input, 401 for invalid Bearer credentials, 403 for disabled management or an unapproved Origin, 404 for an unknown resource, 409 for restart/state conflicts, and 429 for admission limits. The OpenAPI document describes routes, request parameters and core response shapes for frontend integration.
