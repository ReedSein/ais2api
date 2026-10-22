# 验证记录

更新：2026-10-22。当前为开发实现，尚未达到完整版本的发布门槛。已整合可选内置的 [Web 管理界面](webui.md)，通过独立鉴权的 [管理 API](management-api.md) 操作服务并读取实时日志，另保留独立密钥的 userscript Cookie 导入入口。管理 API 的验证记录见 [专项报告](reports/management-api-2026-10-21.md)。

## 已执行

Google 搜索与内置工具转换的后续修复和验证见 [专项报告](reports/grounding-tools-2026-10-22.md)。本轮没有可用的已导入账号，未进行真实 Google 搜索验收。

| 范围 | 环境与结果 | 能证明的范围 |
|---|---|---|
| C++ 构建 | Windows x64、MSVC 14.51、C++20、Release、静态运行库，成功 | 本机编译及链接 |
| C++ 自动测试 | Windows 185 用例、2,872 条断言全部通过（启用 WebUI） | 合成协议、解析器、存储、CLI 控制通道、HTTP、配置密钥与端口诊断、资源限制、下载、浏览器包与内嵌页面路由 |
| WebUI 整合 | Windows 前端类型检查、生产构建、内嵌与纯后端构建、安装及 ZIP 验包通过；浏览器完成合成实例登录、模型/密钥/配置页面和日志 SSE 检查 | 本地管理界面集成；本轮未导入真实账号，也未验证 Linux/Termux WebUI 构建，详见 [WebUI 文档](webui.md) |
| Responses 兼容接口 | Windows 专项 14 用例、233 条断言通过；本次 CLI 冒烟 34 项通过 | JSON / 命名 SSE、usage、函数签名和完整历史、重复工具 ID、图片输入映射、schema、截断及失败状态、流式限额和 FIN/RST 取消；未运行真实账号或 SDK 集成 |
| 网页桥接脚本 | Node 语法检查及 24 项测试通过 | 模拟页面下的捕获、快照、信用协议、取消、身份与错误分类 |
| Cookie 导入与专用 userscript | Windows / Debian 离线 HTTP 测试及 Node VM 测试通过 | 原版推送格式、独立密钥、元数据转换、分区过滤、身份检查、事务更新、队列限额、超时、取消与保存前撤销检查；未使用真实 Cookie |
| 原版 Cookie 头兼容修复 | Windows / Debian 真实浏览器离线回归通过 | 五项 Google 鉴权 Cookie 保持 JS 可读、HSID 保持 HttpOnly、无 textarea 时识别唯一账号；合成 Cookie 与拦截页面，不是 Google 登录或生成验收 |
| 真实账号模型发现 | Windows 冷启动及缓存请求、Gemini 列表、模型详情与 WSL localhost 调用均 HTTP 200，列表 41 项；配置模型 0 项 | 真实 Cookie、页面唯一身份、上游 ListModels；不代表真实内容生成通过 |
| 真实账号基础文本生成 | gemini-3.1-flash-lite 的 OpenAI 普通生成、SSE、Gemini 普通生成和 WSL 多轮均 HTTP 200，分别正确返回 OK / NEXT | Windows 真实账号与 XHR 模板、逐请求快照、正文、usage 和 SSE 完成；不代表全部模型或高级能力验收 |
| LLM API 对照及廉价模型真测 | 固定原版 3e2eff8，31 项离线同输入转换对照确认合同不完全一致；更新 Cookie 后共 5 次生成、5 次实际尝试 | Flash-Lite system/多轮/schema/图片、原生 SSE 音频/stop 及 Gemma OpenAI SSE 通过；函数调用 403、回传未执行，不能宣称完整等价；见 [对照报告](reports/llm-api-parity-2026-10-21.md) |
| CLI 冒烟 | Windows / Debian 各 34 项通过，临时服务与目录清理成功 | init 不生成密钥、旧配置迁移、start/stop、在线存储、配置密钥轮换与清空、脱敏、热更新 |
| Windows 安装包 | CMake install、CPack ZIP、摘要及资产检查通过 | 独立后端、脚本、清单、schema、文档及依赖许可证打包 |
| Windows 原生浏览器准备 | 锁定附件完成下载、SHA-256 校验、解压；重跑复用安装目录成功 | CloakBrowser 146.0.7680.177.4 实际启动、CDP 1.3、创建及释放隔离上下文 |
| 真实浏览器未登录路径 | 使用合成 Cookie，实际页面返回登录跳转，401/session_expired 分类正确 | 真实浏览器启动、导航、桥接脚本及过期状态记录；没有验证真实账号生成 |
| 构建脚本及 CI 定义 | PowerShell Parser、sh -n 通过；新增 Debian job 的 YAML 解析通过 | 配置语法，不代表远端 CI 已成功执行 |
| 本地回放短测 | 固定 4 客户端、100 ms 间隔，120 秒，4,414 次全成功 | 合成生成源下的 HTTP/SSE、调度、CPU/RSS 短测；不是浏览器或两小时验收 |
| Debian 13 / WSL 2 浏览器 | 普通用户、保持沙箱，CDP 1.3 及隔离上下文创建/释放成功；Node 24 项测试通过 | 无登录浏览器及离线桥接；不代表真实账号生成 |
| Debian 13 GCC / Clang | GCC 14.2 及 Clang 19 ASan/UBSan 均为 105 用例、1,591 条断言通过；实际浏览器 CDP 测试通过 | 本机 WSL 2 原生 Linux 验证，不能代替 Ubuntu / Termux |
| Debian 安装与服务 | C++ setup、TGZ 内容及摘要检查、34 项已安装 CLI 冒烟、Windows 到 WSL localhost 健康请求通过 | 系统依赖、锁定浏览器、CLI/IPC/HTTP；无真实账号 |
| Linux 解析器模糊测试 | Clang ASan/UBSan 下 21 秒完成 878,837 次输入，无 sanitizer 错误 | 短时 wire parser 冒烟，不是长期稳定性或完整覆盖证明 |
| 便携目录与内置浏览器 | Windows / Debian 分别通过普通模式 35 项、内置浏览器模式 38 项检查；此前两端实际浏览器各 1 用例、8 条断言通过 | 更换工作目录、移动到含空格目录、用户目录无新增数据、配置密钥保留、实际 CDP |
| 本地浏览器整合包 | Windows ZIP / Debian TGZ，逐文件清单、SHA-256、许可证及无用户数据检查通过 | 仅供个人或内部使用；不是已完成真实账号验收的正式发行版 |

Debian 13 的系统、命令、浏览器测试方式及保留的本机环境见 [WSL 2 适配报告](reports/debian13-wsl2-2026-10-20.md)。
便携存储、下载对照及浏览器整合包的本次验证见 [便携与浏览器包报告](reports/portable-browser-2026-10-20.md)。

C++ HTTP 测试使用真实 loopback TCP、HTTP 分块编码、SQLite 和本地 IPC；生成源是合成的 `GenerationBackend`。它们验证了多轮、Gemini 聚合、OpenAI SSE usage 块、正常结束、首内容后错误不重试、客户端断连取消。生产 CLI 始终使用 BrowserGateway，没有模拟生成开关。

Responses 新增 `POST /v1/responses`，使用无状态完整历史模式，接口说明见 [Responses 使用说明](responses.md)。本次 MSVC Release 构建成功，完整 CTest 在正常用户执行环境中 31 秒通过。第一次受限环境的完整回归停滞后被终止，不计作通过；正常环境重跑的通过结果与 Responses 专项结果分别保留。当前未为这项新增能力重跑 Linux/Termux 或真实浏览器、真实账号测试，也未安装客户端 SDK；这些结果不能由本机合成 HTTP 测试替代。

其他回归包括任意网络分块、UTF-8、XSSI、截断及对象限额；函数值与 schema、签名逐块保留、重复工具调用 ID 及扩展往返；账号隔离元数据、冷却与持久化；归档越界、重复文件、Windows 设备路径；32 个半请求头连接上限；累计媒体预算；超大及已完成下载残片处理。

本次还验证了 probe all 在单账号过期时继续检查其他账号、过期账号不进入调度、无可用账号时 ready 返回 503，以及 Cookie 文件/管道末尾 CRLF 和 UTF-8 BOM 导入。Windows 的安装验证使用临时私有资料目录和 CDP，不再使用不能可靠退出的 chrome.exe --version。受限执行环境的 Schannel 返回 SEC_E_NO_CREDENTIALS，下载在正常用户环境完成，TLS 验证始终开启。

默认端口已改为 `18451`，并核对其不在本机当前 Windows TCP 保留范围内；系统保留范围仍可能变化。API Key 由 `config.json` 的 `api_key` 配置，初始化不生成密钥，旧数据库密钥不再授权 HTTP。测试覆盖空值拒绝、无效 reload 保留原配置、轮换及清空生效、凭据脱敏、旧配置补空字段，并确认验包工具拒绝包含 `config.json` 的归档。

Cookie 推送使用独立的 `cookie_import_key`，默认空值禁用。导入与生成共用有界队列，验证唯一身份后一次事务保存；请求取消或保存前密钥撤销不会写入账号。专用脚本仅在点击后读取与推送，不持久化导入密钥；GM Cookie 元数据不可用时按原版发送原始 Cookie 头，并提示属性未知及缺少 HttpOnly。管理器 API 通过模拟验证；本轮原版脚本在服务启动后由用户确认导入成功，随后真实生成通过。没有单独验收专用脚本在所有扩展管理器下的真实流程。先前登录跳转及更新后的结果分别保留在报告，不互相替代。安装与兼容说明见 [userscript 文档](userscript.md)。

## 尚未满足的门槛

| 发布条件 | 当前状态 |
|---|---|
| Windows 真实账号单轮、多轮、流式 | gemini-3.1-flash-lite 的 Windows 普通生成、OpenAI SSE 与多轮通过；完整能力矩阵未完成 |
| 真实工具调用、思考签名、多模态输入、生图 | 图片及短音频输入通过；函数调用返回 403，结果回传及逐块签名真实往返未验证，生图未测 |
| 当前 AI Studio 页面初始化及身份确认 | Windows 真实身份、模型发现及文本生成初始化通过；其他平台真实账号尚未验收 |
| Ubuntu 24.04 GCC/Clang | CI 已配置；本机 Debian 13 测试通过，Ubuntu 尚未实测 |
| Linux ASan/UBSan 与 libFuzzer | Debian 13 / WSL 2 实测通过；其他发行版与长期模糊测试未完成 |
| Android 10+ ARM64 Termux | 原生构建及 proot 准备入口已实现，未在真实设备执行 |
| 三端干净环境自动准备、升级失败及回退 | 未完成真实验收 |
| 至少两小时固定回放、浏览器资源及泄漏检查 | 未完成；短测不能代替两小时结果 |
| 同设备、同负载与原项目对照 | 未执行，无加速倍数结论 |

`/ready` 的 `scope=local_prerequisites` 及 `google_verified=false` 是有意限定：它不是生成成功证明。配置模型标记为 configured；没有把固定模型表当成实测能力。

## 当前明确限制

2026-10-22 针对 Cherry Studio schema 拒绝问题完成了三种数据 API 的适配审查：常见非 strict JSON Schema 经过统一转换，新增本地引用展开及资源上限，并修复 HTTP 请求头兼容问题。MSVC Release 与全量 CTest 通过；详细修复、接口矩阵、降级语义及未验证项见 [API 兼容审查](reports/api-compatibility-2026-10-22.md)。

随后新增默认关闭的 `ignore_unsupported_parameters` 热更新开关，允许显式舍弃已知不支持的可选参数继续生成。前端类型检查、含 WebUI 的 MSVC Release 构建及全量 CTest（207 个测试、3241 项断言）通过；开关关闭/开启/再关闭与三种 API 普通/SSE 路径均由本机合成 HTTP 回归覆盖。

同日修复安装目录旧桥接与新 exe 混用导致的 browser_incompatible：脚本改为内嵌并增加版本/阶段诊断，完整 CTest 208 个用例、3250 项断言和脚本 37 项回归通过；本机安装已更新，Flash-Lite 真实生成复测 HTTP 200。证据与范围见 [桥接版本错配修复](reports/browser-embedded-bridge-2026-10-22.md)。

thinkingBudget 和 seed 没有已核实的私有协议映射，为兼容 Gemini 客户端接受合法值但忽略，并记录字段名；不保证预算或随机种子生效，thinkingBudget=0 也不关闭模型思考。includeThoughts=false 在网关隐藏思考文本，保留签名、工具调用和 usage。强制函数调用 ANY/required/named、VALIDATED 和 strict JSON Schema 仍明确拒绝。thinkingLevel、原生思考块与逐块签名已有编解码。不能据此声称所有 Gemini 参数或 OpenAI 可选字段完整支持；详见 [协议合同](protocol.md)。

当前只允许一个驻留上下文和一个生成任务；队列与资源限额可配置，尚无多驻留上下文并发调度。统计保存账号/模型的调用计数及 usage；阶段耗时与字节量写入脱敏日志。日志不写正文、Cookie、API Key 或签名。

Termux 使用官方 proot-distro 管理器，其当前系统依赖包括 Python。C++ 后端和浏览器脚本不依赖 Python/Node 运行；开发测试工具可以使用它们。系统包版本由平台包管理器决定，浏览器版本和桌面源码依赖则锁定。

## 复核命令

```text
ctest --preset windows
node --check browser/bridge.js
node --test browser/test_bridge.cjs
python tests/cli_smoke.py --exe build/windows/ais2api.exe
python scripts/check-package.py build/windows/packages/ais2api-0.1.0-Windows-AMD64.zip
```

Linux/Termux 构建入口和系统前提见 [平台说明](platforms.md)。固定负载命令、采样范围及报告限制见 [性能说明](performance.md)。默认安装包不包含浏览器；可另行构建仅供个人或内部使用的 Browser-Local 整合包。两种安装包均不包含用户凭据。
