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
- OpenSSL（生成本地 HTTPS 证书；命令行工具即可，用于签发 `dev-cert.pem`）

## vcpkg 依赖安装

仓库根目录已有 **`vcpkg.json`**（manifest），声明的包为：

| 包名 | 说明 |
|------|------|
| `drogon[sqlite3]` | HTTP 框架，并启用 sqlite3 相关能力 |
| `sqlite3` | SQLite（CMake 里为 `unofficial-sqlite3`） |
| `libsodium` | Argon2id 等（CMake 里为 `unofficial-sodium`） |

### 推荐：在 TomatoServer 根目录用 manifest 安装

先克隆/引导好 [vcpkg](https://github.com/microsoft/vcpkg)，设置环境变量 **`VCPKG_ROOT`** 指向 vcpkg 根目录。

**Windows（x64 示例）：** 在 `TomatoServer` 目录下执行：

```powershell
$env:VCPKG_ROOT = "D:\path\to\vcpkg"   # 按你的实际路径修改
& "$env:VCPKG_ROOT\bootstrap-vcpkg.bat"
& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet x64-windows
```

**Linux（x64 示例）：**

```bash
export VCPKG_ROOT=/path/to/vcpkg   # 按你的实际路径修改
"$VCPKG_ROOT/bootstrap-vcpkg.sh"
"$VCPKG_ROOT/vcpkg" install --triplet x64-linux
```

**macOS（Apple Silicon 示例）：**

```bash
export VCPKG_ROOT=/path/to/vcpkg
"$VCPKG_ROOT/bootstrap-vcpkg.sh"
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx
```

`vcpkg` 会读取当前目录的 **`vcpkg.json`**，自动安装其中列出的依赖（含 `drogon[sqlite3]`、`sqlite3`、`libsodium`）。

### 不配 manifest、手动指定包（等价）

若你希望在任意目录一条命令装齐（三元组请自行替换）：

```text
vcpkg install drogon[sqlite3]:x64-linux sqlite3:x64-linux libsodium:x64-linux
```

Windows 将 `x64-linux` 换成 `x64-windows` 等即可。

### CMake 指向 vcpkg 工具链

配置工程时加上（路径按本机修改）：

```text
-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
```

Linux / macOS：

```text
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

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


vcpkg install drogon[sqlite3]:x64-linux sqlite3:x64-linux libsodium:x64-linux