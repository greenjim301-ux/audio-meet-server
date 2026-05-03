# audio-meet-server

A lightweight audio conferencing server built on PJMEDIA. Clients connect over TCP, exchange G.711 A-law RTP packets, and are mixed together via PJMEDIA's conference bridge. Each meeting has its own isolated bridge and clock.

---

## Dependencies

- **PJSIP / PJMEDIA** — installed to `/usr/local` (headers + static libs)
- **CMake ≥ 3.16**, **Ninja** (or Make)
- **g++ with C++17**
- System libs: `libasound`, `libssl`, `libcrypto`, `libuuid`, `libpthread`

---

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The binary is produced at `build/audio-meet-server`.

---

## Running the server

```
./build/audio-meet-server [options]
```

| Option                   | Default     | Description                                        |
| ------------------------ | ----------- | -------------------------------------------------- |
| `--port <n>`             | `8080`      | HTTP API listen port                               |
| `--listen-ip <ip>`       | `127.0.0.1` | IP address reported to clients for TCP connections |
| `--port-range-start <n>` | `20000`     | First port in the TCP participant pool             |
| `--port-range-size <n>`  | `1000`      | Number of ports in the pool                        |

**Example** — listen on port 8080, advertise the public IP `203.0.113.10` to clients, allocate participant ports from 30000–30099:

```bash
./build/audio-meet-server \
    --port 8080 \
    --listen-ip 203.0.113.10 \
    --port-range-start 30000 \
    --port-range-size 100
```

The server binds the HTTP API on `0.0.0.0:<port>` and allocates individual TCP ports for each active participant from the port pool.

---

## Audio format

| Property    | Value                                                             |
| ----------- | ----------------------------------------------------------------- |
| Codec       | G.711 A-law (PCMA, RTP payload type 8)                            |
| Sample rate | 8 000 Hz                                                          |
| Channels    | 1 (mono)                                                          |
| Frame size  | 20 ms → 160 samples → 320 bytes PCM16                             |
| TX batch    | 2 frames per RTP packet → 320-byte payload, 40 ms interval        |
| Transport   | TCP with a 2-byte big-endian length prefix before each RTP packet |

---

## HTTP API

All endpoints are HTTP GET. Every response is JSON with the shape:

```json
{ "code": <int>, "msg": "<string>", "data": <object|null> }
```

`code: 0` means success. Non-zero codes indicate errors (see each endpoint).

---

### `GET /createAudioMeet`

Create a new meeting. Returns the meeting ID.

**Response**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "meetId": "a3f8c1d2e4b56789" }
}
```

---

### `GET /addAudioMeetPart`

Add an **active** participant. The server opens a TCP listen port; the client must connect to it and then exchange RTP.

**Query parameters**

| Parameter | Required | Description                                  |
| --------- | -------- | -------------------------------------------- |
| `meetId`  | yes      | Meeting ID returned by `/createAudioMeet`    |
| `partId`  | yes      | Caller-chosen unique ID for this participant |

**Response (success)**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "ip": "203.0.113.10", "port": 30001 }
}
```

The client connects a TCP socket to `ip:port`, then:

- **Sends** RTP packets (G.711 A-law, payload type 8), each prefixed by a 2-byte big-endian length.
- **Receives** mixed audio in the same framing and format.

**Error codes**

| Code | Meaning                                 |
| ---- | --------------------------------------- |
| 1    | `meetId` not found                      |
| 2    | `partId` already exists in this meeting |
| 3    | No TCP ports available in the pool      |
| 4    | Missing or empty parameter              |

---

### `GET /addPassiveAudioMeetPart`

Add a **split** participant. The caller provides the TCP address where the server should **send** mixed audio (TX destination). The server simultaneously opens a new TCP listen port for **receiving** inbound audio from the caller and returns that port in the response.

The caller must:

1. Be listening on `ip:port` before making this request (the server connects immediately).
2. Connect to the returned `data.ip:data.port` to push its own audio stream to the server.

**Query parameters**

| Parameter | Required | Description                                          |
| --------- | -------- | ---------------------------------------------------- |
| `meetId`  | yes      | Meeting ID                                           |
| `partId`  | yes      | Unique participant ID                                |
| `ip`      | yes      | Remote IPv4 address the server connects outbound to  |
| `port`    | yes      | Remote TCP port the server connects to (TX, 1–65535) |

**Response (success)**

```json
{
  "code": 0,
  "msg": "success",
  "data": { "ip": "203.0.113.10", "port": 30005 }
}
```

`data.ip:data.port` is the server's RX listen port. The caller must connect to it and send RTP packets (same framing and codec as active participants). The server simultaneously streams mixed audio outbound to the caller's `ip:port`.

**Error codes**

| Code | Meaning                                          |
| ---- | ------------------------------------------------ |
| 1    | `meetId` not found                               |
| 2    | `partId` already exists in this meeting          |
| 3    | No TCP ports available in the pool (for RX port) |
| 4    | Missing, empty, or out-of-range parameter        |
| 5    | TCP connect to `ip:port` (TX side) failed        |

---

### `GET /destroyAudioMeet`

Stop and remove a meeting and all its participants.

**Query parameters**

| Parameter | Required | Description           |
| --------- | -------- | --------------------- |
| `meetId`  | yes      | Meeting ID to destroy |

**Response (success)**

```json
{ "code": 0, "msg": "success", "data": null }
```

**Error codes**

| Code | Meaning                    |
| ---- | -------------------------- |
| 1    | `meetId` not found         |
| 4    | Missing or empty parameter |

---

## Typical flow

```
# 1. Create a meeting
GET /createAudioMeet
→ meetId = "a3f8c1d2e4b56789"

# 2. Add participant A (server listens, client connects)
GET /addAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=alice
→ data.ip = "203.0.113.10", data.port = 30001
# → alice's client connects TCP to 203.0.113.10:30001

# 3. Add participant B the same way
GET /addAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=bob
→ data.ip = "203.0.113.10", data.port = 30002
# → bob's client connects TCP to 203.0.113.10:30002

# 4. Add a split participant (relay):
#    a) relay must be listening on 10.0.0.5:5004 (to receive mixed audio from server)
GET /addPassiveAudioMeetPart?meetId=a3f8c1d2e4b56789&partId=relay&ip=10.0.0.5&port=5004
→ data.ip = "203.0.113.10", data.port = 30003
#    b) relay then connects TCP to 203.0.113.10:30003 and sends its own RTP audio

# 5. Tear down when done
GET /destroyAudioMeet?meetId=a3f8c1d2e4b56789
```

Alice, Bob, and the relay all hear each other's mixed audio in real time.
