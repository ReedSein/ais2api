# Google 请求处理遗留问题交接

日期：2026-10-23。基线：`dc21e6e` 加上工作区里尚未提交的改动（见“工作区状态”）。

本文给接手的新对话使用，可以独立阅读。来源是 ais2api 与参考项目 `.analysis/aistudio-api-termux`（提交 `84b5066`）在“向 Google 请求”方面的对照，原报告共 12 条。其中第 1、2、3、4、6、7、8、9、12 条已修复，下面列出其余未修复的问题，以及修复过程中留下的遗留项。

问题结论都来自读代码，没有用真实 Google 账号验证。下文的行号以本文日期的工作区为准，请以函数名为准定位。参考项目路径都相对于 `.analysis/aistudio-api-termux/src/aistudio_api/`。

## 已完成部分（不要重做）

| 原编号 | 内容 | 主要位置 |
|---|---|---|
| 2、3、4 | 失败分类集中到 `classify_failure()`，可能的结果有：直接失败 / 原样重试 / 就地重建会话 / 重启浏览器 / 冷却并换号。规则：403 先重建会话；401 和身份未确认先重建，再失败只冷却整个账号 300 秒（`auth` 类型），不再永久判为失效；5xx 和流中断原样重试，不冷却；换号不再重启浏览器。 | `include/ais2api/retry_policy.hpp`、`src/server.cpp` 的重试循环、`src/store.cpp` |
| 1 | 浏览器 Cookie 写回：新会话通过身份核对后写一次，之后生成成功时最多每 5 分钟写一次。只在数据库里仍是本会话用的那份凭据时才写入（比较后交换），所以不会覆盖同时进行的新导入。 | `Store::refresh_cookies`、`browser.cpp` 的 `persist_cookies` |
| 2 的遗留（原 A1） | 所有账号都在冷却时的自我恢复：`failures` 表新增 `kind`、`probed` 列。`Store::choose(model, excluded, auth_probe=true)` 在没有可用账号时，放行一个只被 `auth` 冷却挡住的账号尝试一次；同一冷却周期内每个账号最多放行一次（尝试失败只延长冷却，不再放行）。配额、权限、`login_expired`、`disabled` 不放行。生成成功会清掉该账号的 `auth` 冷却。只有生成循环传 `auth_probe`，就绪检查和模型发现不会消耗放行机会。 | `src/store.cpp` 的 `choose`/`failure`/`success`、`server.cpp` 重试循环；测试 `[auth-probe]` |
| 12（原 A2） | 第一次 204 改为新动作 `FailureAction::refresh_template`：清掉 `base_template` 后原账号重试，不占用每个请求唯一一次的恢复机会；再次 204 返回错误。页面桥接的 `bootstrap()` 开始时清掉旧的 `state.template`/`snapshotCall`，重新引导时不会拿到旧模板。引导仍在本地截住，不会发出真实生成。 | `retry_policy.hpp`、`backend.hpp`/`browser.cpp` 的 `clear_template`、`browser/bridge.js` 的 `bootstrap` |
| 8（原 A3） | 注入 Cookie 时剔除 `OSID`、`__Secure-OSID`、`SIDCC`、`__Secure-1PSIDCC`、`__Secure-3PSIDCC`（全被剔除时原样注入，同参考项目）。只在注入时剔除，存储和写回比较不变。 | `browser_detail::injected_cookies`（`browser.cpp`） |
| 6（原 B1） | 选账号顺序改为：当前账号（`settings` 里的 `current`，即最近一次生成成功的账号，也就是浏览器驻留的会话）→ 首选账号 `selected` → 该模型的 `sticky` → `rowid`。交替请求不同模型不再换号；当前账号对某模型冷却时才换，换过去的账号随即成为当前账号，**不会自动回到首选账号**（与参考项目一致）。WebUI 的“设为首选”和“轮换”会同时设置 `current`，立即生效。`rotation_status` 的 `next_account_id` 同样先看 `current`。 | `Store::choose`/`success`/`select_account`/`rotate_account`/`rotation_status`；测试 `[routing]` |
| 9（原 B2） | 页面操作容错：`snapshot()` 失败、返回空值或单次超过 9 秒时重试，最多 3 次，间隔 100 毫秒，宿主侧超时相应改为 30 秒。`bootstrap()`/`run()` 先关闭弹窗，但只按 Close、Dismiss、Got it、No thanks、Not now、Maybe later、Skip、Cancel 这类按钮或发送 Escape，绝不勾选复选框、绝不点同意/接受/继续（需要同意条款的弹窗会保持原样，由用户自己处理）。找不到 Run/Build 按钮时，`run()` 在输入框上发送 Ctrl+Enter（macOS 指纹用 Meta）并返回 `'shortcut'`，宿主在 1 秒内没捕获到模板时再用 CDP `Input.dispatchKeyEvent` 发送一次可信按键。引导仍在本地截住。 | `browser/bridge.js`、`browser.cpp` 的 `ensure_account`；`browser/test_bridge.cjs` |
| 性能 1 | 非流式回复和 Responses 流式回复的拼接从平方复杂度降为线性：`merge_event` 原地合并、文字原地追加，用序列化大小的上界代替每次完整序列化（接近上限时才精确测量，413 的判定与原来一致）。`ResponsesStream` 对“第一个候选追加一段纯文字”的增量，先在合并前确认条件，再原地扩展对应条目，不再每次重建整个回复；其他增量仍走完整路径，并保留聚合结果的回滚（`fail()` 依赖它）。实测 8000 个事件（312 KiB 文字）：`merge_event` 1390 ms → 12 ms，`ResponsesStream` 4449 ms → 20 ms。差分测试保证两条路径逐个事件完全一致。 | `src/protocol.cpp` 的 `merge_event`/`joins_text`、`src/responses.cpp` 的 `plan_append`/`apply_append`；测试 `[stream-performance]` |
| 性能 2 | 流式输出每个 SSE 事件只写一次（超过转发窗口的大事件才分块），接受连接时设置 `TCP_NODELAY`，客户端不再因 Nagle 加延迟确认多等 40–200 ms。 | `server.cpp` 的 `json_sse`、`accept_loop` |
| 性能 3 | 浏览器转发改为等待式：新的桥接方法 `next(sequence, waitMs)` 同时确认上一块并在页面里等到下一块或结束（最多 250 ms），去掉了 30 ms 轮询休眠，每块少一次往返；思考阶段从每秒约 25 次空调用降到 4 次。这次调用不带取消令牌（取消进行中的 CDP 调用会断开整个浏览器连接），取消在两次调用之间检查，最多延迟 250 ms。页面里的 base64 编码改为分块转换（约快 4.5 倍）。 | `browser/bridge.js` 的 `next`/`wake`、`browser.cpp` 的 `generate` |
| C2 的一部分 | 渲染进程崩溃立即感知：页面会话启用 `Inspector`（尽力而为），收到本会话的 `Inspector.targetCrashed`/`Inspector.detached` 时，进行中的调用立即以 `browser_disconnected` 失败，之后对该会话的调用也立即失败，交给现有的重启浏览器逻辑。磁盘缓存上限 64 MB。`data/logs/` 下每次启动产生的 `browser-*.log` 只保留最近 8 个。 | `browser/cdp.hpp`、`browser.cpp` 的 `ensure_account`/`ensure_browser`/`prune_browser_logs` |
| 性能 4–7 | **大请求内存**：`content_hash` 改为增量 SHA-256，不再拼接一份；`body` 序列化后立即释放 `wire`；启动生成的表达式和 CDP 消息都直接拼成预留好容量的文本，请求体只转义一次，不再经过 `quote_js`、JSON 树和二次序列化，峰值从约 6–7 份副本降到约 2 份。**管理日志**：排队的日志记录合并成一批，一次打开、一次设置权限、一次 `fsync`；`management.log` 超过 8 MiB 时轮转为 `management.1.log`（保留一份），新文件第一行是 `journal_rotated` 记录，带本文件起始的全局偏移 `base`，所以 `/logs` 的游标跨轮转保持单调；游标落在上一份文件里时先读完上一份，比保留的还旧时从最早保留的记录开始。Windows 上有读者正打开文件时改名会失败，稍后再试。服务在后台运行时 stderr 就是 `service.log`（每条记录都会重复写一份），进程确认这一点后，超过 8 MiB 时把它复制为 `service.1.log` 再截断，仅追加的写入句柄会接着从新的末尾写。按原始字节读取的日志源在文件变小后返回 409 `log_truncated`，WebUI 已会从头重读。**CDP 读取**：Chrome 的事件以 `{"method":"` 开头，与崩溃检测无关的事件不再解析。**大小测量**：新增 `json_size()`，按 Boost.JSON 的转义规则精确计算序列化长度而不生成文本，用于 `EventPipe::push`、`merge_event` 的上界和精确测量、`ResponsesStream`。 | `browser.cpp` 的 `generate`/`content_hash`、`browser/cdp.hpp` 的 `evaluate`/`exchange`/`append_quoted`、`src/logs.cpp`、`server.cpp` 的 `log`/`drain_logs`、`src/common.cpp` 的 `Sha256`/`json_size`；测试在 `test_logs.cpp`、`test_browser.cpp`、`test_protocol.cpp` |
| C2 的一部分（2026-10-24） | **CDP 后台读取**：`Cdp` 由专用线程持有 WebSocket，持续读取全部消息，按 id 把回复交给等待的调用；无关事件读到即丢，空闲时也不会在 Chrome 里堆积。取消或超时只放弃这一次调用（迟到的回复被丢弃），**不再断开浏览器连接**，客户端断开不会再导致下次请求冷启动浏览器；`next()` 因此改为带取消令牌。浏览器断开时等待中的调用立即失败。新增 `notify()`（只发不等）和 `call_function()`（`Runtime.callFunctionOn`，参数作为协议值传递）。**请求体分块上传**：请求体按 512 KiB 切块（不切开 UTF-8 字符，`utf8_prefix`），用 `callFunctionOn` 作为参数传给桥接的 `append(id, chunk)`，页面里每块立即转成 `Blob`（数据在浏览器进程，不在 JS 堆），`start({upload})` 把各块拼成一个 Blob 作为 `fetch` 的 body，发出后立即丢掉引用。模板请求头没有 `content-type` 时补上 `text/plain;charset=UTF-8`，与字符串 body 的默认值一致。请求体 ≥ 1 MiB 时，生成结束后发送一次 `HeapProfiler.collectGarbage`（只发不等），否则 Blob 要等页面回收那个很小的包装对象才释放。宿主侧请求体从约 2 份降到 1 份加一块。新阶段 `generation_upload`（WebUI 已加标签）。 | `browser/cdp.hpp`、`browser.cpp` 的 `upload`/`collect_garbage`/`generate`、`browser/bridge.js` 的 `append`/`takeUpload`/`start`；测试 `[cdp]`、`test_bridge.cjs` 末尾 3 个 |
| C2 的一部分（2026-10-24） | **空闲时回收浏览器**（可选，默认关闭）：顶层配置 `browser_idle_seconds`，0 关闭，可设 10–86400 秒，热更新生效；环境变量 `AIS2API_BROWSER_IDLE_SECONDS`、CLI `--browser-idle-seconds`，旧配置文件加载时自动补 0。唯一的工作线程在队列空闲到期后调用 `GenerationBackend::release_idle()`（至少每 30 秒醒来一次，以便读到重新加载的设置）；`BrowserGateway` 先写回一次 Cookie 再关闭浏览器，状态为 `idle_closed`，日志 `browser_idle_released`。回收后在有新任务前不会再次尝试。WebUI 在「配置 → 浏览器与存储」。 | `config.hpp`/`config.cpp`、`server.cpp` 的 `work_loop`/`release_idle_browser`、`browser.cpp` 的 `release_idle`、`schemas/config.schema.json`、`ConfigView.vue`；测试 `[idle-release]` |
| C1（2026-10-24） | **本机转发代理**（开关 `browser_proxy_relay`，默认关闭，需重启；`AIS2API_BROWSER_PROXY_RELAY`、`--browser-proxy-relay`）。实验确认：Chromium 遇到 `user:pass@` 会报 `ERR_NO_SUPPORTED_PROXIES`（不会绕过代理直连），但原来会把凭据写进浏览器命令行；CDP 的 `Fetch.authRequired` 只有拦截全部请求时才触发，所以没有采用。开启后 `ProxyRelay` 在 `127.0.0.1` 随机端口接受浏览器的 `CONNECT`（只允许 443，其余返回 403/405），向上游 `http://`（`Proxy-Authorization: Basic`）或 `socks5://`/`socks5h://`（RFC 1929，目标按域名发送，由上游解析）建立隧道；建立阶段 20 秒超时，最多 128 条隧道。凭据只在 ais2api 进程内，失败原因（不含凭据）记为 `last_error`。关闭时代理带凭据：启动浏览器前就报 `503 proxy_auth_unsupported`，凭据不再进入命令行。只有开启时才严格校验代理 URL（`parse_proxy`/`relay_supports`），旧配置里的其他写法（如 Chromium 代理规则）照旧直传。另外检查 `Page.navigate` 的 `errorText`：代理类错误报 `502 proxy_failed`（附转发代理的原因，直接失败），连不上 Google 报 `502 network_failed`（按 5xx 重试同一账号），都不再等满 60 秒后冷却账号。诊断新增 `proxy.mode`/`proxy.relayError`，WebUI 已显示。下载仍由 libcurl 直接使用 `proxy`。 | `include/ais2api/proxy.hpp`、`browser/proxy_relay.hpp`、`browser.cpp` 的 `ensure_browser`/`navigation_failure`、`retry_policy.hpp`、`config.cpp`；测试 `test_proxy.cpp`（`[proxy]`） |
| 7 | 每个账号有永久固定的 CloakBrowser 指纹种子，平台统一，时区和语言默认跟随本机，并加了防止 WebRTC 泄露的参数。WebUI 可以重新生成指纹、修改时区和语言。切换到指纹不同的账号时会重启浏览器。 | `include/ais2api/region.hpp`、`src/region.cpp`、`store.cpp` 的 `fingerprints` 表、`browser.cpp` 的 `ensure_browser`/`identity_for`、`webui/src/components/FingerprintDialog.vue` |

## 待修复问题

A 组（原 A1–A3）和 B 组（原 B1、B2）已在本文日期修复，见“已完成部分”。C1 已解决；C2 剩下的部分和 D 都需要用户先定方向。标“需用户决定”的条目，动手前先向用户确认方向。

### C. 需用户决定

**C1. 带用户名密码的代理在浏览器这一侧用不了（原第 10 条）——已在 2026-10-24 解决**，见“已完成部分”。

**C2. 长时间运行的资源管理（原第 11 条）——部分完成**
- **已做**（见“已完成部分”）：崩溃立即感知、磁盘缓存上限、浏览器日志清理；2026-10-24 又完成了 CDP 后台读取和请求体分块上传。
- **刻意没做**，每项都可能影响正常请求，需用户另行决定：
  - **资源屏蔽**：`Network.setBlockedURLs` 只在 `Network.enable` 后生效。原先的障碍（空闲时事件在 Chrome 里堆积）已由 CDP 后台读取解决，但仍未启用 `Network`、未屏蔽任何资源，需用户决定屏蔽范围。`--blink-settings=imagesEnabled=false` 会连 data: 图片一起禁掉，对 BotGuard 的影响未知。参考项目还屏蔽了 `recaptcha` 和编辑器脚本，这两项风险更大。
  - **JS 内存上限**（`--max-old-space-size=128`）：多份副本的问题已由分块上传解决（实测见“尚未实际验证的项”），但仍**没有加这个参数**：AI Studio 前端本身的 JS 堆有多大，需要在真实账号上用 `Runtime.getHeapUsage` 测过才能定上限，或做成配置项。
  - **空闲时回收浏览器**：已在 2026-10-24 做成默认关闭的可选项，见“已完成部分”。
  - **页面内存清理**：生成不会改动页面 DOM，桥接状态也会被替换，没有发现持续增长的东西，暂不需要。

### D. 大改动，需用户先定方向

**D1. 同一时间只能处理一个生成请求（原第 5 条）**
- **现状**：一个浏览器、一个驻留账号、一次一个请求（`README.md:126`）。排队超过 `queue_seconds` 就返回 504；探测账号和刷新模型也挤在同一个队列里。上一份对照报告（`docs/reports/upstream-request-alignment-2026-10-22.md`）刻意没有引入并发。
- **参考项目**：同一页面可以同时跑多个流式请求；切换账号时在后台新开标签页准备好，再平滑切过去。
- **新约束**：第 7 条修复后，指纹是跟着浏览器进程走的，一个进程只能有一个指纹。要做多账号并存，就需要多个浏览器进程，或者让同指纹的账号共用一个进程。至少有两种做法：同账号多请求并发，和多账号并存。需要用户先选范围。

**D2. 每个账号固定的浏览器配置目录（第 7 条中没做的部分）**
- 现在每次启动浏览器都会新建一个随机配置目录，用完删除（`browser.cpp` 的 `ensure_browser` 和 `close`）。参考项目每个账号有固定目录。
- 指纹已经固定，Cookie 也会写回，所以这一项收益不确定，但会带来磁盘占用和目录锁的问题。可选，需用户决定。

### 性能：尚未处理的项

来自 2026-10-23 的性能排查，高、中优先级项已全部完成（见上表），剩下的都是小项：
- `decode64` 每个字符线性查找字母表；`rpc_text_error` 在前 16 KiB 填满后每块仍重新解析同样的内容；WAL 下仍是默认的 `synchronous=FULL`；存储调用在 HTTP 线程上同步等待；每个请求多次复制整个配置。

## 尚未实际验证的项

- **Cookie 写回**：没有在真实已登录账号上跑过。各组成部分有单元测试；实际效果可以在诊断页的 `cookieRefresh` 字段看到（显示“已写回”）。
- **204 后同页面重新引导**：只用合成测试验证过。真实 AI Studio 页面在已引导过一次后再次点“Run”能否捕获新模板，没有在真实账号上确认；如果失败，会以 `browser_incompatible` 落到重启浏览器的逻辑。
- **B2 的弹窗关闭与 Ctrl+Enter 备用方式**：只在合成 DOM 上测试过，没有在真实 AI Studio 页面上确认弹窗的按钮文字和快捷键是否生效。
- **性能 3 和崩溃感知**：已在内置 Chromium 146 上做过无账号验证（本地伪造 AI Studio 页面和 fetch，代理指向不通的端口）：`Inspector.enable` 可用；`next()` 3 次调用传完 70000 字节、用时 14 ms；流卡住时 250 ms 后返回，`abort()` 约 60 ms 唤醒；`Page.crash` 触发的 `Inspector.targetCrashed` 确实发到页面会话上。没有在真实生成中验证。
- **性能 4–7**：只有单元测试（本地回环 websocket 对端、真实文件和句柄）。没有在真实 Chromium 上重新跑生成请求；新的 `Runtime.evaluate` 消息是普通 JSON，测试确认其转义与 `json::serialize` 逐字节一致。日志写入、轮转和 `service.log` 截断的 POSIX 分支没有在 Linux/macOS/Termux 上编译运行过，需要 CI 确认。
- **WebRTC 防泄露**：本机连普通 Edge 都收集不到 ICE 候选地址，所以只确认了这个参数不改变指纹，没有证明它确实拦住了泄露。
- **CDP 后台读取与分块上传**：已在内置 Chromium 146 上做过无账号验证（本地 HTTP 页面，代理指向不通的端口），用与 `upload()` 相同的协议调用发送 30 MiB 含中文和 emoji 的请求体。原方式（整段字符串）：JS 堆 84.7 MiB，渲染进程私有内存 175 MB；新方式：JS 堆 14 MiB（都是待回收的块字符串），渲染进程 48 MB，Blob 约 37 MB 在浏览器进程，`HeapProfiler.collectGarbage` 后释放（不需要先 enable）。两种方式上游收到的字节、`Content-Length`、`Content-Type` 完全一致，没有变成分块传输编码。`--max-old-space-size=64` 时原方式渲染进程崩溃，新方式正常完成。代价：30 MiB 上传约 2 s（512 KiB 一块，61 块；块越大越快，但待回收的堆越大），普通文本请求只有 1–2 块。没有在真实 AI Studio 页面和真实生成中验证。
- 涉及真实 Google 账号或真实生成的验证，必须先得到用户同意。

## 工作区状态

- 所有改动都没有提交。工作区里还有其他人未提交的改动，例如 `src/store.cpp`、`tests/test_generation.cpp` 中原有的部分，以及 `include/ais2api/media.hpp`、`tests/test_media.cpp`、`tools/stream_timing_probe.py`。只做新增，不要回退或重写别人的改动。
- 除非用户要求，不要提交。

## 构建与测试

Windows 构建（在 VS 开发者环境中，先执行 `vcvars64.bat`）：

```text
cmake --preset windows -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=D:/Codex/ais2api/vcpkg_installed
cmake --build build/windows
ctest --test-dir build/windows
```

- **cmake**：PATH 里没有 cmake 时，使用 `.tools/downloads/tools/cmake-4.4.3-windows/` 下的 `cmake.exe`。
- **WebUI 嵌入**：构建时默认嵌入 WebUI（`AIS2API_BUILD_WEBUI=ON`），会执行 `npm ci`、类型检查和 vite 构建。
- **测试基线**（本文日期，全部通过）：
  - Catch2：`build/windows/ais2api_tests.exe`，271 个用例、4593 个断言。可以按标签过滤，例如 `[fingerprint]`、`[cookie-refresh]`、`[admin]`。
  - CLI 冒烟：`python tests/cli_smoke.py --exe build/windows/ais2api.exe`，36 项，不联网，不需要浏览器。
  - WebUI：在 `webui/` 下执行 `npm run typecheck && npm test`，26 个测试。
  - 页面桥接：`node --test browser/test_bridge.cjs`，46 个测试。
- **无账号浏览器验证**：
  - 使用内置浏览器 `build/browser-bundle-windows/chrome.exe`，在 `.tools/` 下建隔离实例，用 `--config/--data-dir/--port` 指定配置、数据目录和端口。
  - 把 `--proxy` 指向一个不通的本地端口，例如 `http://127.0.0.1:9`，确保请求不会到达 Google。
  - 测试 Cookie 用明显的合成值；`__Secure-` 开头的 Cookie 必须带 `"secure":true`，否则 `Storage.setCookies` 会拒绝。
  - 用完后停止实例并删除目录。
