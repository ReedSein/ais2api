# 浏览器 Cookie 导入

启动 ais2api 后，在 Tampermonkey 打开 [本机安装入口](http://127.0.0.1:18451/userscript/ais2api-cookie.user.js)。如果服务地址不同，使用该服务的 `/userscript/ais2api-cookie.user.js`。再打开 AI Studio，面板或管理器菜单可启动导入。

服务端设置 `cookie_import_key`，在面板中输入同一值；它独立于生成接口的 `api_key`。空密钥不能推送。默认服务为 `http://127.0.0.1:18451`。面板接受 HTTPS origin；HTTP 仅接受 localhost、127.0.0.1、[::1]，不允许路径、查询、片段或 URL 用户信息。远程服务应使用 HTTPS。`@connect *` 用于用户选择服务器，管理器仍可能要求连接授权。

在已有 `config.json` 中设置 `cookie_import_key` 后，运行 `ais2api config reload`；也可重启服务。旧配置缺少这些字段时，离线运行 `ais2api init` 会补充空导入密钥及默认限额，不生成或覆盖密钥。脚本可从程序的 `userscript/` 目录手动安装；只有该静态脚本文件通过 HTTP 提供，没有 Web 管理页面。

点击“读取并导入当前账号”才会读取当前 AI Studio URL 的 Cookie 并发送请求。不会读取页面正文或其他网站 Cookie，不保存 Cookie、密钥或期望邮箱，也不输出敏感日志。仅保存服务地址与非敏感账号索引。索引默认从 URL 的 `authuser` 或 `/u/N` 推断，否则为 0；可修改。填写期望邮箱可防止导错身份。管理器元数据转换为标准 storage_state 字段：有效期使用 `expires`，SameSite 使用 `Strict`、`Lax`、`None`；未指定或未知 SameSite 省略。会话 Cookie 省略有效期，hostOnly Cookie 的域去掉前导点，不发送管理器私有字段。

脚本优先使用 GM_cookie.list，4 秒未完成、失败或不可用则回退到 document.cookie。GM 返回的同名不同域/路径 Cookie、空值、Secure、HttpOnly、SameSite 和有效期会保留；过期及分区 Cookie（包括 partitionKeyOpaque）被排除，面板显示计数。GM 成功时不混入 document.cookie，防止重新加入已排除的分区 Cookie。回退使用原版兼容的原始 Cookie 头字符串，保留空值，不伪造结构化域或其他元数据；域、路径、有效期及安全属性未知，无法判断分区，且不包含 HttpOnly，面板明确标注这些限制，不能保证完整认证。

[Tampermonkey GM_cookie 文档](https://www.tampermonkey.net/documentation.php?q=GM_cookie)指出 HttpOnly 支持目前受 beta 版本及 URL 权限限制。安装此脚本不保证取得全部认证 Cookie；其他管理器能力可能不同。若导入失败，应检查管理器权限及支持情况，不能将缺少 HttpOnly 的回退视为完整导出。

请求使用 `/api/accounts/probe-import`、Bearer 导入密钥、匿名跨域请求并禁止重定向，最长等待 300 秒。请求包含完整 Cookie 记录数组和单个 `auth_user`，可附 `email`。成功仅表示 `verification: browser_identity_only`，`generation_verified: false`；不代表生成请求已通过验收。

失败时只将已知服务端错误原因映射为固定中文提示，例如区分导入密钥错误与 Cookie 未登录或过期；不会显示服务端错误正文、message 或未知原因值。

原版脚本也可继续使用：服务地址填 `http://127.0.0.1:18451`，“管理密码”填 `cookie_import_key`。后端兼容 `{cookies: "Cookie header", name_prefix?: "..."}`；没有 `auth_user` 时依次探测 0 到 7，受 `cookie_import_max_accounts`（1–32）及 `cookie_import_timeout_seconds`（默认 180、最大 240 秒）限制。

已按原版 v0.1.2 / [v0.1.3 固定版本](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/userscript/aistudio-cookie.user.js)的推送合同核对：它们发送合并后的 Cookie 头，不携带元数据。后端对该兼容路径恢复 SID、APISID、SAPISID、__Secure-1PAPISID、__Secure-3PAPISID 的 JavaScript 可读属性，供 Google 页面构造 RPC 鉴权；结构化 Cookie 的显式 HttpOnly 属性保持原样。不能把所有导入 Cookie 都设成 HttpOnly。

导入只等待 AI Studio 的已登录唯一身份，不再要求出现 textarea；编辑器和生成能力仍在生成流程中检查。身份识别优先采用可见 Google 账号控件，排除隐藏菜单；存在多个可见身份时仍拒绝导入。浏览器未配置、启动失败、断连、超时和页面不兼容分别返回固定错误原因，服务日志只记录原因、请求 ID 与耗时，不记录 Cookie 或页面内容。

兼容范围为原版的“抓取并推送”；原版诊断包和其他管理接口未接入。Cookie 头不携带域与路径，无法恢复未知 `__Host-` Cookie 的来源，这些项会跳过并报告计数；需要精确元数据时使用专用脚本。当前不支持分区 Cookie，不会将它们剥离分区属性后当普通 Cookie 保存。

HTTP 输入上限 1 MiB、Cookie 数量上限 512。验证进入与生成共用的有界工作队列，一个账号使用独立 BrowserContext；请求断开、超时或服务关闭会取消验证。保存采用一次事务，只返回账号元数据，按已验证邮箱更新现有账号并保留 ID、名称及统计。导入密钥在保存前再次检查，已撤销的请求不落库。响应的 `details.truncated` 表示探测达到了账号上限，不能将它当作所有账号都已发现。

开发验证：`node userscript/test_cookie_helper.cjs`。测试在隔离 VM 中运行正常脚本、模拟 DOM 与 GM API，不使用真实 Cookie。

可选真实浏览器离线回归：设置 `AIS2API_TEST_BROWSER` 为 CloakBrowser 可执行文件后执行 `node browser/test_cookie_runtime.cjs`。该测试使用临时资料目录、合成 Cookie 和本地拦截页面，阻止外网请求，验证 Cookie 可读属性及无编辑器身份识别；C++ 头部转换由单元测试验证，两者都不能替代真实账号验收。
