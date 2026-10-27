"""光盘镜像发布：不原地修改正在读取的 ISO。"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import tempfile
import threading

VERSION_RE = re.compile(r"[0-9a-f]{64}\Z")


def safe_name(name: str) -> str:
    if (not name or len(name.encode("utf-8")) > 180 or name.startswith(".")
            or name.endswith((".", " "))
            or any(ord(c) < 32 or c in '/\\:*?"<>|%' for c in name)):
        raise ValueError("文件名无效，不能包含路径或 FAT 通配符")
    return name


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


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
                raise ValueError("当前 ISO 快照校验失败，不能原地编辑已发布镜像")

    def image_path(self, version: str) -> Path:
        if not VERSION_RE.fullmatch(version):
            raise ValueError("镜像版本无效")
        return self.images / f"{version}.iso"

    def manifest(self):
        with self.lock:
            return dict(self.current) if self.current else None

    def import_image(self, image: Path):
        with self.lock:
            size = image.stat().st_size
            if not size or size % 2048 or size // 2048 > 0xFFFFFFFF:
                raise ValueError("ISO 大小必须为2048字节整数倍，且在READ10容量范围内")
            fd, temporary = tempfile.mkstemp(dir=self.root, prefix="stage-", suffix=".iso")
            os.close(fd)
            stage = Path(temporary)
            try:
                shutil.copyfile(image, stage)
                if stage.stat().st_size != size:
                    raise ValueError("选择的 ISO 在复制过程中改变大小")
                version = digest(stage)
                with stage.open("r+b") as stream:
                    os.fsync(stream.fileno())
                destination = self.image_path(version)
                if destination.exists():
                    stage.unlink()
                else:
                    os.replace(stage, destination)
                manifest = {"version": version, "size": size, "sector_size": 2048, "name": image.name}
                fd, temporary = tempfile.mkstemp(dir=self.root, prefix="manifest-")
                try:
                    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
                        json.dump(manifest, stream, ensure_ascii=False)
                        stream.flush()
                        os.fsync(stream.fileno())
                    os.replace(temporary, self.manifest_path)
                finally:
                    if os.path.exists(temporary): os.unlink(temporary)
                self.current = manifest
                return dict(manifest)
            finally:
                stage.unlink(missing_ok=True)

    def read_block(self, version: str, offset: int, length: int):
        if offset < 0 or offset % 512 or not 0 < length <= 65536 or length % 512:
            raise ValueError("读取范围无效")
        with self.image_path(version).open("rb") as stream:
            if offset + length > os.fstat(stream.fileno()).st_size:
                raise ValueError("读取越过 ISO 末尾")
            stream.seek(offset)
            data = stream.read(length)
        if len(data) != length:
            raise OSError("ISO 短读")
        return data
