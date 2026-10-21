# Command-line interface

The executable is `ais2api` (`ais2api.exe` on Windows). Configuration and service control are managed locally through the CLI. There is no Web frontend. The optional, separately authenticated userscript Cookie import endpoint is the only HTTP account mutation; see [userscript setup](userscript.md).

`--help` prints human-readable usage. Other successful commands print one JSON object to stdout and exit with code 0. Failures print a structured JSON error to stderr and exit with code 1. API keys are configured in `config.json`; CLI output redacts them. Keep the configuration file private. `keys list` returns metadata, never the secret or its hash.

## Initialization and service lifecycle

```text
ais2api init --config ./config.json --data-dir ./data
ais2api setup --config ./config.json --data-dir ./data
ais2api start --config ./config.json --data-dir ./data
ais2api status --config ./config.json --data-dir ./data
ais2api stop --config ./config.json --data-dir ./data
```

`init` writes the first configuration and initializes local storage. It never generates or prints an API key. Its result includes `api_key_configured` and `next_step`. Set `api_key` in `config.json` to your own key before starting; the default is empty. Re-running initialization preserves it. Keys allow at most 4096 visible ASCII characters without whitespace.

The default listener is `127.0.0.1:18451`. Windows port reservations can change dynamically; use configuration `port` or `--port PORT` to override it.

Defaults are portable: `config.json` and `data/` live beside the executable.
Downloads are `data/runtime/downloads/`, logs are `data/service.log`, and temporary
browser profiles are `data/runtime/browser-*/`. No AppData/HOME/XDG fallback is
used if this directory is not writable. Use a writable program folder or explicit
path overrides. Persisted paths inside the configuration directory are relative,
so the folder can move as a unit. File-relative paths resolve from the config
directory; explicit CLI/environment relative paths resolve from the shell.
Existing external absolute paths are retained; old state is not silently migrated.
`doctor` reports the effective program, config, download, data and log paths.

Local browser packages auto-detect `browser-runtime/PLATFORM/chrome[.exe]` beside
the executable. `init`/`start` can use this browser without downloading it;
`setup` verifies its full inventory and still prepares OS packages where needed.

`serve` runs in the foreground; `start` launches a background process and waits up to five seconds for local control readiness. The service log is `data_dir/service.log`. `status` returning `running: true` means the process and local control are reachable. Readiness describes local prerequisites, not a successful Google generation. `stop` requests orderly shutdown; callers can poll `status` until `running: false`.

If `start` times out, inspect `data/service.log`. HTTP listener errors include the
operation, address, port and numeric OS error. On Windows, `os_error=10013` can
mean the port falls in a system reservation; inspect with
`netsh interface ipv4 show excludedportrange protocol=tcp`. Choose an available
`port` in `config.json` or pass `--port PORT`, then restart. The service does not
silently change its listening port. A listener failure happens before browser
login and is independent of imported Cookie contents.

`setup` prepares supported platform/browser dependencies; `--yes` authorizes required system package changes. Browser installation requires network access. `doctor` inspects the environment. `browser update` and `browser rollback` manage browser versions. Stop the service before setup, update or rollback; these commands take the same exclusive data-directory ownership lock as the service.

## Global options and precedence

Options may precede or follow the command. Use a value after the option or `--option=value`:

```text
--config PATH          --data-dir PATH       --bind ADDRESS
--port PORT            --browser PATH        --proxy URL
--headless=true|false
--request-bytes N      --object-bytes N      --relay-bytes N
--queue-size N         --queue-seconds N     --generation-seconds N
--stream-idle-seconds N                     --max-attempts N
```

The effective configuration combines defaults, the configuration file, `AIS2API_*` environment overrides, then explicit CLI options. `start` preserves its effective settings in the child process without adding proxy credentials to the child command line. Keep using the same `--data-dir` for all commands targeting that service. Paths with spaces should be quoted by the shell.

## Accounts and keys

```text
ais2api accounts import --file ./accounts.json --name "Imported account"
ais2api accounts import --stdin --name "Imported account"
ais2api accounts list
ais2api accounts select ACCOUNT_ID
ais2api accounts remove ACCOUNT_ID
ais2api accounts reset ACCOUNT_ID
ais2api accounts probe ACCOUNT_ID
ais2api accounts probe all
ais2api keys list
```

`keys list` returns `source: "config"`, `configured`, and metadata in `keys` (empty when unconfigured). `keys create` and `keys revoke` fail with `reason: "keys_managed_in_config"`; edit `api_key` then reload or restart instead. Existing database key records remain intact but no longer authorize HTTP.

Append the appropriate global configuration/data-directory options to these examples. Import reads stdin only with explicit `--stdin`; it never consumes stdin implicitly. Inputs may be supported JSON cookie/storage-state objects, account bundles, or bare Cookie-header text. Avoid putting cookies in command arguments or shell history. Import/list results exclude cookie values and storage origins. Account metadata such as names and emails remains visible to the local owner.

`reset` clears a recorded account failure/disabled state; it does not repair an expired Google cookie. `probe` deliberately contacts the upstream using the selected credentials and requires a running service. It can prepare a browser. Do not use it as an offline health check.

## Configuration, models and operations

```text
ais2api config show
ais2api config validate
ais2api config reload
ais2api stats
ais2api models list
ais2api operations status OPERATION_ID
```

`config show` redacts `api_key`, proxy userinfo and recognized credential fields while retaining nonsecret settings such as limits. `config validate` checks configuration locally. `config reload` requires a running service and applies reloadable settings, including queue capacity and `api_key`; changing the key immediately rejects the old key and clearing it disables protected HTTP access; listener, storage, browser and transport changes can require a restart and produce `restart_required`. `stats` reports recorded counters.

`models list` requests live discovery when the service is running and can prepare a browser. Without a running service it can report explicitly configured models; it cannot claim live discovery. Discovery and account probes run as queued operations so local status/stop control stays responsive. The CLI polls their operation IDs every 200 ms for up to 120 seconds. If the operation remains pending, it exits with `operation_pending`; use the reported ID with `operations status`. Completion returns the result; failure returns a structured error. Operations are retained temporarily and IDs can expire.

## Local control and HTTP authentication

The running process owns its database and accepts owner-only local IPC: a protected Windows named pipe or a protected Unix-domain socket. CLI commands first contact that process. Only `not_running` permits supported commands to open storage offline, after acquiring the exclusive instance lock. Timeouts or authorization failures do not trigger an offline database bypass.

Inference/model clients authenticate with an API key in `Authorization: Bearer KEY`, `x-api-key`, or `x-goog-api-key`. `/health` is a public process-liveness check. With an empty `api_key`, protected generation/model routes reject authentication and `/ready` returns `ready: false`. The optional `POST /api/accounts/probe-import` requires its own Bearer `cookie_import_key`; a generation key alone does not authorize account import. `/ready` reports local prerequisites and explicitly does not verify Google. Configuration and browser control remain local operations. Edit both keys in the configuration file.

## Developer smoke check

`python tests/cli_smoke.py --exe build/windows/ais2api.exe` runs isolated CLI/IPC/HTTP checks using only Python's standard library. Python is not a production runtime dependency. Every fixture lives under the workspace `.tools/smoke-*` subtree, with explicit config and data paths. The script never prints API secrets, never performs browser preparation and never contacts Google. Its authentication probes deliberately use an invalid Content-Type and stop before generation is queued. It stops the service, confirms offline ownership, removes the owned fixture directory, and reports concise JSON check names.
