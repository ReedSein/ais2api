# 第三方软件及参考来源

ais2api 的自有实现采用 [MPL 2.0](LICENSE)。该许可不替换第三方软件已有的许可或版权声明。

## 构建依赖

桌面依赖的源码版本及摘要由 `vcpkg.json` 中固定的 baseline 和对应 port 锁定。主要依赖包括：

| 依赖 | 许可 |
|---|---|
| Boost.Asio / Beast / JSON 及 Boost 组件 | Boost Software License 1.0 |
| Catch2（开发测试） | Boost Software License 1.0 |
| libcurl | curl license |
| OpenSSL | Apache License 2.0 |
| SQLite | 上游 public-domain dedication |
| libarchive | 上游 BSD 及各组件对应条款 |
| zlib 等传递依赖 | 各自的上游许可 |

此表不是传递依赖的完整清单。桌面 CMake 安装和 CPack 流程会保留 vcpkg 提供的 `copyright` 文件，位置为 `share/licenses/ais2api-dependencies/`。修改依赖或分发方式时，应一并检查对应声明。Termux 系统依赖由其包管理器分发，不属于本仓库的 MPL 源码。

## 浏览器

CloakBrowser/Chromium 独立下载，不包含在后端源码仓库或当前后端 ZIP 内。其归档内的许可、版权和上游条款继续适用；本项目没有将浏览器改为 MPL 2.0。准备流程保留所下载归档中的文件。

## 原理参考

[aistudio-api-termux](https://github.com/ReedSein/aistudio-api-termux/tree/84b50663fcae79058fe33e0e45b5c3452da7c461) 用于理解行为和私有协议事实。其本身采用 MIT 许可，版权归其贡献者；该许可不因 ais2api 的许可选择而改变。

本仓库的 C++ 实现、网页桥接脚本及回归样例独立编写，不包含参考项目的 Python/JavaScript 实现文件或测试实现。本地 `.analysis/` 克隆、依赖 checkout 和生成文件不纳入 Git 提交。如果后续引入任何第三方源码，应逐文件保留其许可和版权声明，并更新本说明。
