# Command-line interface

The executable is `ais2api` (`ais2api.exe` on Windows). Configuration, credentials and service control are managed locally through the CLI. There is no HTTP administration API or Web frontend.

`--help` prints human-readable usage. Other successful commands print one JSON object to stdout and exit with code 0. Failures print a structured JSON error to stderr and exit with code 1. API-key secrets appear only in the successful `init` or `keys create` result that creates them; keep that output private. `keys list` returns metadata, never the secret or its hash.

## Initialization and service lifecycle

```text
ais2api init --config ./config.json --data-dir ./data
ais2api setup --config ./config.json --data-dir ./data
ais2api start --config ./config.json --data-dir ./data
ais2api status --config ./config.json --data-dir ./data
ais2api stop --config ./config.json --data-dir ./data
```

`init` writes the first configuration and initializes local storage. The first initialization creates an API key and prints its secret once. Re-running initialization does not issue another initial secret, including after keys are revoked. Create a new key explicitly when needed.

`serve` runs in the foreground; `start` launches a background process and waits up to five seconds for local control readiness. The service log is `data_dir/service.log`. `status` returning `running: true` means the process and local control are reachable. Readiness describes local prerequisites, not a successful Google generation. `stop` requests orderly shutdown; callers can poll `status` until `running: false`.

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
ais2api keys create "Client name"
ais2api keys list
ais2api keys revoke KEY_ID
```

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

`config show` redacts proxy userinfo and recognized credential fields while retaining nonsecret settings such as limits. `config validate` checks configuration locally. `config reload` requires a running service and applies reloadable settings, including queue capacity; listener, storage, browser and transport changes can require a restart and produce `restart_required`. `stats` reports recorded counters.

`models list` requests live discovery when the service is running and can prepare a browser. Without a running service it can report explicitly configured models; it cannot claim live discovery. Discovery and account probes run as queued operations so local status/stop control stays responsive. The CLI polls their operation IDs every 200 ms for up to 120 seconds. If the operation remains pending, it exits with `operation_pending`; use the reported ID with `operations status`. Completion returns the result; failure returns a structured error. Operations are retained temporarily and IDs can expire.

## Local control and HTTP authentication

The running process owns its database and accepts owner-only local IPC: a protected Windows named pipe or a protected Unix-domain socket. CLI commands first contact that process. Only `not_running` permits supported commands to open storage offline, after acquiring the exclusive instance lock. Timeouts or authorization failures do not trigger an offline database bypass.

HTTP remains the inference/model API. Clients authenticate with an API key in `Authorization: Bearer KEY`, `x-api-key`, or `x-goog-api-key`. `/health` is a public process-liveness check. `/ready` reports local prerequisites and explicitly does not verify Google. Cookie import, key mutation, configuration and browser control remain local CLI operations.

## Developer smoke check

`python tests/cli_smoke.py --exe build/windows/ais2api.exe` runs isolated CLI/IPC/HTTP checks using only Python's standard library. Python is not a production runtime dependency. Every fixture lives under the workspace `.tools/smoke-*` subtree, with explicit config and data paths. The script never prints API secrets, never performs browser preparation and never contacts Google. Its authentication probes deliberately use an invalid Content-Type and stop before generation is queued. It stops the service, confirms offline ownership, removes the owned fixture directory, and reports concise JSON check names.
