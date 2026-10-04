"""Exercise the firmware web manager in an isolated, headless simulator."""

import argparse
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--port", type=int, default=18080)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65534:
        parser.error("port must be between 1024 and 65534")
    binary = args.binary.resolve(strict=True)
    base = f"http://127.0.0.1:{args.port}"
    for port in (args.port, args.port + 1):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    env = dict(os.environ)
    # Use only this run's schedules and fixtures, regardless of caller defaults.
    for key in list(env):
        if key.startswith("CROSSPOINT_SIM_"):
            del env[key]
    env.update(
        SDL_VIDEODRIVER="dummy",
        CROSSPOINT_SIM_HTTP_PORT=str(args.port),
        CROSSPOINT_SIM_INPUT_SCRIPT=(
            "2500:DOWN;2800:DOWN;3100:ENTER;"
            "4000:DOWN;4300:DOWN;4600:ENTER;20000:QUIT"
        ),
    )

    def request(path, method="GET", data=None):
        req = urllib.request.Request(base + path, data=data, method=method)
        if path.startswith("/api/") and data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=2) as response:
            body = response.read()
            if response.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
            return response.status, body

    with tempfile.TemporaryDirectory(prefix="crosspoint-web-") as directory:
        root = Path(directory)
        (root / "fs_" / "books").mkdir(parents=True)
        with (root / "simulator.log").open("w+") as log:
            process = subprocess.Popen([str(binary)], cwd=root, env=env,
                                       stdout=log, stderr=log)
            try:
                for _ in range(150):
                    if process.poll() is not None:
                        raise RuntimeError("simulator exited before starting HTTP")
                    try:
                        request("/api/status")
                        break
                    except urllib.error.URLError:
                        time.sleep(0.1)
                else:
                    raise RuntimeError("simulator did not start HTTP within 15 seconds")
                for path in ("/", "/files", "/settings", "/fonts"):
                    status, body = request(path)
                    assert status == 200 and b"<html" in body.lower(), path
                    print(f"PASS GET {path}")
                status, body = request("/api/settings")
                settings = json.loads(body)
                assert status == 200 and isinstance(settings, list) and settings
                assert all("key" in setting for setting in settings)
                print("PASS settings API")
                status, _ = request("/api/settings", "POST", b"{}")
                assert status == 200
                status, body = request("/api/crypto", "POST", b'{"op":"sha1","data":"YWJj"}')
                expected = base64.b64encode(hashlib.sha1(b"abc").digest()).decode()
                assert status == 200 and json.loads(body)["data"] == expected
                status, body = request("/api/crypto", "POST", b'{"op":"keygen"}')
                assert status == 200 and "unsupported in simulator" in json.loads(body)["error"]
                print("PASS JSON POST, host hash, unsupported key operation")
                status, _ = request("/books/smoke.txt", "PUT", b"simulator smoke\n")
                assert status in (200, 201, 204), status
                status, body = request("/books/smoke.txt")
                assert status == 200 and body == b"simulator smoke\n"
                print("PASS WebDAV PUT/GET")
                with socket.create_connection(("127.0.0.1", args.port + 1), timeout=2) as ws:
                    ws.sendall((
                        "GET / HTTP/1.1\r\n"
                        f"Host: 127.0.0.1:{args.port + 1}\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n"
                    ).encode())
                    headers = b""
                    while b"\r\n\r\n" not in headers:
                        chunk = ws.recv(4096)
                        assert chunk, "WebSocket closed before handshake"
                        headers += chunk
                    assert b"101 Switching Protocols" in headers
                    assert b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in headers
                print("PASS WebSocket handshake")
            except Exception:
                log.seek(0)
                print(log.read())
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
