# TomatoServer API v1

Base URL: `https://127.0.0.1:8443`

认证方式：除 `POST /api/v1/auth/login`、`POST /api/v1/auth/register` 外，其他接口都需要请求头：

`Authorization: Bearer <token>`

## 0) 仅服务端：注册用户

客户端不提供注册入口。在服务器配置中设置非空的 `registerApiKey` 后，可用本地工具调用：

- `POST /api/v1/auth/register`
- 请求头（必填）：`X-Tomato-Register-Key: <与 config 中 registerApiKey 一致>`
- Body：

```json
{
  "username": "newuser",
  "password": "secret"
}
```

- 若 `registerApiKey` 为空字符串，则返回 `403`，表示未开放注册。
- 用户名已存在时返回 `409`。

## 1) 登录

- `POST /api/v1/auth/login`
- Body:

```json
{
  "username": "hesphoros",
  "password": "hesphoros"
}
```

- 200:

```json
{
  "token": "xxxx",
  "user": {
    "id": 1,
    "username": "hesphoros",
    "bio": "",
    "avatarMediaUrl": ""
  },
  "expiresInHours": 168
}
```

## 2) 好友

- `GET /api/v1/friends`
  - 返回当前用户好友列表；每项包含 `id`、`username`、`bio`、`avatarMediaUrl`（头像为相对路径时与媒体下载规则一致）

- `POST /api/v1/friends/add`
  - Body:
```json
{
  "username": "ruansiqi"
}
```
  - 自动建立双向好友关系并确保 direct 会话存在

## 3) 会话

- `GET /api/v1/conversations`
  - 返回 direct 会话与最后一条消息摘要（若存在）；`peer` 中含 `bio`、`avatarMediaUrl`

## 4) 消息

- `POST /api/v1/messages/send`
  - Body 示例（文本）：
```json
{
  "conversationId": 1,
  "msgType": "text",
  "contentText": "hello",
  "clientMsgId": "client-001"
}
```

  - Body 示例（图片/贴纸）：
```json
{
  "conversationId": 1,
  "msgType": "image",
  "mediaUrl": "/media/2026/04/abcd.webp",
  "clientMsgId": "client-002"
}
```

- `GET /api/v1/messages/sync?conversationId=1&afterId=0&limit=50`
  - 返回增量消息列表与 `nextAfterId`（已软删除的消息不会出现）

- `POST /api/v1/messages/clear`
  - 清空指定会话中全部消息（对双方生效，软删除）
  - Body：

```json
{
  "conversationId": 1
}
```

  - 响应含 `clearedCount`

## 4b) 个人资料

- `GET /api/v1/profile/me`：当前登录用户资料（`id`、`username`、`bio`、`avatarMediaUrl`）

- `PATCH /api/v1/profile/me`：更新自己的资料；Body 可只包含需要修改的字段：

```json
{
  "bio": "新的个性签名"
}
```

```json
{
  "avatarMediaUrl": "/media/2026/04/avatar.webp"
}
```

  - `avatarMediaUrl` 必须是当前用户已通过 `POST /api/v1/media/upload` 上传且记录在 `media_assets` 中的路径；传空字符串可清除头像。

- `GET /api/v1/users/{userId}`：查看好友（或本人）的公开资料；非好友且非本人时返回 `404`。

## 5) 媒体

- `POST /api/v1/media/upload`
  - `multipart/form-data`，字段名任意（服务端读取首个文件）
  - 支持：`png/jpg/jpeg/webp/gif`
  - 大小限制：默认 10MB（可配置）
  - 返回 `mediaUrl`

- `GET /media/{yyyy}/{mm}/{file}`
  - 受鉴权保护，需带 Bearer Token

## 错误码约定

- `400` 参数错误
- `401` token 缺失或无效
- `403` 会话不可访问
- `404` 资源不存在
- `500` 服务端异常
