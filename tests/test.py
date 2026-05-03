"""
Integration test for audio-meet-server.

Flow:
  1. POST /createAudioMeet
  2. For each of two participants:
       a. GET /addAudioMeetPart  →  (audio_ip, audio_port)
       b. Start a local TCP listener on 127.0.0.1:0 (OS assigns free port)
       c. Call ZLM startSendRtp targeting that local port
       d. Accept ZLM's connection, then connect outbound to (audio_ip, audio_port)
       e. Relay ZLM → server unchanged (length-prefixed RTP frames)
       f. Receive mixed audio from server, strip 2-byte length + 12-byte RTP header,
          write raw payload to out_partN.bin
  3. Sleep 20 s
  4. GET /destroyAudioMeet, release all resources
"""

import socket
import threading
import time

import requests

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SERVER_API = "http://127.0.0.1:8080"
ZLM_API    = "http://127.0.0.1:11080/index/api"
SECRET     = "PCFL9DfWk9kuDhej5XcowCh67jvJ8tiC"

RTP_HEADER_SIZE = 12   # fixed minimum RTP header (no extension, no CSRC)
LOCAL_HOST      = "127.0.0.1"
ACCEPT_TIMEOUT  = 10   # seconds to wait for ZLM to connect back

# ---------------------------------------------------------------------------
# Server API helpers
# ---------------------------------------------------------------------------

def create_audio_meet() -> str:
    """Call /createAudioMeet and return the meetId."""
    resp = requests.get(f"{SERVER_API}/createAudioMeet", timeout=5)
    resp.raise_for_status()
    data = resp.json()
    if data.get("code") != 0:
        raise RuntimeError(f"/createAudioMeet error {data.get('code')}: {data.get('msg')}")
    meet_id = data["data"]["meetId"]
    print(f"[createAudioMeet] meetId={meet_id}")
    return meet_id


def add_audio_meet_part(meet_id: str, part_id: str) -> tuple[str, int]:
    """Call /addAudioMeetPart and return (ip, port)."""
    params = {"meetId": meet_id, "partId": part_id}
    resp = requests.get(f"{SERVER_API}/addAudioMeetPart", params=params, timeout=5)
    resp.raise_for_status()
    data = resp.json()
    if data.get("code") != 0:
        raise RuntimeError(f"/addAudioMeetPart error {data.get('code')}: {data.get('msg')}")
    ip   = data["data"]["ip"]
    port = data["data"]["port"]
    print(f"[addAudioMeetPart] partId={part_id}  audio_ip={ip}  audio_port={port}")
    return ip, port


def destroy_audio_meet(meet_id: str) -> None:
    """Call /destroyAudioMeet."""
    params = {"meetId": meet_id}
    resp = requests.get(f"{SERVER_API}/destroyAudioMeet", params=params, timeout=5)
    resp.raise_for_status()
    data = resp.json()
    if data.get("code") != 0:
        raise RuntimeError(f"/destroyAudioMeet error {data.get('code')}: {data.get('msg')}")
    print(f"[destroyAudioMeet] meetId={meet_id}  ok")


# ---------------------------------------------------------------------------
# ZLM API helper
# ---------------------------------------------------------------------------

def start_send_rtp(app: str, stream: str, dst_url: str, dst_port: int) -> None:
    """Call ZLM startSendRtp (TCP, audio-only, PT=8/PCMA)."""
    params = {
        "secret":    SECRET,
        "vhost":     "__defaultVhost__",
        "app":       app,
        "stream":    stream,
        "dst_url":   dst_url,
        "dst_port":  dst_port,
        "is_udp":    0,
        "pt":        8,
        "use_ps":    0,
        "only_audio": 1,
        "close_delay_ms": 10000,
        "ssrc": 12345678,
    }
    resp = requests.get(f"{ZLM_API}/startSendRtp", params=params, timeout=5)
    resp.raise_for_status()
    data = resp.json()
    if data.get("code") != 0:
        raise RuntimeError(f"startSendRtp error {data.get('code')}: {data.get('msg')}")
    print(f"[startSendRtp] app={app} stream={stream} → dst={dst_url}:{dst_port}")


# ---------------------------------------------------------------------------
# Low-level socket helpers
# ---------------------------------------------------------------------------

def _recvall(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes from sock; raises EOFError on connection close."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed")
        buf.extend(chunk)
    return bytes(buf)


def _sendall(sock: socket.socket, data: bytes) -> None:
    """Send all bytes to sock; raises OSError on failure."""
    sock.sendall(data)


# ---------------------------------------------------------------------------
# Relay thread functions
# ---------------------------------------------------------------------------

def relay_zlm_to_server(
    zlm_sock: socket.socket,
    server_sock: socket.socket,
    stop_event: threading.Event,
    label: str = "",
) -> None:
    """
    Forward length-prefixed RTP frames from ZLM to the audio-meet-server
    participant port.  Data is forwarded byte-for-byte (including the
    2-byte length prefix).
    """
    print(f"[relay_zlm_to_server] {label}started")
    recv_count = 0
    try:
        while not stop_event.is_set():
            # 2-byte big-endian length
            hdr = _recvall(zlm_sock, 2)
            pkt_len = int.from_bytes(hdr, "big")
            payload = _recvall(zlm_sock, pkt_len)
            _sendall(server_sock, hdr + payload)
            recv_count += 1
    except (EOFError, OSError) as exc:
        print(f"[relay_zlm_to_server] {label}stopped after {recv_count} pkts: {exc}")


def recv_from_server(
    server_sock: socket.socket,
    out_path: str,
    stop_event: threading.Event,
    label: str = "",
) -> None:
    """
    Receive length-prefixed RTP frames from the audio-meet-server participant
    port.  Strip the 2-byte length prefix and the 12-byte RTP fixed header;
    write the remaining audio payload to out_path.
    """
    print(f"[recv_from_server] {label}started → {out_path}")
    pkt_count = 0
    with open(out_path, "wb") as f:
        try:
            while not stop_event.is_set():
                hdr = _recvall(server_sock, 2)
                pkt_len = int.from_bytes(hdr, "big")
                pkt = _recvall(server_sock, pkt_len)
                # strip fixed RTP header
                audio = pkt[RTP_HEADER_SIZE:] if pkt_len > RTP_HEADER_SIZE else b""
                if audio:
                    f.write(audio)
                pkt_count += 1
        except (EOFError, OSError) as exc:
            print(f"[recv_from_server] {label}stopped after {pkt_count} pkts: {exc}")
    print(f"[recv_from_server] {label}done  total={pkt_count} pkts → {out_path}")


# ---------------------------------------------------------------------------
# ParticipantRelay
# ---------------------------------------------------------------------------

class ParticipantRelay:
    """
    Bridges ZLM → local TCP listener → audio-meet-server participant port.

    Typical usage:
        relay = ParticipantRelay("part1", audio_ip, audio_port, "out_part1.bin")
        relay.start_listener()               # binds local listener, sets self.local_port
        # tell ZLM to startSendRtp to 127.0.0.1:relay.local_port
        relay.accept_and_connect()           # blocks until ZLM connects
        relay.start_relay()                  # start forwarding threads
        ...
        relay.stop()                         # tear down
    """

    def __init__(
        self,
        label: str,
        audio_ip: str,
        audio_port: int,
        out_file: str,
    ) -> None:
        self.label      = label
        self.audio_ip   = audio_ip
        self.audio_port = audio_port
        self.out_file   = out_file

        self.local_port: int = 0

        self._listener_sock: socket.socket | None = None
        self._zlm_sock:      socket.socket | None = None   # ZLM's incoming connection
        self._server_sock:   socket.socket | None = None   # our connection to the server

        self._stop_event = threading.Event()
        self._relay_thread:  threading.Thread | None = None
        self._recv_thread:   threading.Thread | None = None

    # ------------------------------------------------------------------
    # Setup
    # ------------------------------------------------------------------

    def start_listener(self) -> int:
        """Bind a TCP listener on 127.0.0.1:0; return the OS-assigned port."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((LOCAL_HOST, 0))
        sock.listen(1)
        sock.settimeout(ACCEPT_TIMEOUT)
        self.local_port = sock.getsockname()[1]
        self._listener_sock = sock
        print(f"[{self.label}] listening on {LOCAL_HOST}:{self.local_port}")
        return self.local_port

    def accept_and_connect(self) -> None:
        """
        Accept ZLM's TCP connection, then connect outbound to
        (audio_ip, audio_port).  Intended to run in a daemon thread
        (so both participants can set up concurrently).
        """
        assert self._listener_sock is not None, "call start_listener() first"
        try:
            print(f"[{self.label}] waiting for ZLM connection …")
            zlm_conn, peer = self._listener_sock.accept()
            print(f"[{self.label}] ZLM connected from {peer}")
            self._zlm_sock = zlm_conn
        except socket.timeout:
            print(f"[{self.label}] accept timed out – ZLM did not connect")
            return
        finally:
            # We only need one connection; close the listener.
            try:
                self._listener_sock.close()
            except OSError:
                pass
            self._listener_sock = None

        # Connect outbound to the server participant port
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            srv.connect((self.audio_ip, self.audio_port))
        except OSError as exc:
            print(f"[{self.label}] connect to server failed: {exc}")
            zlm_conn.close()
            self._zlm_sock = None
            return
        self._server_sock = srv
        print(f"[{self.label}] connected to server {self.audio_ip}:{self.audio_port}")

    def start_relay(self) -> None:
        """Start the forwarding and receiving daemon threads."""
        if self._zlm_sock is None or self._server_sock is None:
            print(f"[{self.label}] sockets not ready – skip start_relay")
            return

        self._relay_thread = threading.Thread(
            target=relay_zlm_to_server,
            args=(self._zlm_sock, self._server_sock, self._stop_event),
            kwargs={"label": f"[{self.label}] "},
            daemon=True,
        )
        self._recv_thread = threading.Thread(
            target=recv_from_server,
            args=(self._server_sock, self.out_file, self._stop_event),
            kwargs={"label": f"[{self.label}] "},
            daemon=True,
        )
        self._relay_thread.start()
        self._recv_thread.start()

    # ------------------------------------------------------------------
    # Teardown
    # ------------------------------------------------------------------

    def stop(self) -> None:
        """Signal stop, close sockets (unblocks threads), join threads."""
        self._stop_event.set()

        for sock in (self._listener_sock, self._zlm_sock, self._server_sock):
            if sock is not None:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass

        for t in (self._relay_thread, self._recv_thread):
            if t is not None and t.is_alive():
                t.join(timeout=3)

        print(f"[{self.label}] stopped")


# ---------------------------------------------------------------------------
# Main test flow
# ---------------------------------------------------------------------------

def main() -> None:
    # 1. Create meeting
    meet_id = create_audio_meet()

    # 2. Add participant 1
    audio_ip1, audio_port1 = add_audio_meet_part(meet_id, "part1")
    relay1 = ParticipantRelay("part1", audio_ip1, audio_port1, "out_part1.bin")
    relay1.start_listener()

    # 3. Add participant 2
    audio_ip2, audio_port2 = add_audio_meet_part(meet_id, "part2")
    relay2 = ParticipantRelay("part2", audio_ip2, audio_port2, "out_part2.bin")
    relay2.start_listener()

    # 4. Add participant 3
    audio_ip3, audio_port3 = add_audio_meet_part(meet_id, "part3")
    relay3 = ParticipantRelay("part3", audio_ip3, audio_port3, "out_part3.bin")
    relay3.start_listener()

    # 5. Tell ZLM to start sending to our local listeners
    start_send_rtp("live", "test",  LOCAL_HOST, relay1.local_port)
    start_send_rtp("live", "test2", LOCAL_HOST, relay2.local_port)
    start_send_rtp("live", "test3", LOCAL_HOST, relay3.local_port)

    # 6. Accept ZLM connections and connect to server – run all in parallel
    t1 = threading.Thread(target=relay1.accept_and_connect, daemon=True)
    t2 = threading.Thread(target=relay2.accept_and_connect, daemon=True)
    t3 = threading.Thread(target=relay3.accept_and_connect, daemon=True)
    t1.start()
    t2.start()
    t3.start()
    t1.join(timeout=ACCEPT_TIMEOUT + 2)
    t2.join(timeout=ACCEPT_TIMEOUT + 2)
    t3.join(timeout=ACCEPT_TIMEOUT + 2)

    if relay1._zlm_sock is None or relay1._server_sock is None:
        raise RuntimeError("relay1 failed to establish connections")
    if relay2._zlm_sock is None or relay2._server_sock is None:
        raise RuntimeError("relay2 failed to establish connections")
    if relay3._zlm_sock is None or relay3._server_sock is None:
        raise RuntimeError("relay3 failed to establish connections")

    # 7. Start forwarding / receiving threads
    relay1.start_relay()
    relay2.start_relay()
    relay3.start_relay()

    # 8. Run for 30 seconds
    print("[main] running for 30 s …")
    time.sleep(30)

    # 9. Tear down
    print("[main] destroying meeting …")
    destroy_audio_meet(meet_id)

    relay1.stop()
    relay2.stop()
    relay3.stop()

    print("[main] done.  Output files: out_part1.bin  out_part2.bin  out_part3.bin")


if __name__ == "__main__":
    main()
