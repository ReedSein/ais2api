# Cherry Studio browser_incompatible 与桥接版本错配修复

日期：2026-10-22。此次读取本机已安装实例的脱敏日志、比较安装文件，并更新该实例完成真实生成复测。

## 证据与原因

用户提供的 HTTP 502 日志时间戳为 1792673079。对应生成请求已完成参数归一化（normalize_ms=0），在浏览器阶段约 11 秒后报 browser_incompatible；此前相同形态请求也在约 11 秒后失败。

安装目录的 ais2api.exe 已更新到带参数舍弃开关的版本，但其优先加载的 browser/bridge.js 仍为 2026-10-21 02:39 的旧文件，缺少新主程序调用的 finishBootstrap()，也缺少后续鉴权及权益适配更新。旧加载顺序优先使用 exe 旁脚本，因此工作区新脚本并未生效。新程序调用缺失的页面方法时，CDP exceptionDetails 被统一归为 browser_incompatible。

该证据确认存在会触发此故障的宿主/脚本合同错配；不能仅由通用 HTTP 错误码判断为 Cookie、客户端请求格式或某种特定网页变化。

## 修复

- CMake 将 browser/bridge.js 按字节嵌入程序，脚本改变会触发重建。运行时仅使用内嵌版本，旧安装文件和开发源码目录均不能覆盖它；外置脚本保留为参考资源。
- 内嵌脚本附 SHA-256；单测核对字节哈希及必须存在的宿主方法，包括 finishBootstrap。
- 管理诊断增加 browser 字段，含 bridgeSource、bridgeSha256 与阶段 phase；浏览器繁忙时诊断立即返回 busy，不等待整次生成。
- browser_incompatible 另写 browser_failure 日志，记录请求 ID、阶段和桥接版本等稳定信息，不记录原始 JavaScript 异常、Cookie、快照或页面正文。

## 验证与本机更新

Windows MSVC Release 构建通过，完整 CTest 通过 **208 个用例、3250 项断言**（35.09 秒）；桥接语法检查及 **37 项 Node.js 回归**通过。本轮未重跑 Linux/Termux。

当前 Windows 安装的程序和参考脚本已备份、更新并重启服务，配置字节保持不变。HTTP /health 返回 200，管理诊断确认 bridgeSource=embedded，SHA-256 与工作区源码一致。

随后以已发现的 gemini-3.1-flash-lite 做一次最小 Chat Completions 复测，最终 HTTP 200、finish_reason=stop，诊断为 gateway_ready / generation_complete，未再出现 browser_incompatible。该请求经历一次既有策略允许的上游重试，总生成阶段约 70.8 秒。测试没有断言模型必须逐字输出预定文本，也没有保存正文。

这验证了本机此模型的真实普通生成路径；没有操作 Cherry Studio GUI，也不代表所有模型、工具和客户端请求形态均完成真实验收。
