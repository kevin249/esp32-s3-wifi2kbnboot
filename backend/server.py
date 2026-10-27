"""只读扇区服务，以及通过不可变快照向 Ventoy 数据分区添加文件的服务。"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SECTOR = 512
BLOCK = 16 * 1024
MAX_READ = 64 * 1024
VERSION_RE = re.compile(r"[0-9a-f]{64}\Z")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def first_partition(path: Path) -> tuple[int, int]:
    """返回第一分区的字节偏移、长度；接受普通 MBR 和 GPT Ventoy 镜像。"""
    size = path.stat().st_size
    with path.open("rb") as stream:
        mbr = stream.read(SECTOR)
        if len(mbr) != SECTOR or mbr[510:512] != b"\x55\xaa":
            raise ValueError("镜像缺少有效 MBR 签名")
        start, sectors = struct.unpack_from("<II", mbr, 454)
        if mbr[450] == 0xEE:
            stream.seek(SECTOR)
            header = stream.read(SECTOR)
            if header[:8] != b"EFI PART":
                raise ValueError("GPT 头无效")
            table_lba = struct.unpack_from("<Q", header, 72)[0]
            count, entry_size = struct.unpack_from("<II", header, 80)
            if not count or entry_size < 128 or entry_size > 4096:
                raise ValueError("GPT 分区表参数无效")
            stream.seek(table_lba * SECTOR)
            entry = stream.read(entry_size)
            if len(entry) != entry_size or entry[:16] == bytes(16):
                raise ValueError("GPT 第一分区不存在")
            start, end = struct.unpack_from("<QQ", entry, 32)
            sectors = end - start + 1
        offset, length = start * SECTOR, sectors * SECTOR
        if start < 1 or length <= 0 or offset + length > size:
            raise ValueError("第一分区越界")
        return offset, length


def add_fat_file(image: Path, source: Path, name: str) -> None:
    """只修改尚未发布的副本；mtools 负责 FAT32 分配与目录更新。"""
    offset, length = first_partition(image)
    with image.open("rb") as stream:
        stream.seek(offset)
        bpb = stream.read(SECTOR)
    if (bpb[82:90] != b"FAT32   " or bpb[510:512] != b"\x55\xaa"
            or struct.unpack_from("<H", bpb, 11)[0] != SECTOR
            or struct.unpack_from("<I", bpb, 32)[0] * SECTOR > length):
        raise ValueError("在线添加文件要求 Ventoy 第一分区为 FAT32；exFAT 镜像仅能读取")
    for command in ("mcopy", "mmd", "mdir"):
        if not shutil.which(command):
            raise ValueError("缺少 mtools，请先安装 mtools 后再上传文件")
    target = f"{image}@@{offset}"
    env = dict(os.environ, MTOOLS_SKIP_CHECK="0")

    def run(*args: str, check: bool = True) -> subprocess.CompletedProcess:
        result = subprocess.run(args, capture_output=True, timeout=120, env=env)
        if check and result.returncode:
            raise ValueError("mtools 操作失败：" + result.stderr.decode("utf-8", "replace")[-500:])
        return result

    if run("mdir", "-i", target, "::/BIOS", check=False).returncode:
        run("mmd", "-i", target, "::/BIOS")
    run("mcopy", "-o", "-i", target, str(source), f"::/BIOS/{name}")


def safe_name(name: str) -> str:
    if (not name or len(name.encode("utf-8")) > 180 or name in (".", "..")
            or name.startswith(".") or name.endswith((".", " "))
            or any(ord(c) < 32 or c in '/\\:*?"<>|%' for c in name)):
        raise ValueError("文件名无效；只能提供普通文件名，不能包含路径或 FAT 通配符")
    return name


class SnapshotStore:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.images = self.root / "images"
        self.images.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.manifest_path = self.root / "current.json"
        self.current = json.loads(self.manifest_path.read_text(encoding="utf-8")) if self.manifest_path.exists() else None
        if self.current:
            path = self.image_path(self.current["version"])
            if path.stat().st_size != self.current["size"] or digest(path) != self.current["version"]:
                raise ValueError("当前快照校验失败，请恢复原镜像；不能原地编辑已发布镜像")

    def image_path(self, version: str) -> Path:
        if not VERSION_RE.fullmatch(version):
            raise ValueError("镜像版本格式错误")
        return self.images / f"{version}.img"

    def manifest(self) -> dict:
        with self.lock:
            if self.current is None:
                raise ValueError("尚未导入磁盘镜像")
            return dict(self.current)

    def _save(self, manifest: dict) -> dict:
        fd, temp = tempfile.mkstemp(dir=self.root, prefix="manifest-")
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
                json.dump(manifest, stream, ensure_ascii=False)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temp, self.manifest_path)
        finally:
            if os.path.exists(temp):
                os.unlink(temp)
        self.current = manifest
        return dict(manifest)

    def _publish(self, stage: Path, activate: bool) -> dict:
        size = stage.stat().st_size
        if size < SECTOR or size % SECTOR or size // SECTOR > 0xFFFFFFFF:
            raise ValueError("磁盘大小必须为 512 字节整数倍，且不超过 READ10 的寻址范围")
        version = digest(stage)
        destination = self.image_path(version)
        with stage.open("r+b") as stream:
            os.fsync(stream.fileno())
        if destination.exists():
            stage.unlink()
        else:
            os.replace(stage, destination)
        previous = self.current or {}
        return self._save({
            "version": version, "size": size, "sector_size": SECTOR,
            "block_size": BLOCK,
            "generation": previous.get("generation", 0) + 1,
            "refresh": previous.get("refresh", 0) + int(activate),
        })

    def import_image(self, image: Path, activate: bool = False) -> dict:
        with self.lock:
            fd, name = tempfile.mkstemp(dir=self.root, prefix="stage-", suffix=".img")
            os.close(fd)
            stage = Path(name)
            try:
                shutil.copyfile(image, stage)
                return self._publish(stage, activate)
            finally:
                stage.unlink(missing_ok=True)

    def add_file(self, source: Path, name: str, activate: bool = False, mutator=add_fat_file) -> dict:
        name = safe_name(name)
        with self.lock:
            active = self.manifest()
            fd, temporary = tempfile.mkstemp(dir=self.root, prefix="stage-", suffix=".img")
            os.close(fd)
            stage = Path(temporary)
            try:
                shutil.copyfile(self.image_path(active["version"]), stage)
                mutator(stage, source, name)
                if stage.stat().st_size != active["size"]:
                    raise ValueError("添加文件时镜像大小发生变化，拒绝发布")
                return self._publish(stage, activate)
            finally:
                stage.unlink(missing_ok=True)

    def activate(self) -> dict:
        with self.lock:
            manifest = self.manifest()
            manifest["refresh"] += 1
            return self._save(manifest)

    def read_block(self, version: str, offset: int, length: int) -> bytes:
        path = self.image_path(version)
        if offset < 0 or offset % SECTOR or length <= 0 or length > MAX_READ or length % SECTOR:
            raise ValueError("读取必须按 512 字节对齐，长度不超过 64 KiB")
        with path.open("rb") as stream:
            if offset + length > os.fstat(stream.fileno()).st_size:
                raise ValueError("读取越过磁盘末尾")
            stream.seek(offset)
            data = stream.read(length)
        if len(data) != length:
            raise OSError("镜像短读，拒绝返回不完整数据")
        return data


class DiskServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, store: SnapshotStore, token: str, max_upload: int):
        super().__init__(address, Handler)
        self.store, self.token, self.max_upload = store, token, max_upload


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def log_message(self, fmt, *args):
        # 不记录 URL，避免文件名与请求参数进入日志。
        pass

    def send_data(self, code: int, data: bytes, content_type="application/json; charset=utf-8", **headers):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), str(value))
        self.end_headers()
        self.wfile.write(data)

    def json_response(self, code, body):
        self.send_data(code, json.dumps(body, ensure_ascii=False).encode("utf-8"))

    def authorized(self):
        if hmac.compare_digest(self.headers.get("Authorization", "").encode("utf-8"), f"Bearer {self.server.token}".encode("utf-8")):
            return True
        self.close_connection = True
        self.json_response(401, {"error": "访问令牌无效"})
        return False

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/":
            self.send_data(200, Path(__file__).with_name("index.html").read_bytes(), "text/html; charset=utf-8")
            return
        if not self.authorized():
            return
        try:
            if url.path == "/api/manifest":
                self.json_response(200, self.server.store.manifest())
            elif url.path == "/api/block":
                query = parse_qs(url.query, strict_parsing=True)
                version = query["version"][0]
                offset, length = int(query["offset"][0]), int(query["length"][0])
                data = self.server.store.read_block(version, offset, length)
                self.send_data(200, data, "application/octet-stream", X_Image_Version=version, X_Offset=offset)
            else:
                self.json_response(404, {"error": "接口不存在"})
        except FileNotFoundError:
            self.json_response(404, {"error": "镜像版本不存在"})
        except (ValueError, KeyError, IndexError) as exc:
            self.json_response(400, {"error": str(exc)})
        except OSError:
            self.json_response(500, {"error": "磁盘镜像读取失败"})

    def do_POST(self):
        if not self.authorized():
            return
        url = urlsplit(self.path)
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("上传需要明确的 Content-Length")
            length = int(self.headers.get("Content-Length", "-1"))
            if length < 0:
                raise ValueError("缺少 Content-Length")
            if url.path == "/api/activate":
                if length:
                    raise ValueError("刷新请求不能包含请求体")
                self.json_response(200, self.server.store.activate())
            elif url.path == "/api/files":
                if length == 0 or length > self.server.max_upload:
                    self.close_connection = True
                    self.json_response(413, {"error": "文件为空或超过上传大小上限"})
                    return
                query = parse_qs(url.query, strict_parsing=True)
                name = safe_name(query["name"][0])
                with tempfile.TemporaryDirectory(dir=self.server.store.root, prefix="upload-") as folder:
                    source = Path(folder) / "payload"
                    with source.open("wb") as stream:
                        remaining = length
                        while remaining:
                            part = self.rfile.read(min(remaining, 64 * 1024))
                            if not part:
                                raise ValueError("上传中断，未发布镜像")
                            stream.write(part)
                            remaining -= len(part)
                    result = self.server.store.add_file(source, name, query.get("activate", ["0"])[0] == "1")
                self.json_response(200, result)
            else:
                self.close_connection = True
                self.json_response(404, {"error": "接口不存在"})
        except (ValueError, KeyError, IndexError) as exc:
            self.close_connection = True
            self.json_response(400, {"error": str(exc)})
        except (OSError, subprocess.TimeoutExpired):
            self.close_connection = True
            self.json_response(500, {"error": "上传或镜像发布失败，旧版本保持可用"})


def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 Wi-Fi 虚拟磁盘服务")
    parser.add_argument("--data", type=Path, default=Path("data"))
    parser.add_argument("--image", type=Path, help="首次启动时导入的完整 Ventoy 磁盘镜像")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--max-upload-mib", type=int, default=128)
    args = parser.parse_args()
    token = os.environ.get("WIFI_DISK_TOKEN", "")
    if not token.isascii() or not 16 <= len(token) <= 240 or any(ord(c) <= 32 or ord(c) >= 127 for c in token):
        parser.error("请设置 16～240 个可见 ASCII 字符的 WIFI_DISK_TOKEN 环境变量")
    if args.max_upload_mib <= 0:
        parser.error("上传上限必须大于零")
    store = SnapshotStore(args.data)
    if store.current is None:
        if not args.image:
            parser.error("首次启动必须通过 --image 指定完整磁盘镜像")
        store.import_image(args.image)
    server = DiskServer((args.host, args.port), store, token, args.max_upload_mib * 1024 * 1024)
    print(f"服务已启动：http://{args.host}:{server.server_port}；管理页面为 /", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
