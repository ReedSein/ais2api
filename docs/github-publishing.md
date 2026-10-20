# GitHub 上传与发布

本地默认分支为 `main`。第一次上传只发布源码和文档；浏览器、依赖 checkout、凭据、数据库、日志及构建输出不进入 Git。当前测试状态以 `validation.md` 为准，不能标成三端完整正式版。

当前推送目标为私有仓库 [mananekoha114/ais2api](https://github.com/mananekoha114/ais2api)。自有源码的版权署名为 Mana Nekoha (mananekoha114)，采用 MPL 2.0；第三方署名与许可不属于本次变更范围。

## 首次上传

1. 在自己的 GitHub 账号或组织下创建空仓库；名称建议 `ais2api`。公开性由维护者选择。不要额外初始化 README、LICENSE 或 .gitignore，以免与已有本地历史分叉。
2. 检查待发布提交，确认 `.analysis/`、`.tools/`、`build/`、`vcpkg_installed/`、本地账号文件及数据库没有被跟踪。
3. 首次配置远程时使用实际仓库地址，再推送：

```sh
git status --short
git log -1 --oneline
git ls-files
git remote add origin https://github.com/mananekoha114/ais2api.git
git push -u origin main
```

如果已有 origin，先用 `git remote -v` 检查它，再按实际需要执行 `git remote set-url origin URL`。不要使用强制推送覆盖未知远程历史。若远程已有提交，先 fetch 并检查差异。

Git 身份沿用本机已配置的 name/email，没有修改全局身份。公开提交会公开其中的作者信息；如需使用 GitHub noreply 地址，应在推送前按自己的账号设置调整本仓库身份并更新提交。

## GitHub 配置

仓库描述可用：`C++20 AI Studio gateway with Gemini and OpenAI Chat Completions APIs; CLI-only management; Windows, Ubuntu and Termux targets.`

建议 topics：`cpp20`、`gemini`、`openai-compatible`、`aistudio`、`termux`、`mpl-2-0`。这些是目标平台/接口描述，不代表所有真实能力已通过验收。

已有 `.github/workflows/build.yml`。首次推送后检查 Windows、Ubuntu GCC/Clang、sanitizer、fuzzer 和 bridge 作业的实际结果，再决定后续标签或发行包。尚未执行的远端 CI 不应写成通过。

## 二进制发行

- 当前版本应标为 development/prerelease，说明未完成的账号、平台、性能和能力验证。
- 上传与发行提交一致的 CPack ZIP/TGZ 及 SHA-256 文件；不要上传本机 `.tools/` 或 runtime 目录。
- MPL 覆盖的二进制应提供对应源码的获取方式。GitHub Release 应指向同一提交/标签的源码，并保留自动提供的源码归档；在发行说明中清楚列出对应标签/提交，不能只发二进制。
- 安装包保留 `LICENSE`、`NOTICE`、`THIRD_PARTY_NOTICES.md` 和依赖版权文件。浏览器及系统包不因本项目许可改变。
- 运行 `python scripts/check-package.py PATH_TO_ARCHIVE`，检查清单和摘要；另外检查真实发布附件不包含 Cookie 或个人数据。

本指南只是本项目的发布操作流程。MPL 条款以仓库完整许可及 [Mozilla 官方文本](https://www.mozilla.org/MPL/2.0/) 为准。
