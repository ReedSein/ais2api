# 管理 API v1

管理 API 的基础地址为 `http://127.0.0.1:18451/api/admin/v1`，机器可读合同见 [management.openapi.json](../schemas/management.openapi.json)。启用 WebUI 的构建在根路径 `/` 提供的 [网页控制台](webui.md) 使用的就是这套接口；同源访问无需配置 `admin_origins`。

管理 API 操作的是真实的本地服务状态。就绪状态、账号探测和诊断信息都不能证明上游生成已经成功。

## 鉴权

每个请求（包括日志流）都要携带 `Authorization: Bearer <admin_key>`。生成用的 API Key 与托管密钥都不能访问管理 API；密钥也不接受通过 URL 查询参数传递。

### 默认密码

新创建的配置（执行 `init`，或首次启动时没有配置文件）使用默认管理密码 `admin`，并受以下限制：

- 只接受来自回环、RFC 1918 私有地址、链路本地和 IPv6 ULA 的连接；其他来源返回 `403 admin_default_password_remote`。
- 除 `GET /auth/check` 与 `POST /auth/password` 外，所有路由都返回 `403 admin_password_change_required`。`GET /auth/check` 会返回 `password_change_required`。

使用 `POST /auth/password` 提交 `{"password": "..."}` 修改密码：新密码为 8–4096 个可见 ASCII 字符，不能是 `admin`，也不能与当前值相同（否则返回 `400 admin_password_invalid`）。修改立即生效并写入 `config.json`，旧密码立刻失效；即使存在待重启的配置也一样。

其他说明：

- 已有的配置文件保留其中的 `admin_key`；也可以直接在 `config.json` 中设置。`admin_key` 为空表示关闭 HTTP 管理。
- 配置 PATCH 不能把 `admin_key` 设为 `admin`。
- 同一主机上的反向代理会让所有客户端看起来都来自本机。通过反向代理暴露服务前，务必先修改默认密码。
- 配置与诊断输出会隐藏密钥；账号信息不包含 Cookie 和浏览器存储凭据。

### 跨域访问

`admin_origins` 是允许访问的精确 HTTP(S) 来源数组，例如 `http://localhost:5173`。默认为空，只允许同源。来源不能包含路径、查询、片段、凭据或通配符。CORS 预检支持 `Authorization`、`Content-Type` 与 `Last-Event-ID`。来源检查是 Bearer 鉴权之外的附加限制。

```sh
export AIS2API_ADMIN_KEY='your-admin-key'
curl -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  http://127.0.0.1:18451/api/admin/v1/auth/check
curl -X PATCH -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  -H 'Content-Type: application/json' -d '{"limits":{"queue_size":12}}' \
  http://127.0.0.1:18451/api/admin/v1/config
```

## 路由

以下路径相对于基础地址。修改类请求的 JSON 正文必须是对象。需要使用浏览器的操作可能返回 HTTP 202 和 `operation_id`，通过 `GET /operations/{id}` 轮询完成或失败。

### 服务与认证

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/auth/check` | 验证密码，返回 `password_change_required` |
| POST | `/auth/password` | 修改管理密码，正文 `{"password": "..."}`，立即生效 |
| GET | `/version`、`/capabilities` | 版本与能力 |
| GET | `/status` | 服务状态、队列、正在生成的账号、`logging_healthy`、`pending_logs` |
| POST | `/service/stop` | 停止本地服务，返回 202 |

### 账号

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/accounts` | 不含凭据的账号摘要 |
| GET | `/accounts/active` | 首选账号与 `active_generation_account_id`（首选账号不等于正在生成的账号） |
| POST | `/accounts/import` | 导入账号，正文 `{"input": ..., "name": "可选"}` |
| POST | `/accounts/probe-import` | 先验证再保存，使用 [Cookie 导入](userscript.md) 的输入格式 |
| PATCH | `/accounts/{id}` | 修改 `name`（字符串）和/或 `disabled`（布尔） |
| DELETE | `/accounts/{id}` | 删除账号 |
| POST | `/accounts/{id}/activate` | 设为首选账号 |
| POST | `/accounts/{id}/reset` | 重置失败/禁用状态 |
| POST | `/accounts/probe`、`/accounts/{id}/probe`、`/accounts/{id}/detect-email` | 探测全部或单个账号；检测邮箱使用同一个浏览器探测 |
| GET | `/accounts/{id}/fingerprint` | 永久指纹：`seed`、`platform`、保存的 `timezone`/`locale`（为空表示跟随本机）、`effective_*` 与 `host_*` |
| PATCH | `/accounts/{id}/fingerprint` | 覆盖 `timezone`（IANA）和/或 `locale`（BCP 47）；`null` 或 `""` 恢复为本机默认；下次使用该账号时生效 |
| POST | `/accounts/{id}/fingerprint/regenerate` | 分配新种子，Google 会把该账号视为新设备 |

### 轮换与模型

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/rotation` | 轮换与首选状态 |
| POST | `/rotation/next` | 切换到下一个账号（不代表浏览器已就绪） |
| POST | `/rotation/clear-cooldown` | 清除冷却；必填 `id`，可选精确匹配的 `model` |
| GET | `/models` | 缓存的模型信息 |
| POST | `/models/refresh` | 通过浏览器重新发现模型 |

### 密钥

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/keys` | 配置密钥与托管密钥的元数据，从不返回密钥原文 |
| POST | `/keys` | 创建托管密钥，正文 `{"name": "frontend"}`；原始密钥只在此时返回一次 |
| PATCH | `/keys/{id}` | 重命名托管密钥，正文 `{"name": "new name"}` |
| DELETE | `/keys/{id}` | 撤销托管密钥 |
| PUT | `/keys/config` | 设置配置文件中的 `api_key`，正文 `{"api_key": "..."}` |
| DELETE | `/keys/config` | 清空配置文件中的 `api_key` |

托管密钥有独立的持久化记录，旧版本数据库中的密钥不会自动成为托管密钥。

`/keys/config` 的两个接口实际是一次配置修改，返回 `ConfigSaved`（`saved`、`applied`、`restart_required`）。存在待重启的配置时，新值只会写入待重启配置：`applied: false` 表示当前进程仍在使用旧密钥，重启后才切换。HTTP 200 本身不能说明已经生效。

### 配置、统计与诊断

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/config` | 当前生效配置（脱敏）、文件路径与 `restart_required` |
| PATCH | `/config` | 保存部分配置 |
| POST | `/config/validate` | 只校验部分配置，不保存 |
| POST | `/config/reload` | 重新读取配置文件与环境变量；存在需重启的差异时返回 409 |
| GET、DELETE | `/stats` | 读取或重置用量统计 |
| GET、DELETE | `/operations/{id}` | 读取异步操作结果，或处理后释放 |
| GET | `/logs/sources`、`/logs`、`/logs/stream` | 日志来源、历史与实时流，见下文 |
| GET | `/diagnostics` | 脱敏诊断快照 |
| POST | `/diagnostics/export` | 导出脱敏诊断快照 |

## 配置保存与重启

- `GET /config` 返回当前运行的脱敏配置 `config`、文件路径和 `restart_required`。有待重启的修改时，`saved_config` 包含脱敏后的待生效配置。
- PATCH 会在原子保存前校验修改后的完整配置。未知字段、类型错误、无效限额以及把脱敏占位符 `***` 当作密钥提交都会被拒绝。提交表单时请省略未修改的脱敏密钥字段。
- 只有 `limits` 按子项合并；`model_aliases`、`model_defaults`、`configured_models` 与 `admin_origins` 整体替换。要删除某个别名，提交不含它的完整对象。
- 需要重启的字段：监听地址、端口、数据目录、浏览器路径、代理、`browser_proxy_relay`、无界面模式、生成时限、流式空闲时限、`object_bytes` 与 `relay_bytes`。其余字段热更新。
- 响应包含 `saved`、`applied` 与 `restart_required`。只要包含任一需重启的差异，整份配置就作为待重启配置保存，不会部分应用；之后的 PATCH 以待重启配置为基础，保留之前保存的修改。把所有需重启的字段改回运行值后，完整结果会立即应用。重启时加载保存的配置，环境变量覆盖在启动和重新加载时依然生效。

部分字段的补充说明：

- `ignore_unsupported_parameters`：布尔，默认 `false`，立即生效。只影响生成参数，不放宽配置、鉴权或内容校验。见 [协议合同](protocol.md#optional-unsupported-parameter-dropping)。
- `search_model_drop_functions`：布尔，默认 `true`，立即生效。见 [搜索模型](protocol.md#search-models)。
- `browser_proxy_relay`：布尔，默认 `false`，需重启。开启时 `proxy` 必须是单个 `http://` 或 `socks5://` URL（凭据可选），否则 PATCH 被拒绝。浏览器诊断显示 `proxy.mode`（`none`、`direct`、`relay`）与 `proxy.relayError`（不含凭据的最近一次上游失败原因）。
- `browser_idle_seconds`：整数，`0`（默认，关闭）或 10–86400，立即生效。浏览器因空闲关闭后，诊断状态为 `idle_closed`。

## 日志与断点续读

### 日志来源

- 省略 `source` 时读取结构化的管理日志（journal）。
- `GET /logs/sources` 列出原始日志来源，包括 `service`、`management` 以及 `browser-X.log` 等浏览器日志。请使用返回的来源标识，不要自行拼接文件路径。
- 要显示全部日志：列出来源、读取各来源的完整历史、跟随活动来源，并定期刷新来源列表以发现新的浏览器会话。
- 管理日志同时镜像到 stderr，因此也可能出现在 `service` 原始来源中。前台 `serve` 总会记录服务事件；`service.log` 由后台 `start` 创建。
- 原始数据以 base64 传输，可以无损传递任意字节和被拆开的 UTF-8 序列。游标是不透明的，且只对所属来源有效，不要混用。

### 读取历史

`GET /logs` 接受 `cursor`（默认 `0`）和 `limit`（1–500，默认 100）。管理日志每批最多 256 KiB；原始来源每批最多传输 48 KiB，并返回 `next_cursor` 与 `has_more`。读取管理日志不会等待 fsync。

- 日志会一直保留到本地显式删除。`management.log` 超过 8 MiB 时轮转为 `management.1.log`（保留一份），游标在轮转后仍单调递增；后台运行时 `service.log` 超过 8 MiB 也会复制为 `service.1.log` 后截断。
- 原始来源文件变小时返回 `409 log_truncated`，请从头重新打开。

### 实时流

`GET /logs/stream` 先回放历史，追上后每 200 毫秒读取新数据：

- SSE 数据事件为 `event: logs`、`id: <next_cursor>`；空闲时发送心跳注释。
- 断线后用 `Last-Event-ID` 或显式的 `cursor` 查询参数（优先）续读。
- 轮换或撤销管理密钥会关闭现有的流。
- 独立的写入队列保证已接受的日志不丢失；待写入数达到 256 时暂停接受 HTTP 请求，直到积压消化。请关注状态中的 `logging_healthy` 与 `pending_logs`。

```sh
curl -N -H "Authorization: Bearer $AIS2API_ADMIN_KEY" \
  -H 'Last-Event-ID: 0' \
  http://127.0.0.1:18451/api/admin/v1/logs/stream
```

浏览器中请使用可携带 Authorization 头的 fetch，而不是 EventSource。下面的最小读取器能处理跨网络分块的 SSE 记录，并保留最后的游标用于重连：

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

只有成功处理一批数据后才保存游标；重连时使用有上限的退避间隔并沿用该游标。显示原始来源的文本时，把 base64 解码后的字节依次送入同一个 `TextDecoder`（`{stream: true}`），不要把每块单独解码为字符串。

## 错误

错误使用服务统一的 JSON 错误格式。常见状态码：400 输入格式错误；401 Bearer 凭据无效；403 管理已关闭、来源未授权或需要修改默认密码；404 资源不存在；409 需要重启或状态冲突；429 超出准入限制。各 `reason` 的含义见 [故障排查](troubleshooting.md)。

## 与参考项目前端的对应关系

[参考项目前端](https://github.com/ReedSein/aistudio-api-termux/tree/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/web/src/api) 的功能对应到：账号导入/列表/编辑/删除/激活、浏览器探测与邮箱检测、轮换与冷却控制、模型发现、用量统计与重置、生成密钥管理、配置编辑、服务状态、历史与实时日志以及脱敏诊断。

差异：浏览器操作是真实的异步操作，而不是模拟成功；首选账号、正在生成的账号、缓存模型、就绪状态与上游验证是相互独立的概念；配置使用 JSON 而不是 YAML；诊断导出是脱敏 JSON 快照，日志通过日志接口单独读取。Cookie 组删除和原始请求转储不是本项目账号模型中的独立功能。这些路由不提供任意文件浏览、远程 shell 或账号凭据导出。
