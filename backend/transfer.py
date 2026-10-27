"""可写文件盘及互斥使用权：USB 主机与后端文件工具不能同时访问文件系统。"""
from __future__ import annotations

import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading


class OwnershipError(Exception):
    """当前使用权不允许此操作。"""


class TransferDisk:
    def __init__(self, image: Path, size_mib: int = 128):
        self.image = image.resolve()
        self.image.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.version = secrets.token_hex(32)
        self.mode = "host"
        self.owner = self.session = None
        if not self.image.exists():
            if not 32 <= size_mib <= 1024:
                raise ValueError("文件盘容量需为32～1024MiB")
            if not shutil.which("mformat"):
                raise ValueError("首次启动需要 mtools，服务会自动创建普通 FAT 文件盘")
            stage = self.image.with_suffix(".creating")
            try:
                subprocess.run(["mformat", "-i", str(stage), "-C", "-T", str(size_mib * 2048),
                                "-h", "64", "-s", "32", "-v", "W2BFILES", "::"],
                               check=True, capture_output=True, timeout=120)
                os.replace(stage, self.image)
            finally:
                stage.unlink(missing_ok=True)
        self.size = self.image.stat().st_size
        if not self.size or self.size % 512 or self.size // 512 > 0xFFFFFFFF:
            raise ValueError("文件盘容量无效")

    def info(self):
        with self.lock:
            return {"version": self.version, "size": self.size, "sector_size": 512,
                    "mode": self.mode, "owned": self.owner is not None}

    def claim(self, device: str):
        if not device or len(device) > 64 or not device.isascii():
            raise ValueError("设备标识无效")
        with self.lock:
            if self.mode != "host" or (self.owner is not None and self.owner != device):
                raise OwnershipError("文件盘暂时不能交给该设备")
            self.owner = device
            if self.session is None:
                self.session = secrets.token_hex(32)
            return {"session": self.session, **self.info()}

    def _check(self, session: str, version: str):
        # mode 变成 server 时仍允许已在途主机 I/O 完成，直到设备明确 release。
        if not self.session or not secrets.compare_digest(self.session, session) or version != self.version:
            raise OwnershipError("文件盘会话已失效，需要重新挂载")

    def release(self, session: str):
        with self.lock:
            if self.session is None:
                self.mode = "server"
                return self.info()
            self._check(session, self.version)
            self.owner = self.session = None
            self.mode = "server"
            return self.info()

    def request_server(self):
        with self.lock:
            self.mode = "server"
            return self.info()

    def attach(self):
        with self.lock:
            if self.owner is not None:
                raise OwnershipError("设备尚未释放文件盘")
            self.mode = "host"
            self.version = secrets.token_hex(32)
            return self.info()

    def _range(self, offset: int, length: int):
        if offset < 0 or offset % 512 or not 0 < length <= 65536 or length % 512 or offset + length > self.size:
            raise ValueError("文件盘读写范围无效")

    def read(self, session: str, version: str, offset: int, length: int):
        with self.lock:
            self._check(session, version)
            self._range(offset, length)
            with self.image.open("rb") as stream:
                stream.seek(offset)
                data = stream.read(length)
            if len(data) != length:
                raise OSError("文件盘短读")
            return data

    def write(self, session: str, version: str, offset: int, data: bytes):
        with self.lock:
            self._check(session, version)
            self._range(offset, len(data))
            with self.image.open("r+b") as stream:
                stream.seek(offset)
                if stream.write(data) != len(data):
                    raise OSError("文件盘短写")
                stream.flush()
                os.fsync(stream.fileno())
            return {"written": len(data), "version": version, "offset": offset}

    def _server_only(self):
        if self.mode != "server" or self.owner is not None:
            raise OwnershipError("请先在受控主机安全弹出，再请求取回文件盘，等待设备释放")

    def _tool(self, *args: str):
        if not shutil.which(args[0]):
            raise ValueError("文件管理需要安装 mtools")
        result = subprocess.run(args, capture_output=True, timeout=120)
        if result.returncode:
            raise ValueError("文件系统工具失败：" + result.stderr.decode("utf-8", "replace")[-300:])
        return result.stdout

    def list_files(self):
        with self.lock:
            self._server_only()
            output = self._tool("mdir", "-b", "-i", str(self.image), "::/")
            return [line.removeprefix("::/") for line in output.decode("utf-8", "replace").splitlines()
                    if line.startswith("::/") and not line.endswith("/")]

    def upload(self, source: Path, name: str):
        with self.lock:
            self._server_only()
            # 直接修改文件盘；失败时不自动重新交给主机，应检查文件系统后再挂载。
            self._tool("mcopy", "-o", "-i", str(self.image), str(source), "::/" + name)
            with self.image.open("r+b") as stream:
                os.fsync(stream.fileno())
            self.version = secrets.token_hex(32)
            return self.info()

    def download(self, name: str, target: Path):
        with self.lock:
            self._server_only()
            self._tool("mcopy", "-i", str(self.image), "::/" + name, str(target))
