# ais2api 控制台

ais2api 的 Vue 3 / TypeScript / Vite 管理控制台，采用 Fluent 风格并适配移动端。构建时嵌入 ais2api 可执行文件，生产环境由 C++ HTTP 服务提供页面，运行时不需要 Node.js。

使用方法、页面说明与构建开关见 [Web 管理界面](../docs/webui.md)；它调用的接口见 [管理 API](../docs/management-api.md)。

## 开发

需要 Node.js 20.19+（20.x）或 22.12+，推荐 22.12+：

```sh
npm ci
npm run dev        # http://localhost:5173
npm run typecheck
npm test           # Vitest + happy-dom
npm run build
```

- 开发服务器把 `/api/admin/` 代理到 `http://127.0.0.1:18451`，可用 `AIS2API_URL` 指定其他后端；登录时后端地址留空。
- `npm run preview` 只预览构建产物。要连接独立后端，在登录页填写后端地址，并在后端配置 `admin_origins`。

## 目录

| 路径 | 内容 |
|---|---|
| `src/api` | 管理 API 与日志 SSE 客户端 |
| `src/views` | 各管理页面 |
| `src/components` | Fluent 风格组件 |
| `src/stores` | 会话、主题与服务状态 |
| `src/styles` | 主题和控件样式 |
| `embed.mjs` | CMake 构建时把 `dist/` 编码为 C++ 内嵌资源 |

源码采用 MPL-2.0；Vue、Vue Router 与 Fluent System Icons 的 MIT 许可见 [THIRD_PARTY_LICENSES.txt](public/THIRD_PARTY_LICENSES.txt)。
