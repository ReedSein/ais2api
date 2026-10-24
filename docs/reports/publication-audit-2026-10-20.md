# 上传前范围与归属审查

审查日期：2026-10-20。范围为本地 C++ 重写项目的上传候选、忽略规则、文档报告及打包归属；未操作 Git、推送、创建远程仓库或发布附件。写入本报告前扫描了 67 个候选文本，排除了 `.git/`、`.analysis/`、`.tools/`、构建输出、依赖安装树和运行数据。参考克隆只核对了 LICENSE 与文档目录名，没有读取其中的凭据、数据库或实现文件。

## 上传范围

- `.gitignore:1` 排除参考克隆、工具、构建输出、依赖树与数据；`.gitignore:12` 起排除本地编辑器、编译产物和归档；`.gitignore:32` 起排除运行状态、常见凭据、数据库、私钥及环境文件；`.gitignore:59` 起排除 Agent/云账号状态。
- 预期源候选为根构建/许可/说明文件，以及 `src/`、`include/`、`browser/`、`schemas/`、`scripts/`、`tests/`、`tools/`、`docs/` 和 `.github/workflows/build.yml`。`tools/replay.cpp` 是合成开发工具，不是下载依赖或运行数据。
- 忽略规则不会自动排除任意名称的外部账号导出，也不会移除已跟踪文件。上传前仍须由主 Agent 对实际暂存区使用明确源文件清单复核；本审查没有检查 Git 暂存区或历史。

## 凭据与主机信息

| 文件/行号 | 类别与结果 |
|---|---|
| 全部候选文本 | 未发现私钥头、常见 Google API Key/provider token 形态或绝对用户/工作区主机路径；这不是对未知格式凭据的穷尽证明 |
| `browser/test_bridge.cjs:112`、`:113`、`:114` | 邮箱形态，位于身份测试的合成示例上下文 |
| `tests/cli_smoke.py:170`；`tests/test_generation.cpp:137`、`:138`；`tests/test_state.cpp:65`、`:256`、`:257`、`:258`、`:273`、`:274`；`tools/replay.cpp:221` | Cookie 形态，均位于测试夹具或合成回放构造上下文；未输出其值 |
| `docs/reports/windows-paced-120s.json:1`、`docs/reports/windows-unpaced-120s.json:1` | 度量报告；字段为负载、计数、延迟、CPU/RSS、队列及验证标志，未发现凭据字段或主机路径 |
| `docs/reports/README.md:3` | 设备环境指纹：OS、CPU、内存及编译器版本；不是账号凭据，发布者可按隐私偏好决定是否保留 |

`docs/reports/README.md:14` 已明确合成负载、无真实模型生成与未完成门槛；不应将这些报告解释为真实账号或跨平台发布验收。`.env.example` 等允许提交的示例仍应保持合成内容。

## 许可与第三方归属

- `LICENSE:1` 为完整 MPL 2.0 文本；`NOTICE:8` 声明本项目自有源码、脚本、schema、测试和文档的范围。源码中的 MPL 标识不替换第三方原有许可。
- `README.md:104`、`docs/protocol.md:3` 与 `THIRD_PARTY_NOTICES.md:25` 标明固定参考来源及独立实现声明。参考克隆的 `LICENSE:1`、`:3` 为 MIT 与其贡献者版权。本审查未做实现逐行相似性比对，不能单凭语言重写或扩展名变化证明来源独立；以后实际引入第三方实现时应保留对应原始许可及归属。
- `vcpkg.json:4` 声明桌面依赖；`THIRD_PARTY_NOTICES.md:5` 列出主要组件，`:19` 说明并非完整传递依赖清单。`CMakeLists.txt:87`、`:90` 保留依赖安装树的 `copyright` 文件；不得把这些第三方文件改标为本项目 MPL。
- `THIRD_PARTY_NOTICES.md:21` 区分独立下载的 CloakBrowser/Chromium。浏览器附件及系统包不进入自有源码范围，相关归档内的许可应继续保留。
- `CMakeLists.txt:83`、`:84` 已将 LICENSE、NOTICE、第三方说明及发布指南列入安装清单；`:86` 会打包报告，因此后续新增报告也须按相同规则脱敏。`:111` 禁止生成整个工作区的默认源码打包配置。

## 旧产物与最终复核

审查时发现原默认名称的 ZIP 属于切换 MPL 前的开发产物。构建目录及归档始终不在源码上传候选中；旧包不能充当当前 MPL 发行附件。

主 Agent 随后重建了明确命名的新包 `ais2api-0.1.0-dev-MPL2-Windows-AMD64.zip`，新验包规则通过：80 个文件，含 MPL、NOTICE 和第三方说明；包内 MPL 正文与当前仓库 LICENSE 字节一致，SHA-256 旁文件匹配。包 SHA-256 为 `e9d0e2d408aed381cb307c37644494d86a6d7ae6f2af0a982f8036a53eafc6f5`。当前附件仍是开发包，不代表真实账号或三端完整验收。

本次重建和检查了 MPL 开发包，没有执行远端 CI、真实 Termux 或真实账号调用。源码上传准备与正式版本发布是不同阶段；后续发布流程及对应源码说明见 [github-publishing.md](../github-publishing.md)。
