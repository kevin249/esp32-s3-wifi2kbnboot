"""双逻辑盘服务：只读 ISO 光驱、可写文件盘、服务器文件浏览和设备控制代理。"""
from __future__ import annotations
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import threading
import urllib.error
import urllib.request
from urllib.parse import parse_qs, urlsplit

from .images import SnapshotStore, safe_name
from .transfer import TransferDisk, OwnershipError


class DiskServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, store, transfer, token, library, device_url="", max_upload=128 * 1024 * 1024):
        super().__init__(address, Handler)
        self.store, self.transfer, self.token = store, transfer, token
        self.library = Path(library).resolve()
        self.library.mkdir(parents=True, exist_ok=True)
        self.device_url, self.max_upload = device_url.rstrip("/"), max_upload
        self.control_lock = threading.RLock()
        self.refresh = secrets.randbits(30)
        self.iso_present = store.current is not None

    def manifest(self):
        with self.control_lock:
            return {"iso": self.store.manifest() if self.iso_present else None,
                    "transfer": self.transfer.info(), "refresh": self.refresh, "block_size": 16384}

    def refresh_usb(self):
        with self.control_lock:
            self.refresh = (self.refresh + 1) & 0xFFFFFFFF

    def library_path(self, name):
        path = (self.library / name).resolve()
        if not path.is_relative_to(self.library):
            raise ValueError("路径超出 ISO 目录")
        return path


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def log_message(self, fmt, *args):
        pass

    def send_data(self, code, data, content_type="application/json; charset=utf-8", **headers):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if self.close_connection: self.send_header("Connection", "close")
        for name, value in headers.items(): self.send_header(name.replace("_", "-"), str(value))
        self.end_headers()
        self.wfile.write(data)

    def json_response(self, code, body):
        self.send_data(code, json.dumps(body, ensure_ascii=False).encode("utf-8"))

    def authorized(self):
        if secrets.compare_digest(self.headers.get("Authorization", "").encode("utf-8"),
                                  f"Bearer {self.server.token}".encode("utf-8")):
            return True
        self.close_connection = True
        self.json_response(401, {"error": "访问令牌无效"})
        return False

    def handle_error(self, exc):
        self.close_connection = True
        if isinstance(exc, OwnershipError): self.json_response(409, {"error": str(exc)})
        elif isinstance(exc, FileNotFoundError): self.json_response(404, {"error": "文件不存在"})
        elif isinstance(exc, (ValueError, KeyError, IndexError, TypeError)): self.json_response(400, {"error": str(exc)})
        else: self.json_response(500, {"error": "磁盘或设备操作失败"})

    def body(self, limit):
        if self.headers.get("Transfer-Encoding"):
            raise ValueError("需要明确的 Content-Length")
        size = int(self.headers.get("Content-Length", "-1"))
        if not 0 <= size <= limit: raise ValueError("请求体长度无效或超过上限")
        data = self.rfile.read(size)
        if len(data) != size: raise ValueError("请求体不完整")
        return data

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/":
            self.send_data(200, Path(__file__).with_name("index.html").read_bytes(), "text/html; charset=utf-8")
            return
        if not self.authorized(): return
        try:
            query = parse_qs(url.query, strict_parsing=True)
            if url.path == "/api/manifest":
                self.json_response(200, self.server.manifest())
            elif url.path == "/api/library":
                path = self.server.library_path(query.get("path", [""])[0])
                items = [{"name": p.name, "path": p.relative_to(self.server.library).as_posix(), "directory": p.is_dir()}
                         for p in path.iterdir() if not p.is_symlink() and (p.is_dir() or p.suffix.lower() == ".iso")]
                self.json_response(200, {"path": path.relative_to(self.server.library).as_posix(),
                                         "items": sorted(items, key=lambda p: (not p["directory"], p["name"]))})
            elif url.path == "/api/block":
                version, offset, length = query["version"][0], int(query["offset"][0]), int(query["length"][0])
                if query.get("lun", ["0"])[0] == "0":
                    data = self.server.store.read_block(version, offset, length)
                elif query["lun"][0] == "1":
                    data = self.server.transfer.read(self.headers.get("X-Session", ""), version, offset, length)
                else: raise ValueError("逻辑盘编号无效")
                self.send_data(200, data, "application/octet-stream", X_Image_Version=version, X_Offset=offset)
            elif url.path == "/api/transfer/files":
                self.json_response(200, {"files": self.server.transfer.list_files()})
            elif url.path == "/api/transfer/download":
                name = safe_name(query["name"][0])
                with tempfile.TemporaryDirectory(dir=self.server.store.root) as directory:
                    target = Path(directory) / "download"
                    self.server.transfer.download(name, target)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(target.stat().st_size))
                    self.end_headers()
                    with target.open("rb") as stream: shutil.copyfileobj(stream, self.wfile, 65536)
            else: self.json_response(404, {"error": "接口不存在"})
        except (ValueError, KeyError, IndexError, TypeError, OSError, OwnershipError, subprocess.TimeoutExpired) as exc:
            self.handle_error(exc)

    def do_POST(self):
        if not self.authorized(): return
        url = urlsplit(self.path)
        try:
            query = parse_qs(url.query, strict_parsing=True)
            path = url.path
            if path == "/api/transfer/upload":
                name = safe_name(query["name"][0])
                if self.headers.get("Transfer-Encoding"): raise ValueError("需要 Content-Length")
                length = int(self.headers.get("Content-Length", "-1"))
                if not 0 < length <= self.server.max_upload:
                    self.close_connection = True
                    self.json_response(413, {"error": "文件为空或超过上传上限"})
                    return
                with tempfile.TemporaryDirectory(dir=self.server.store.root) as directory:
                    source = Path(directory) / "upload"
                    with source.open("wb") as stream:
                        remaining = length
                        while remaining:
                            data = self.rfile.read(min(remaining, 65536))
                            if not data: raise ValueError("上传中断")
                            stream.write(data)
                            remaining -= len(data)
                    result = self.server.transfer.upload(source, name)
                self.json_response(200, result)
                return
            data = self.body(65536 if path == "/api/transfer/write" else 1024)
            if path == "/api/mount":
                body = json.loads(data)
                image = self.server.library_path(body["path"])
                if not image.is_file() or image.suffix.lower() != ".iso": raise ValueError("请选择 ISO 文件")
                self.server.store.import_image(image)
                self.server.iso_present = True
                self.server.refresh_usb()
                result = self.server.manifest()
            elif path == "/api/eject":
                self.server.iso_present = False
                self.server.refresh_usb()
                result = self.server.manifest()
            elif path == "/api/activate":
                self.server.refresh_usb()
                result = self.server.manifest()
            elif path == "/api/transfer/claim":
                result = self.server.transfer.claim(json.loads(data)["device"])
            elif path == "/api/transfer/release":
                result = self.server.transfer.release(self.headers.get("X-Session", ""))
            elif path == "/api/transfer/take":
                result = self.server.transfer.request_server()
            elif path == "/api/transfer/attach":
                result = self.server.transfer.attach()
            elif path == "/api/transfer/write":
                result = self.server.transfer.write(self.headers.get("X-Session", ""), query["version"][0],
                                                    int(query["offset"][0]), data)
            elif path in ("/api/hid/keyboard", "/api/hid/mouse", "/api/control/power"):
                if not self.server.device_url: raise ValueError("请先通过 --device-url 指定 ESP32-S3 地址")
                request = urllib.request.Request(self.server.device_url + path.removeprefix("/api"), data=data,
                    headers={"Authorization": f"Bearer {self.server.token}", "Content-Type": "application/json"}, method="POST")
                try:
                    with urllib.request.urlopen(request, timeout=5) as response:
                        result = json.load(response)
                except urllib.error.HTTPError as exc:
                    self.json_response(exc.code, {"error": "设备拒绝指令，请检查GPIO是否启用或指令队列是否已满"})
                    return
            else:
                self.json_response(404, {"error": "接口不存在"})
                return
            self.json_response(200, result)
        except (ValueError, KeyError, IndexError, TypeError, OSError, OwnershipError, subprocess.TimeoutExpired) as exc:
            self.handle_error(exc)


def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 双逻辑盘与键鼠、电源控制服务")
    parser.add_argument("--data", type=Path, default=Path("data"))
    parser.add_argument("--library", type=Path, default=Path("isos"), help="网页可以浏览的服务器 ISO 根目录")
    parser.add_argument("--image", type=Path, help="可选，首次启动时挂载的 ISO")
    parser.add_argument("--device-url", default="", help="例如 http://192.168.1.20，用于代理键鼠与主板按键")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--file-disk-mib", type=int, default=128)
    parser.add_argument("--max-upload-mib", type=int, default=128)
    args = parser.parse_args()
    token = os.environ.get("WIFI_DISK_TOKEN", "")
    if not 16 <= len(token) <= 240 or not token.isascii() or any(ord(c) <= 32 or ord(c) >= 127 for c in token):
        parser.error("WIFI_DISK_TOKEN 必须为16～240个可见ASCII字符")
    if args.max_upload_mib <= 0: parser.error("上传上限必须大于零")
    if args.device_url:
        url = urlsplit(args.device_url)
        if url.scheme not in ("http", "https") or not url.netloc or url.path not in ("", "/") or url.username:
            parser.error("设备地址必须是 http(s)://主机[:端口]")
    store = SnapshotStore(args.data)
    if args.image and store.current is None: store.import_image(args.image)
    transfer = TransferDisk(store.root / "transfer.img", args.file_disk_mib)
    server = DiskServer((args.host, args.port), store, transfer, token, args.library,
                        args.device_url, args.max_upload_mib * 1024 * 1024)
    print(f"管理页面：http://{args.host}:{server.server_port}/", flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()


if __name__ == "__main__": main()
