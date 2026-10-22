# Google 请求逻辑对齐（2026-10-22）

对照 [ReedSein/aistudio-api-termux 的 master 提交 642ebea](https://github.com/ReedSein/aistudio-api-termux/tree/642ebea615988dfb7a9ec82e7575fbb052b01636)。本次按实际请求收益选择行为，采用本项目的 C++/页面桥接实现；没有复制原项目实现文件，也没有调用真实 Google 或替换运行中的服务。

## 已应用

| 行为 | 实现与效果 |
|---|---|
| Authorization 更新 | 当前隔离页面 CDP Cookie 缓存；每次重算 SAPISIDHASH/1P/3P；缺失、过期及 60 秒刷新，切号清除；捕获头仅作当前页面回退 |
| 会员等级头 | 实查 GetAiStudioBenefitTier；移除旧头，仅 plus/pro/ultra 设置 TIER0/1/2；查询有界、按页面缓存，失败不猜等级；请求固定记录发送时等级 |
| 文本默认 | 未指定 thinkingLevel 时 HIGH；Gemini/Gemma 未指定安全设置时四类 OFF；调用者明确设置优先 |
| 图像适配 | 默认 IMAGE-only、MINIMAL；TEXT+IMAGE 固定 [2,1]；发送前清除 safety 与 gc[7,13,17]，避免文本参数被图像模型拒绝 |
| 配置优先级 | 精确 model_defaults 对 generationConfig/thinkingConfig 逐字段补缺，而非整体对象覆盖 |
| 工具模型门槛 | 图像只允许搜索；其他 Gemini 允许 search/code/Maps/URL；其他模型只允许 search/code。自定义函数保持原处理；不兼容内置工具明确报错 |
| 短暂路由异常 | 仅 404 且同时识别 ambiguous request for service 与 GenerateContent 才同账号有界重试；普通 404 直接返回，不冷却账号 |
| 空响应 | 仅首次尝试的空 204 可再试一次，仍受 max_attempts 限制；对外重复空响应为 502 |
| 配额冷却 | 429 按模型隔离；minute 不增加每日计数，day/unknown 使用 60/60/300/300/600 秒。独立持久计数按美西午夜重置，进入 drip 后成功仍保留计数，尊重更长 Retry-After |
| 权限失败 | 等级未知的 403 不换号、不冷却；已知等级的权限失败只暂停受影响模型五分钟，缩小原项目账号级冷却的影响 |
| 完成判定 | 传输完整之外还必须有有效 finishReason；缺失或 UNSPECIFIED 不能记成功；已经输出内容不重新生成 |

## 有意保留的差异

- 每次重建请求已知字段，清除历史 tools、cached content、evergreen 与 location；不继承浏览器草稿的采样、缓存或位置。未知顶层槽仍保留以兼容协议变化。
- 保留严格参数校验和工具报错，避免静默丢弃用户要求。
- 保留 32 KiB BYOB 逐块 ACK、完整 JSON framing 校验和账号隔离；不改为全响应缓存或无界推送队列。
- ListModels 继续依赖已验证浏览器页面并返回真实/显式配置模型，不引入固定模型列表或服务器 HTTP Cookie 重放。
- 路由重试沿用已验证上下文，每次重新编码并取得 snapshot；不为短暂路由错误重启浏览器。
- 不引入多驻留账号、蓝绿并发调度、持久会员元数据或管理页面扩展；这些不属于当前请求兼容性修改。

## 验证

Windows/MSVC 本次验证结果：

| 检查 | 结果 |
|---|---|
| C++ 核心、测试程序和 CLI 链接 | 通过；原主程序被运行中的服务占用，另行链接 `build/windows/ais2api-auth-check.exe` 用于 CLI 验证，未覆盖现有服务 |
| 完整 C++ 回归 | 180 项测试、2784 条断言全部通过；报告 `.tools/upstream-parity-native-tests.log` |
| bridge 语法及脚本回归 | `node --check browser/bridge.js` 通过，`node --test browser/test_bridge.cjs` 37/37；报告 `.tools/upstream-parity-bridge-tests.log` |
| CLI/IPC/HTTP smoke | 34 项全部通过，清理成功；报告 `.tools/upstream-parity-cli-tests.json` |
| 离线真实浏览器/CDP | Edge 无外网测试通过：Cookie 属性、HttpOnly 轮换值、不同 BrowserContext 隔离 |
| 差异格式检查 | `git diff --check` 通过 |

完整 C++ 和浏览器回归在沙箱外运行，以避免沙箱的本地 TLS/IPC 与 GPU 子进程权限限制。配额刷新样例最初因无邮箱的重复导入新增账号而失败，已改为同邮箱原账号凭据替换；工具 HTTP 样例改用符合 URL context 门槛的合成 Gemini 模型。没有通过放宽生产校验来规避这些失败。

测试使用合成 Cookie、合成 RPC 响应和本地 HTTP/CDP，不代表真实账号的地区、会员权益、配额或模型能力已经验证。本次没有重新运行 Linux/Termux 的实机验收。

关键覆盖：模型默认与明确参数优先级、图像清理、工具门槛；等级枚举/XSSI/响应上限/超时/单飞/缓存/取消/旧头清洗；RPC 分类、同账号重试预算、普通错误不误冷却、HTTP/SSE 未完成响应；配额状态迁移、分模型、计数隔离、DST、长 Retry-After、凭据更新保护。
