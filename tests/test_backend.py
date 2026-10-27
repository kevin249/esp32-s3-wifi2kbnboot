"""覆盖版本隔离、失败回滚、HTTP 长连接及真正 FAT32 文件写入。"""
import concurrent.futures
import http.client
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import threading
import unittest

from backend.server import SnapshotStore, DiskServer, first_partition, safe_name

# HTTP 头只使用 ASCII；文件名和页面使用 UTF-8。
TOKEN = "test-access-token-123456789"


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.image = self.root / "original.img"
        self.image.write_bytes(bytes(range(256)) * 256)
        self.store = SnapshotStore(self.root / "store")
        self.original = self.store.import_image(self.image)

    def tearDown(self):
        self.temp.cleanup()

    def test_blocks_and_bounds(self):
        version = self.original["version"]
        self.assertEqual(self.store.read_block(version, 512, 16384), self.image.read_bytes()[512:16896])
        for offset, length in ((-512, 512), (1, 512), (0, 0), (0, 65537), (65536, 512)):
            with self.assertRaises(ValueError):
                self.store.read_block(version, offset, length)
        for version in ("../original", "0" * 63, "G" * 64):
            with self.assertRaises(ValueError): self.store.read_block(version, 0, 512)

    def test_snapshot_publication_and_explicit_refresh(self):
        payload = self.root / "bios.bin"
        payload.write_bytes(b"BIOS")

        def mutate(image, source, name):
            with image.open("r+b") as stream:
                stream.seek(512)
                stream.write(source.read_bytes())

        published = self.store.add_file(payload, "bios.bin", mutator=mutate)
        self.assertNotEqual(self.original["version"], published["version"])
        self.assertEqual(published["refresh"], self.original["refresh"])
        self.assertEqual(self.store.read_block(self.original["version"], 512, 512), self.image.read_bytes()[512:1024])
        self.assertTrue(self.store.read_block(published["version"], 512, 512).startswith(b"BIOS"))
        self.assertEqual(self.store.activate()["refresh"], published["refresh"] + 1)
        self.assertEqual(SnapshotStore(self.store.root).manifest(), self.store.manifest())

    def test_failed_mutation_never_publishes(self):
        before = self.store.manifest()

        def fail(image, source, name):
            image.write_bytes("损坏".encode("utf-8"))
            raise ValueError("模拟磁盘满")

        with self.assertRaises(ValueError): self.store.add_file(self.image, "bios.bin", mutator=fail)
        self.assertEqual(self.store.manifest(), before)
        self.assertEqual(list(self.store.root.glob("stage-*")), [])

    def test_invalid_names(self):
        for name in ("", "../bios.bin", "a\\b", ".", "..", "*", "a?", "a:", "x\x00", "x.", "x ", "%FILE"):
            with self.assertRaises(ValueError): safe_name(name)
        self.assertEqual(safe_name("主板BIOS-1.2.bin"), "主板BIOS-1.2.bin")

    def test_parallel_readers_are_version_isolated(self):
        changed = self.root / "changed.img"
        changed.write_bytes(b"X" * self.original["size"])
        updated = self.store.import_image(changed)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            futures = [executor.submit(self.store.read_block, version, 0, 16384)
                       for version in [self.original["version"], updated["version"]] * 20]
        for i, future in enumerate(futures):
            expected = self.image.read_bytes()[:16384] if i % 2 == 0 else b"X" * 16384
            self.assertEqual(future.result(), expected)

    def test_startup_detects_modified_snapshot(self):
        self.store.image_path(self.original["version"]).write_bytes(b"Y" * self.original["size"])
        with self.assertRaises(ValueError): SnapshotStore(self.store.root)

    def test_http_keepalive_auth_and_exact_bytes(self):
        server = DiskServer(("127.0.0.1", 0), self.store, TOKEN, 1024)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)
        headers = {"Authorization": f"Bearer {TOKEN}"}
        try:
            connection.request("GET", "/api/manifest", headers=headers)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read())["version"], self.original["version"])
            sock = connection.sock
            connection.request("GET", f'/api/block?version={self.original["version"]}&offset=0&length=16384', headers=headers)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(response.getheader("X-Image-Version"), self.original["version"])
            self.assertEqual(response.getheader("X-Offset"), "0")
            self.assertEqual(response.read(), self.image.read_bytes()[:16384])
            self.assertIs(connection.sock, sock)
            connection.request("POST", "/api/activate", body=b"", headers=headers)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            response.read()
            connection.close()
            connection.request("GET", "/api/manifest")
            response = connection.getresponse()
            self.assertEqual(response.status, 401)
            response.read()
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            thread.join()

    def test_http_rejects_oversize_and_path_traversal(self):
        server = DiskServer(("127.0.0.1", 0), self.store, TOKEN, 1024)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=5)
        headers = {"Authorization": f"Bearer {TOKEN}"}
        try:
            for path, body, status in (("/api/files?name=bios.bin", b"x" * 1025, 413),
                                       ("/api/files?name=..%2Fbios.bin", b"x", 400)):
                connection.request("POST", path, body=body, headers=headers)
                response = connection.getresponse()
                self.assertEqual(response.status, status)
                response.read()
                connection.close()
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            thread.join()

    def test_mbr_and_gpt_partition_offsets(self):
        data = bytearray(65536)
        data[510:512] = b"\x55\xaa"
        data[450] = 0x0C
        struct.pack_into("<II", data, 454, 8, 100)
        self.image.write_bytes(data)
        self.assertEqual(first_partition(self.image), (4096, 51200))
        data[450] = 0xEE
        data[512:520] = b"EFI PART"
        struct.pack_into("<QII", data, 512 + 72, 2, 128, 128)
        data[1024:1040] = b"X" * 16
        struct.pack_into("<QQ", data, 1024 + 32, 8, 107)
        self.image.write_bytes(data)
        self.assertEqual(first_partition(self.image), (4096, 51200))

    @unittest.skipUnless(all(shutil.which(x) for x in ("mkfs.fat", "mcopy", "mdir", "mmd")), "本机未安装 FAT32 集成测试工具")
    def test_real_fat32_upload_and_extract(self):
        partition = self.root / "fat.img"
        with partition.open("wb") as stream: stream.truncate(64 * 1024 * 1024)
        subprocess.run(["mkfs.fat", "-F", "32", str(partition)], check=True, capture_output=True)
        mbr = bytearray(512)
        mbr[510:512] = b"\x55\xaa"
        mbr[450] = 0x0C
        struct.pack_into("<II", mbr, 454, 2048, partition.stat().st_size // 512)
        disk = self.root / "fat-disk.img"
        with disk.open("wb") as target, partition.open("rb") as source:
            target.write(mbr)
            target.seek(1024 * 1024)
            shutil.copyfileobj(source, target)
        initial = self.store.import_image(disk)
        payload = self.root / "BIOS.bin"
        payload.write_bytes(bytes(range(256)) * 100)
        updated = self.store.add_file(payload, "BIOS.bin", activate=True)
        output = self.root / "extracted.bin"
        subprocess.run(["mcopy", "-i", f'{self.store.image_path(updated["version"])}@@1048576', "::/BIOS/BIOS.bin", str(output)], check=True, capture_output=True)
        self.assertEqual(output.read_bytes(), payload.read_bytes())
        self.assertEqual(updated["refresh"], initial["refresh"] + 1)
        self.assertEqual(self.store.image_path(initial["version"]).stat().st_size, disk.stat().st_size)


if __name__ == "__main__":
    unittest.main()
