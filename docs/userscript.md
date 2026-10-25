# Cookie 导入（userscript）

ais2api 提供专用的 Tampermonkey 脚本，可以在已登录的 AI Studio 页面一键读取 Cookie 并推送到服务；原项目（aistudio-api-termux）的脚本也可以继续使用。导入只验证浏览器中的登录身份，**不代表生成能力已经可用**。

## 快速使用

1. 在 `config.json` 设置 `cookie_import_key`（独立于 `api_key` 的密钥），执行 `ais2api config reload` 或重启服务。为空时不能推送。
2. 在 Tampermonkey 中打开 `http://127.0.0.1:18451/userscript/ais2api-cookie.user.js` 安装脚本（服务地址不同时使用对应地址）。也可以从程序目录 `bin/userscript/` 手动安装。
3. 打开 AI Studio，通过面板或管理器菜单启动导入，填写服务地址和导入密钥。
4. 点击“读取并导入当前账号”。

旧配置缺少导入相关字段时，离线执行 `ais2api init` 会补上空的导入密钥和默认限额，不会生成或覆盖密钥。

## 面板设置

- **服务地址**：默认 `http://127.0.0.1:18451`。接受 HTTPS 来源；HTTP 只接受 `localhost`、`127.0.0.1`、`[::1]`。不允许路径、查询、片段或 URL 用户信息。远程服务请使用 HTTPS。脚本声明了 `@connect *` 以便用户选择服务器，管理器仍可能要求授权连接。
- **账号索引**：默认从 URL 中的 `authuser` 或 `/u/N` 推断，否则为 0，可以修改。
- **期望邮箱**：可选，填写后可防止导错账号。

脚本只保存服务地址和非敏感的账号索引，不保存 Cookie、密钥或期望邮箱，也不输出敏感日志。

## 读取方式

点击按钮后才会读取当前 AI Studio URL 的 Cookie 并发送请求，不会读取页面正文或其他网站的 Cookie。

1. 优先使用 `GM_cookie.list`。同名但不同域/路径的 Cookie、空值、Secure、HttpOnly、SameSite 和有效期都会保留；过期 Cookie 和分区 Cookie（包括 `partitionKeyOpaque`）被排除，面板显示数量。管理器元数据转换为标准 storage state 字段：有效期使用 `expires`，SameSite 使用 `Strict` / `Lax` / `None`（未指定或未知时省略）；会话 Cookie 省略有效期，hostOnly Cookie 的域名去掉前导点，不发送管理器私有字段。
2. `GM_cookie` 4 秒内未完成、失败或不可用时，回退到 `document.cookie`，以原版兼容的原始 Cookie 头字符串发送。此时域、路径、有效期与安全属性未知，无法判断分区，**也不包含 HttpOnly Cookie**，面板会明确提示这些限制，不能保证认证完整。GM 成功时不会混入 `document.cookie`，避免重新加入已排除的分区 Cookie。

根据 [Tampermonkey GM_cookie 文档](https://www.tampermonkey.net/documentation.php?q=GM_cookie)，HttpOnly 支持目前受 beta 版本和 URL 权限限制。安装脚本不保证能取得全部认证 Cookie，其他管理器的能力可能不同。导入失败时请先检查管理器的权限与支持情况。

## 服务端处理

- 请求发往 `/api/accounts/probe-import`，使用 Bearer 导入密钥、匿名跨域且禁止重定向，最长等待 300 秒。请求包含完整的 Cookie 记录数组和单个 `auth_user`，可附带 `email`。
- 导入只等待 AI Studio 上出现已登录的唯一身份，不要求出现输入框；编辑器和生成能力在生成流程中另行检查。身份识别优先使用可见的 Google 账号控件，排除隐藏菜单；出现多个可见身份时拒绝导入。
- 验证与生成共用有界工作队列，每个账号使用独立的 BrowserContext；请求断开、超时或服务关闭都会取消验证。
- 保存在一个事务中完成，只返回账号元数据。按已验证的邮箱更新现有账号，保留其 ID、名称和统计。保存前会再次检查导入密钥，已撤销密钥的请求不会写入。
- 成功响应为 `verification: browser_identity_only`、`generation_verified: false`。`details.truncated` 表示探测达到了账号数量上限，不代表已发现所有账号。
- 限制：HTTP 输入最大 1 MiB，Cookie 最多 512 个。
- 浏览器未配置、启动失败、断开、超时和页面不兼容分别返回固定的错误原因。服务日志只记录原因、请求 ID 和耗时，不记录 Cookie 或页面内容。
- 失败时脚本只把已知的服务端错误原因映射为固定的中文提示（例如区分导入密钥错误和 Cookie 未登录或过期），不显示服务端错误正文或未知原因值。

## 使用原版脚本

原项目 v0.1.2 / [v0.1.3](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/userscript/aistudio-cookie.user.js) 的“抓取并推送”功能兼容：服务地址填 `http://127.0.0.1:18451`，“管理密码”填 `cookie_import_key`。

- 后端兼容 `{cookies: "Cookie header", name_prefix?: "..."}`。没有 `auth_user` 时依次探测索引 0 到 7，单个索引未登录或返回 401 时继续探测，连续三次失败后停止；受 `cookie_import_max_accounts`（1–32）与 `cookie_import_timeout_seconds`（默认 180，最大 240 秒）限制。
- 验证导入只保存 cookies；storage state 中的 origins/localStorage 和其他元数据会被丢弃。
- 原版脚本发送合并后的 Cookie 头，不带元数据。后端为该路径恢复 SID、APISID、SAPISID、`__Secure-1PAPISID`、`__Secure-3PAPISID` 的 JavaScript 可读属性，供 Google 页面构造 RPC 鉴权；结构化 Cookie 中显式的 HttpOnly 属性保持原样。
- Cookie 头不带域与路径，无法还原未知 `__Host-` Cookie 的来源，这些项会被跳过并报告数量；需要精确元数据时请使用专用脚本。
- 原版的诊断包和其他管理接口未接入。

当前不支持分区 Cookie，也不会去掉分区属性后当作普通 Cookie 保存。

## 开发测试

```sh
node userscript/test_cookie_helper.cjs
```

在隔离的 VM 中运行脚本，模拟 DOM 与 GM API，不使用真实 Cookie。

可选的真实浏览器离线回归：把 `AIS2API_TEST_BROWSER` 设为 CloakBrowser 可执行文件后执行 `node browser/test_cookie_runtime.cjs`。该测试使用临时资料目录、合成 Cookie 和本地拦截页面，阻止外网请求，验证 Cookie 可读属性与无编辑器时的身份识别。C++ 侧的 Cookie 头转换由单元测试覆盖。两者都不能替代真实账号验收。
