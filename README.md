# TomatoServer

TomatoServer 是基于 Drogon + SQLite 的聊天后端（Jetchat 场景）。

## 功能清单

- 登录（argon2id 密码验证 + token 认证）
- 好友列表、添加好友、自动建立 direct 会话
- 会话列表（附最后一条消息摘要）
- 消息发送与增量同步（text/image/sticker）
- 媒体上传、落盘、鉴权下载

## 前置依赖

- CMake 3.21+
- vcpkg（并设置 `VCPKG_ROOT`）
- OpenSSL（生成本地 HTTPS 证书）

## 配置说明

配置文件：`config/server.dev.json`

关键字段：

- `bindAddress` / `httpsPort`：服务监听地址
- `tlsCertPath` / `tlsKeyPath`：HTTPS 证书与私钥路径
- `sqlitePath`：SQLite 文件路径
- `migrationsDir`：迁移脚本目录
- `mediaRootDir`：媒体文件落盘根目录
- `maxUploadBytes`：上传大小限制（默认 10MB）
- `tokenTtlHours`：登录 token 过期时间（小时）

## 本地开发启动

1) 生成开发证书（首次）：

```powershell
openssl req -x509 -nodes -newkey rsa:2048 `
  -keyout "config/dev-key.pem" `
  -out "config/dev-cert.pem" `
  -days 365 `
  -config "config/openssl.cnf"
```

2) 一键构建并启动：

```powershell
./scripts/dev-run.ps1
```

服务默认监听 `https://0.0.0.0:8443`。

## API 文档

详见：`docs/API.md`

## 内置测试数据

启动时自动迁移并确保以下账户存在：

- `hesphoros / hesphoros`
- `ruansiqi / ruansiqi`

并自动建立双向好友关系与 direct 会话。