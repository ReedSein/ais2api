# 验证记录

更新：2026-10-20。当前为开发实现，尚未达到完整版本的发布门槛。代码与文档没有 Web 管理界面或 HTTP 管理接口。

## 已执行

| 范围 | 环境与结果 | 能证明的范围 |
|---|---|---|
| C++ 构建 | Windows x64、MSVC 14.51、C++20、Release、静态运行库，成功 | 本机编译及链接 |
| C++ 自动测试 | Windows 81 用例、1,371 条断言全部通过 | 合成协议、解析器、存储、CLI 控制通道、HTTP、端口错误诊断、资源限制、下载与浏览器包 |
| 网页桥接脚本 | Node 语法检查及 9 项测试通过 | 模拟页面下的捕获、快照、信用协议、取消、身份与错误分类 |
| CLI 冒烟 | 23 项通过，临时服务与目录清理成功 | init/start/stop、有效启动配置、在线存储、密钥鉴权与撤销、脱敏、热更新 |
| Windows 安装包 | CMake install、CPack ZIP、摘要及资产检查通过 | 独立后端、脚本、清单、schema、文档及依赖许可证打包 |
| Windows 原生浏览器准备 | 锁定附件完成下载、SHA-256 校验、解压；重跑复用安装目录成功 | CloakBrowser 146.0.7680.177.4 实际启动、CDP 1.3、创建及释放隔离上下文 |
| 真实浏览器未登录路径 | 使用合成 Cookie，实际页面返回登录跳转，401/session_expired 分类正确 | 真实浏览器启动、导航、桥接脚本及过期状态记录；没有验证真实账号生成 |
| 构建脚本及 CI 定义 | PowerShell Parser、sh -n 通过；新增 Debian job 的 YAML 解析通过 | 配置语法，不代表远端 CI 已成功执行 |
| 本地回放短测 | 固定 4 客户端、100 ms 间隔，120 秒，4,414 次全成功 | 合成生成源下的 HTTP/SSE、调度、CPU/RSS 短测；不是浏览器或两小时验收 |
| Debian 13 / WSL 2 浏览器 | 普通用户、保持沙箱，CDP 1.3 及隔离上下文创建/释放成功；Node 9 项测试通过 | 无登录浏览器及离线桥接；不代表真实账号生成 |
| Debian 13 GCC / Clang | GCC 14.2 构建及 81 用例、1,363 条断言通过；此前 Clang 19 ASan/UBSan 的 80 用例、1,355 条断言及实际浏览器测试通过，新增端口诊断尚未重跑 sanitizer | 本机 WSL 2 原生 Linux 验证，不能代替 Ubuntu / Termux |
| Debian 安装与服务 | C++ setup、TGZ 内容及摘要检查、23 项已安装 CLI 冒烟、Windows 到 WSL localhost 健康请求通过 | 系统依赖、锁定浏览器、CLI/IPC/HTTP；无真实账号 |
| Linux 解析器模糊测试 | Clang ASan/UBSan 下 21 秒完成 878,837 次输入，无 sanitizer 错误 | 短时 wire parser 冒烟，不是长期稳定性或完整覆盖证明 |
| 便携目录与内置浏览器 | Windows / Debian 分别通过普通模式 31 项、内置浏览器模式 34 项检查；两端实际浏览器各 1 用例、8 条断言通过 | 更换工作目录、移动到含空格目录、用户目录无新增数据、密钥保留、实际 CDP |
| 本地浏览器整合包 | Windows ZIP / Debian TGZ，逐文件清单、SHA-256、许可证及无用户数据检查通过 | 仅供个人或内部使用；不是已完成真实账号验收的正式发行版 |

Debian 13 的系统、命令、浏览器测试方式及保留的本机环境见 [WSL 2 适配报告](reports/debian13-wsl2-2026-10-20.md)。
便携存储、下载对照及浏览器整合包的本次验证见 [便携与浏览器包报告](reports/portable-browser-2026-10-20.md)。

C++ HTTP 测试使用真实 loopback TCP、HTTP 分块编码、SQLite 和本地 IPC；生成源是合成的 `GenerationBackend`。它们验证了多轮、Gemini 聚合、OpenAI SSE usage 块、正常结束、首内容后错误不重试、客户端断连取消。生产 CLI 始终使用 BrowserGateway，没有模拟生成开关。

其他回归包括任意网络分块、UTF-8、XSSI、截断及对象限额；函数值与 schema、签名逐块保留、重复工具调用 ID 及扩展往返；账号隔离元数据、冷却与持久化；归档越界、重复文件、Windows 设备路径；32 个半请求头连接上限；累计媒体预算；超大及已完成下载残片处理。

本次还验证了 probe all 在单账号过期时继续检查其他账号、过期账号不进入调度、无可用账号时 ready 返回 503，以及 Cookie 文件/管道末尾 CRLF 和 UTF-8 BOM 导入。Windows 的安装验证使用临时私有资料目录和 CDP，不再使用不能可靠退出的 chrome.exe --version。受限执行环境的 Schannel 返回 SEC_E_NO_CREDENTIALS，下载在正常用户环境完成，TLS 验证始终开启。

## 尚未满足的门槛

| 发布条件 | 当前状态 |
|---|---|
| Windows 真实账号单轮、多轮、流式 | 未验证：尚未提供测试凭据 |
| 真实工具调用、思考签名、多模态输入、生图 | 未验证：需要相应权限账号和真实上游响应 |
| 当前 AI Studio 页面初始化及身份确认 | 单元模拟通过，真实页面与账号未验收 |
| Ubuntu 24.04 GCC/Clang | CI 已配置；本机 Debian 13 测试通过，Ubuntu 尚未实测 |
| Linux ASan/UBSan 与 libFuzzer | Debian 13 / WSL 2 实测通过；其他发行版与长期模糊测试未完成 |
| Android 10+ ARM64 Termux | 原生构建及 proot 准备入口已实现，未在真实设备执行 |
| 三端干净环境自动准备、升级失败及回退 | 未完成真实验收 |
| 至少两小时固定回放、浏览器资源及泄漏检查 | 未完成；短测不能代替两小时结果 |
| 同设备、同负载与原项目对照 | 未执行，无加速倍数结论 |

`/ready` 的 `scope=local_prerequisites` 及 `google_verified=false` 是有意限定：它不是生成成功证明。配置模型标记为 configured；没有把固定模型表当成实测能力。

## 当前明确限制

thinkingBudget、includeThoughts、强制函数调用 ANY/required/named、VALIDATED 和 strict JSON Schema 没有已核实的私有协议映射，当前明确拒绝。thinkingLevel、原生思考块与逐块签名已有编解码。不能据此声称所有 Gemini 参数或 OpenAI 可选字段完整支持；详见 [协议合同](protocol.md)。

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
