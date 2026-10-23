# Web 管理界面

WebUI 是可选的 Vue 管理控制台。启用后，前端静态资源会在构建时生成并嵌入 ais2api 可执行文件；运行时无需安装 Node.js，也无需把 `webui/` 或额外资源目录放在程序旁。未启用 WebUI 的构建仍可通过 CLI 和管理 API 管理服务。

## 构建

WebUI 构建需要 Node.js `^20.19.0` 或 `>=22.12.0`，推荐 Node.js 22.12 或更新版本。构建机还需满足对应平台的 C++、CMake、Ninja 和依赖要求，详见 [平台说明](platforms.md)。

Windows 在开发者 PowerShell 中运行：

```powershell
./scripts/build.ps1 -WebUI
```

Linux 运行：

```sh
sh scripts/build.sh linux --webui
```

Termux 运行：

```sh
sh scripts/build.sh termux --webui
```

也可以在直接调用 CMake 时设置 `-DAIS2API_BUILD_WEBUI=ON`。项目会执行前端依赖安装和生产构建，并把产物嵌入后端。默认值为 `OFF`，因此纯后端构建不要求 Node.js。构建脚本的其余选项（如 `--jobs`、`--skip-tests`、`--package`）仍可照常使用。

## 启动与登录

先按项目的[首次运行说明](../README.md#首次运行)初始化配置并启动服务。新创建的配置默认管理密码为 `admin`；首次登录后控制台会强制进入“设置新的管理密码”页面，修改前服务拒绝其他所有管理请求。默认密码只接受来自本机或局域网（回环、私有地址、链路本地、IPv6 ULA）的连接；若服务只能从公网访问，请先在 `config.json` 中设置一个足够强的 `admin_key`，再重启服务或执行 `ais2api config reload`。管理密钥与生成 API 的 `api_key` 相互独立。

在浏览器打开服务地址，例如 `http://127.0.0.1:18451/`，进入登录页并使用 `admin_key` 登录。控制台通过同源 `/api/admin/v1` 调用管理 API，无需设置跨域来源。账号、模型、密钥、统计、日志、配置、诊断和设置页面分别位于 `/accounts`、`/models`、`/keys`、`/stats`、`/logs`、`/config`、`/diagnostics`、`/settings`；`/login` 为登录页。

之后可在“设置 → 管理密码”中再次修改密码，新密码立即生效，即使服务存在待重启的配置也是如此；新密码需为 8–4096 个可见 ASCII 字符且不能是 `admin`。已有配置文件中 `admin_key` 为空时，管理 API 仍保持关闭。请像其他凭据一样保护该密钥；如需撤销，清空 `admin_key` 并重新加载配置或重启服务。详见[管理 API 文档](management-api.md)。

登录页的后端地址留空表示当前服务。管理密钥默认仅保存在当前标签页的 sessionStorage；勾选“在此设备上记住”才写入 localStorage。

## 前端开发

在 `webui/` 运行 `npm ci` 和 `npm run dev`，默认开发地址为 `http://localhost:5173`，管理请求代理到 `http://127.0.0.1:18451`；可用 `AIS2API_URL` 环境变量指定其他后端。类型检查使用 `npm run typecheck`，交互测试使用 `npm test`（Vitest + happy-dom，模拟管理 API，不连接真实服务），生产构建使用 `npm run build`。独立静态部署时，在登录页填写后端地址，并在后端 `admin_origins` 中配置页面的完整来源。

## 验证范围

2026-10-22 在 Windows x64 / MSVC Release / Node.js 22.20 验证了前端类型检查与生产构建、内嵌及纯后端构建、C++ 全量回归（185 用例 / 2872 断言）、CLI 冒烟（34 项）、目录迁移（35 项）、安装与 ZIP 包校验。浏览器使用无真实账号的合成实例，验证同源登录、模型列表、密钥页接入地址、配置页读取和日志实时 SSE。

本轮未验证 Linux/Termux 的 WebUI 构建或真实账号的管理与生成；CI 已配置桌面 WebUI 构建，但本机检查不代表 CI 已运行通过。Google 生成能力继续以项目的[验证记录](validation.md)为准。
