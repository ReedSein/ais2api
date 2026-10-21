# ais2api

[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-blue.svg)](LICENSE)

面向 Windows、Linux 和 Termux 的 C++20 AI Studio 网页会话网关。提供 Gemini 与 OpenAI Chat Completions 数据接口，账号和运行状态通过 CLI 管理，密钥及策略写入配置；支持可选的 userscript Cookie 导入，没有 Web 管理页面。

**这是待真实账号验收的开发版本。** 离线协议测试、浏览器脚本测试和本机服务测试不代表 Google 网页适配、账号权限、生图或三端部署已经验证。具体结果与未完成门槛见 [验证记录](docs/validation.md)。

## 构建

Windows 使用 Visual Studio C++ 工具链、CMake 3.25+、Ninja，在开发者 PowerShell 中运行：

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 664933b8d385dfc01f4e0cdff4c7c74e71f6a740
.tools/vcpkg/bootstrap-vcpkg.bat -disableMetrics
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

Ubuntu 24.04 或 Debian 13 x86_64 安装 C++20 编译器、CMake、Ninja、Git、curl、zip/unzip 之后运行 `sh scripts/build.sh linux`。所有桌面依赖由 `vcpkg.json` 的固定 baseline 决定；源码下载由 vcpkg 校验。首次构建可能较长。WSL 2 的 Debian 13 使用同一 Linux 构建入口，源码、构建及运行数据应放在 Linux 文件系统内；具体步骤见 [平台说明](docs/platforms.md)。

Termux 使用 `sh scripts/build.sh termux`，系统提供 clang、cmake、ninja、boost、libcurl、openssl、libarchive、libsqlite；Catch2 从固定版本构建为开发依赖。Termux 的系统库版本由包管理器管理，并记录在验证报告中，不能宣称与桌面二进制依赖完全相同。

测试网页桥接脚本可选使用开发机上的 Node.js：

```text
node --check browser/bridge.js
node --test browser/test_bridge.cjs
```

Node.js 和原项目的 Python 服务都不是后端依赖。Termux 的发行版容器管理器可能包含其自己的系统依赖。

## 首次运行

配置默认位于可执行文件旁的 `config.json`，数据库、日志、下载和浏览器资料放在旁边的 `data/`，不使用用户的 AppData、HOME 或 XDG 配置目录。请将程序放在当前用户可写的文件夹。内置浏览器包会自动识别旁边的 `browser-runtime/`，不必下载浏览器。Windows 构建可执行文件位于 `build/windows/ais2api.exe`，其他平台对应各自构建目录。

```text
ais2api init
ais2api setup --yes
ais2api accounts import --file credentials.json --name personal
ais2api start
ais2api accounts list
ais2api accounts probe all
ais2api models list
ais2api status
```

`init` 只创建配置和数据库，不生成或输出 API Key。请在 `config.json` 的 `api_key` 字段填写自己的密钥，再启动服务。重复 `init` 不覆盖密钥；结果中的 `api_key_configured` 和 `next_step` 提示配置状态与下一步。所有 CLI 输出采用 JSON，日志写到 stderr。`start` 的日志保存到数据目录的 `service.log`。`serve` 在当前终端运行。`stop` 使用当前用户专属的本地控制通道停止服务。

也可使用 userscript 直接抓取并推送 Cookie：在配置里填写独立的 `cookie_import_key`，执行 `config reload`，安装 [ais2api 专用脚本](userscript/ais2api-cookie.user.js)，在 AI Studio 页面点击导入。原项目脚本的“抓取并推送”同样兼容，地址改为 `http://127.0.0.1:18451`，“管理密码”填写 `cookie_import_key`。后端验证唯一账号身份后才保存，重复推送更新原账号；详见 [脚本安装与兼容范围](docs/userscript.md)。导入仅验证浏览器登录身份，不代表生成能力已验收。

程序自己写入的配置使用相对路径，整个程序目录可一起移动；配置文件内的相对路径以配置文件所在目录为基准，CLI/环境变量中的相对路径以当前工作目录为基准。显式 `--config`、`--data-dir` 及现有配置中的绝对路径继续有效；不会自动搬动原来用户目录中的状态。

下载器对支持强 ETag 与 HTTP Range 的大文件使用四路分段下载，保留可续传片段，并在完成后核对长度与 SHA-256。不支持分段或身份不一致时安全回退到单连接；代理与 TLS 验证继续生效。实际速度取决于网络和上游限速。

可选的 `Browser-Local` 包附带锁定的 CloakBrowser，适用于本机个人或内部使用；对外分发须遵守其独立许可，详见 [第三方声明](THIRD_PARTY_NOTICES.md)。创建方法见 [平台说明](docs/platforms.md)。Linux 仍需要相应系统运行库；系统包及 Termux/proot 管理器状态由各自工具管理。

`setup --yes` 授权安装本次列出的系统依赖；Ubuntu/Debian 可能需要 sudo，Termux 会创建项目专属的 proot Ubuntu 24.04 容器。浏览器下载和运行目录默认均在程序旁的 `data/runtime/`。不会自动升级所有系统软件或切换镜像源。安装中断后可重跑；浏览器归档按固定摘要和大小验证。系统包的改变不属于浏览器回退范围。

已有浏览器可以通过 `--browser PATH` 或配置文件指定；`doctor` 检查本地前提，不会把浏览器文件存在当作 Google 调用成功。

## 接口

默认监听 `127.0.0.1:18451`。生成和模型接口始终需要 API Key；支持 `Authorization: Bearer`、`x-goog-api-key`、`x-api-key` 或 `?key=`。HTTP 日志不记录 URL 查询、请求头、正文或返回内容。

```sh
curl http://127.0.0.1:18451/v1/models -H "Authorization: Bearer YOUR_KEY"
curl http://127.0.0.1:18451/v1/chat/completions \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"ACTUAL_MODEL_ID","messages":[{"role":"user","content":"hello"}],"stream":true}'
```

Gemini 接口为 `/v1beta/models/MODEL:generateContent` 与 `:streamGenerateContent`；OpenAI 为 `/v1/chat/completions`。模型列表仅来自实时发现、缓存或显式配置，不内置声称可用的模型清单。高级内容通过 `ais2api` 命名空间无损往返，详见 [协议合同](docs/protocol.md) 和 [扩展 schema](schemas/protocol-extensions.schema.json)。

`/health` 只代表 HTTP 进程存活。`/ready` 检查 API Key、本地账号与浏览器前提，明确返回 `google_verified:false`；真实可用性必须通过账号探测和实际生成确认。

不提供 Responses、Files、embeddings、独立图片/音频接口和网页控制台。HTTP 增加可选的 userscript Cookie 导入入口，其他账号、配置及服务管理仍通过 CLI。`fileData`、无可靠私有协议映射的参数、强制工具执行等请求会明确报错。

## 管理与配置

```text
ais2api accounts import --file cookies.txt
ais2api accounts select ACCOUNT_ID
ais2api accounts reset ACCOUNT_ID
ais2api accounts remove ACCOUNT_ID
ais2api keys list
ais2api stats
ais2api config show
ais2api config validate
ais2api config reload
ais2api browser update --yes
ais2api browser rollback
ais2api stop
```

导入支持 Cookie 头、Netscape Cookie 文本、Cookie JSON、storage state 和已有凭据包。不会读取或覆盖旧项目目录。命令中的账号 ID 来自 `accounts list`；凭据不进入列表或统计。不要把真实凭据放入仓库。

`api_key` 默认为空，此时受保护接口拒绝访问，`/health` 仍可用且 `/ready` 未就绪。密钥最长 4096 个可见 ASCII 字符，不含空白。编辑密钥后执行 `config reload` 或重启，后续请求使用新密钥，设为空可撤销访问。`config show` 隐藏密钥，配置文件保存原值，应按凭据保护。`keys list` 仅返回配置来源及配置状态等元数据；`keys create/revoke` 返回 `reason=keys_managed_in_config`，提示编辑配置。旧数据库密钥记录保留，但不再授权 HTTP。Windows 端口保留范围可能动态变化，必要时修改 `port` 或使用 `--port PORT`。

配置优先级为默认值 → 配置文件 → `AIS2API_*` 环境变量 → CLI。`--config PATH` 和 `--data-dir PATH` 可用于完全隔离的安装。配置 schema 位于 [schemas/config.schema.json](schemas/config.schema.json)。启动端口、数据目录、浏览器和传输限额的变动需要重启；热更新会在校验完整配置后生效。

后台服务在线时，CLI 使用当前用户专属的命名管道或 Unix socket；离线操作必须先获得实例锁。API Key 保存在 `config.json`，Cookie 保存在访问权限受限的 SQLite 数据库中，未宣称应用层加密。备份和浏览器资料应视为凭据。

## 实现边界

浏览器上下文独立于标签页，每个请求固定其账号、上下文和一次性请求快照。模型返回的 thoughtSignature 与浏览器快照是两种不同数据。默认一个浏览器进程、一个驻留账号、一个生成请求，最多排队 8 个请求；取消贯穿 HTTP、原生层和浏览器读取。

浏览器桥接使用 BYOB 分块和逐块确认；生成输出不完整积存在无界队列。内联媒体和非流式响应具有独立对象上限，并不意味着整个 Chromium 进程的 RSS 可以由 C++ 精确限制。

原理参考 [aistudio-api-termux](https://github.com/ReedSein/aistudio-api-termux/tree/84b50663fcae79058fe33e0e45b5c3452da7c461)。实现代码和回归样例在本项目独立编写；上游私有协议变化仍需更新适配器。浏览器行为与验证限制见 [docs/browser.md](docs/browser.md)。

## 许可与贡献

Copyright (c) 2026 Mana Nekoha (mananekoha114)。

本项目自有源码、脚本、schema、测试及文档采用 [Mozilla Public License 2.0](LICENSE)。许可声明见 [NOTICE](NOTICE)，第三方依赖及浏览器保留各自许可，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。

贡献流程见 [CONTRIBUTING.md](CONTRIBUTING.md)。GitHub 首次上传及发布步骤见 [docs/github-publishing.md](docs/github-publishing.md)。当前仍为开发版本，不因上传 GitHub 而改变验收状态。
