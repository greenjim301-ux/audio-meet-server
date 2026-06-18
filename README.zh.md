# audio-meet-server

基于 PJMEDIA 的轻量级音频会议服务器。客户端通过 TCP 连接，使用 G.711 A-law RTP 数据包交换音频，通过 PJMEDIA 的混音桥对所有参与者进行混音。每个会议拥有独立的混音桥和时钟。

---

## 依赖

- **PJSIP / PJMEDIA** — 安装到 `/usr/local`（头文件 + 静态库）
- **CMake ≥ 3.16**、**Ninja**（或 Make）
- **g++ 支持 C++17**
- 系统库：`libasound`、`libssl`、`libcrypto`、`libuuid`、`libpthread`

---

## 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

编译产物位于 `build/audio-meet-server`。

---

## 运行服务器

```bash
./build/audio-meet-server [options]
```

### 命令行参数

| 选项                     | 默认值        | 说明                                    |
| ------------------------ | ------------- | --------------------------------------- |
| `--port <n>`             | `20080`       | HTTP API 监听端口                       |
| `--listen-ip <ip>`       | `10.20.30.21` | 向客户端广播的 IP 地址（用于 TCP 连接） |
| `--port-range-start <n>` | `20000`       | TCP 参与者端口池起始值                  |
| `--port-range-size <n>`  | `80`          | TCP 端口池大小                          |

### 示例

在端口 20080 监听，向客户端广播公网 IP `203.0.113.10`，分配 30000~30099 的参与者端口：

```bash
./build/audio-meet-server \
    --port 20080 \
    --listen-ip 203.0.113.10 \
    --port-range-start 30000 \
    --port-range-size 100
```

服务器在 `0.0.0.0:<port>` 上绑定 HTTP API，并从端口池中为每个活跃参与者分配独立的 TCP 端口。

---

## 音频格式

| 属性      | 值                                          |
| --------- | ------------------------------------------- |
| 编码      | G.711 A-law（PCMA，RTP 负载类型 8）         |
| 采样率    | 8000 Hz                                     |
| 声道数    | 1（单声道）                                 |
| 帧大小    | 20 ms → 160 个采样点 → 320 字节 PCM16       |
| TX 批处理 | 每个 RTP 包 2 帧 → 320 字节负载，40 ms 间隔 |
| 传输层    | TCP，每个 RTP 包前加 2 字节大端序长度前缀   |

---

## HTTP API

所有接口均为 HTTP GET。每个响应均为 JSON 格式：

```json
{ "code": <int>, "msg": "<string>", "data": <object|null> }
```

`code: 0` 表示成功。非零值表示错误（详见各接口说明）。

通用错误码：

| Code | 说明                            |
| ---- | ------------------------------- |
| 0    | 成功                            |
| 1    | `meetId` 未找到                 |
| 2    | 参与者相关错误（已存在/未找到） |
| 3    | 端口池无可用端口                |
| 4    | 缺少参数或参数无效              |
| 5    | 内部操作失败（如 TCP 连接失败） |

---

### `GET /ams/createAudioMeet`

创建新会议。返回会议 ID。

**请求参数**：无

**成功响应**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "meetId": "a3f8c1d2e4b56789" }
}
```

---

### `GET /ams/addAudioMeetPart?meetId=<>&partId=<>`

添加**主动模式**参与者。服务器打开一个 TCP 监听端口，客户端需要主动连接到该端口，然后双向交换 RTP 音频数据。

**请求参数**

| 参数     | 必填 | 说明                           |
| -------- | ---- | ------------------------------ |
| `meetId` | 是   | 会议 ID                        |
| `partId` | 是   | 调用方自行指定的参与者唯一标识 |

**成功响应**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "ip": "203.0.113.10", "port": 30001 }
}
```

客户端需通过 TCP 连接到 `data.ip:data.port`，然后：

- **发送** RTP 数据包（G.711 A-law，负载类型 8），每个包前加 2 字节大端序长度前缀
- **接收** 相同格式的混音后音频数据

**错误码**

| Code | 说明                           |
| ---- | ------------------------------ |
| 1    | `meetId` 未找到                |
| 2    | `partId` 在该会议中已存在      |
| 3    | 端口池无可用端口               |
| 4    | 缺少 `meetId` 或 `partId` 参数 |

---

### `GET /ams/addPassiveAudioMeetPart?meetId=<>&partId=<>&ip=<>&port=<>`

添加**分离模式**参与者。调用方提供目标地址，服务器主动连接该地址并发送混音音频；同时服务器打开一个新的 TCP 监听端口用于接收调用方的音频，该端口在响应中返回。

调用方需：

1. 在 `ip:port` 上提前监听（服务器收到请求后会立即发起连接）
2. 连接到响应返回的 `data.ip:data.port` 以发送自己的音频流

**请求参数**

| 参数     | 必填 | 说明                                           |
| -------- | ---- | ---------------------------------------------- |
| `meetId` | 是   | 会议 ID                                        |
| `partId` | 是   | 参与者唯一标识                                 |
| `ip`     | 是   | 服务器主动连接的远端 IPv4 地址（发送混音音频） |
| `port`   | 是   | 服务器主动连接的远端 TCP 端口（1~65535）       |

**成功响应**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "ip": "203.0.113.10", "port": 30005 }
}
```

`data.ip:data.port` 是服务器用于接收音频的监听端口。调用方需连接到该端口并发送 RTP 数据包（格式与主动模式相同）。服务器同时会将混音音频流式发送到调用方指定的 `ip:port`。

---

### `GET /ams/delAudioMeetPart?meetId=<>&partId=<>`

从会议中删除指定参与者，释放其占用的端口和资源。

**请求参数**

| 参数     | 必填 | 说明              |
| -------- | ---- | ----------------- |
| `meetId` | 是   | 会议 ID           |
| `partId` | 是   | 要删除的参与者 ID |

**成功响应**

```json
{ "code": 0, "msg": "success", "data": null }
```

**错误码**

| Code | 说明                           |
| ---- | ------------------------------ |
| 1    | `meetId` 未找到                |
| 2    | `partId` 未找到                |
| 4    | 缺少 `meetId` 或 `partId` 参数 |

---

### `GET /ams/muteAudioMeetPart?meetId=<>&partId=<>&mute=true|false`

静音或取消静音指定参与者的麦克风（接收方向）。静音后该参与者仍能听到其他人，但其他人听不到该参与者。

**请求参数**

| 参数     | 必填 | 说明                                  |
| -------- | ---- | ------------------------------------- |
| `meetId` | 是   | 会议 ID                               |
| `partId` | 是   | 参与者 ID                             |
| `mute`   | 是   | `true` 表示静音，`false` 表示取消静音 |

**成功响应**

```json
{ "code": 0, "msg": "success", "data": null }
```

**错误码**

| Code | 说明                                |
| ---- | ----------------------------------- |
| 1    | `meetId` 未找到                     |
| 2    | `partId` 未找到                     |
| 4    | 缺少参数或 `mute` 不是 `true/false` |

---

### `GET /ams/destroyAudioMeet?meetId=<>`

销毁会议，停止所有参与者线程，释放所有占用的端口。

**请求参数**

| 参数     | 必填 | 说明            |
| -------- | ---- | --------------- |
| `meetId` | 是   | 要销毁的会议 ID |

**成功响应**

```json
{ "code": 0, "msg": "success", "data": null }
```

**错误码**

| Code | 说明               |
| ---- | ------------------ |
| 1    | `meetId` 未找到    |
| 4    | 缺少 `meetId` 参数 |

---

## 参与者模式说明

### 主动模式（Active）

通过 `/ams/addAudioMeetPart` 添加。服务器在指定端口上监听 TCP 连接，客户端主动连接。连接建立后双向传输音频：

- 客户端 → 服务器：发送本端音频
- 服务器 → 客户端：发送混音后的音频

### 分离模式（Split）

通过 `/ams/addPassiveAudioMeetPart` 添加。服务器同时扮演主动和被动角色：

- **发送方向（TX）**：服务器主动连接客户端指定的 `ip:port`，将混音音频推送给客户端
- **接收方向（RX）**：服务器在分配的新端口上监听，等待客户端连接并发送本端音频

---

## 典型流程

```
# 1. 创建会议
GET /ams/createAudioMeet
→ meetId = "a3f8c1d2e4b56789"

# 2. 添加参与者 A（主动模式，服务器监听，客户端连接）
GET /ams/addAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=alice
→ data.ip = "203.0.113.10", data.port = 30001
# → alice 的客户端通过 TCP 连接到 203.0.113.10:30001

# 3. 添加参与者 B
GET /ams/addAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=bob
→ data.ip = "203.0.113.10", data.port = 30002
# → bob 的客户端通过 TCP 连接到 203.0.113.10:30002

# 4. 添加分离模式参与者（中继）：
#    a) 中继需在 10.0.0.5:5004 上提前监听（用于接收服务器的混音音频）
GET /ams/addPassiveAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=relay&ip=10.0.0.5&port=5004
→ data.ip = "203.0.113.10", data.port = 30003
#    b) 中继通过 TCP 连接到 203.0.113.10:30003 并发送自己的 RTP 音频

# 5. 静音参与者 B（其他人听不到 B）
GET /ams/muteAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=bob&mute=true

# 6. 删除参与者 A
GET /ams/delAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=alice

# 7. 结束会议
GET /ams/destroyAudioMeet?meetId=a3f8c1d2e4b56789
```

Alice、Bob 和中继都能实时听到彼此的混音音频。

---

## 项目结构

```
├── CMakeLists.txt              # CMake 构建配置
├── src/
│   ├── main.cpp                # 入口、命令行解析
│   ├── server.h / server.cpp   # HTTP API 路由注册
│   ├── meeting_manager.h / .cpp # 会议管理器（创建/销毁/管理会议）
│   ├── meeting.h / meeting.cpp  # 会议（混音桥、参与者管理）
│   ├── participant.h / .cpp     # 参与者（TCP 连接、音频收发线程）
│   ├── rtp_packet.h / .cpp      # RTP 数据包序列化/解析
│   ├── port_allocator.h / .cpp  # 线程安全端口池
│   ├── pjmedia_ctx.h / .cpp     # PJMEDIA 生命周期管理
│   ├── g711.h                   # G.711 A-law 编解码
├── tests/
│   ├── test.py                  # Python 集成测试（基于 ZLM）
│   ├── test.html                # 浏览器测试页面（基于 WHIP/WHEP）
├── third_party/
│   ├── include/
│   │   ├── httplib.h            # cpp-httplib
│   │   └── nlohmann/            # nlohmann/json
```
