# ais2api 控制台

由 Claude 编写的 Vue 3 / TypeScript / Vite 控制台，已整合为 ais2api 可选内嵌页面，保留 Fluent 风格与移动端布局。

构建、启动与管理密钥配置见 [WebUI 使用说明](../docs/webui.md)。生产环境由 C++ HTTP 服务提供页面，运行时不需要 Node.js。

开发需要 Node.js 20.19+（20.x）或 22.12+，推荐 22.12+：

```sh
npm ci
npm run dev
npm run typecheck
npm run build
```

开发服务器默认监听 http://localhost:5173，将 `/api/admin/` 代理到 http://127.0.0.1:18451。可通过 `AIS2API_URL` 环境变量指定后端地址。登录时后端地址留空。

`npm run preview` 只预览构建产物；如需操作独立后端，在登录页填写后端地址，并配置后端 `admin_origins`。

主要目录：`src/api` 管理 API 与日志 SSE 客户端，`src/views` 各管理页面，`src/components` Fluent 组件，`src/styles` 主题和控件样式。`embed.mjs` 在 CMake 构建时将 `dist/` 编码成 C++ 内嵌资源。

源码采用 MPL-2.0；Vue、Vue Router 及 Fluent System Icons 的 MIT 许可见 [THIRD_PARTY_LICENSES.txt](public/THIRD_PARTY_LICENSES.txt)。
