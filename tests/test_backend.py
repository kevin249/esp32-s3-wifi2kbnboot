"""验证双逻辑盘、互斥交接、写入落盘、ISO 浏览挂载和真实文件传输。"""
import concurrent.futures
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest

from backend.images import SnapshotStore, safe_name
from backend.server import DiskServer
from backend.transfer import TransferDisk, OwnershipError

TOKEN = "test-access-token-123456789"


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.library = self.root / "isos"
        self.library.mkdir()
        self.iso = self.library / "测试系统.iso"
        self.iso.write_bytes(bytes(range(256)) * 256)
        self.store = SnapshotStore(self.root / "data")
        self.initial = self.store.import_image(self.iso)
        transfer_image = self.root / "transfer.img"
        transfer_image.write_bytes(b"X" * 65536)
        self.transfer = TransferDisk(transfer_image)
        self.server = None

    def tearDown(self):
        if self.server:
            self.connection.close()
            self.server.shutdown()
            self.server.server_close()
            self.thread.join()
        self.temp.cleanup()

    def start_http(self, **kwargs):
        self.server = DiskServer(("127.0.0.1", 0), self.store, self.transfer, TOKEN, self.library, **kwargs)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.connection = http.client.HTTPConnection(*self.server.server_address, timeout=5)

    def request(self, method, path, body=None, session=None, authorized=True):
        headers = {"Authorization": f"Bearer {TOKEN}"} if authorized else {}
        if session: headers["X-Session"] = session
        if isinstance(body, dict):
            body = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        self.connection.request(method, path, body=body, headers=headers)
        response = self.connection.getresponse()
        data = response.read()
        if "json" in response.getheader("Content-Type", ""): data = json.loads(data)
        return response.status, data

    def test_iso_snapshot_versions_and_restart(self):
        old = self.initial["version"]
        self.iso.write_bytes(b"Y" * 65536)
        new = self.store.import_image(self.iso)
        self.assertNotEqual(old, new["version"])
        self.assertEqual(self.store.read_block(old, 0, 16384), bytes(range(256)) * 64)
        self.assertEqual(self.store.read_block(new["version"], 0, 16384), b"Y" * 16384)
        self.assertEqual(SnapshotStore(self.store.root).manifest(), new)

    def test_ranges_and_names(self):
        for offset, length in ((-512, 512), (1, 512), (0, 0), (0, 65537), (65536, 512)):
            with self.assertRaises(ValueError): self.store.read_block(self.initial["version"], offset, length)
        for name in ("", "../bios.bin", "a\\b", "*", "a?", "a:", "x\x00", "x.", "x "):
            with self.assertRaises(ValueError): safe_name(name)
        self.assertEqual(safe_name("主板BIOS.bin"), "主板BIOS.bin")

    def test_transfer_read_write_and_invalid_sessions(self):
        lease = self.transfer.claim("board-one")
        data = b"B" * 8192
        result = self.transfer.write(lease["session"], lease["version"], 512, data)
        self.assertEqual(result["written"], len(data))
        self.assertEqual(self.transfer.read(lease["session"], lease["version"], 512, len(data)), data)
        self.assertEqual(self.transfer.image.read_bytes()[512:8704], data)
        with self.assertRaises(OwnershipError): self.transfer.claim("board-two")
        with self.assertRaises(OwnershipError): self.transfer.write("invalid", lease["version"], 0, b"x" * 512)
        with self.assertRaises(ValueError): self.transfer.write(lease["session"], lease["version"], 1, b"x" * 512)

    def test_exclusive_handoff(self):
        lease = self.transfer.claim("board")
        self.transfer.request_server()
        # 请求交接后仍允许已经在途的主机 I/O 完成。
        self.transfer.write(lease["session"], lease["version"], 0, b"Y" * 512)
        with self.assertRaises(OwnershipError): self.transfer.list_files()
        with self.assertRaises(OwnershipError): self.transfer.attach()
        self.transfer.release(lease["session"])
        with self.assertRaises(OwnershipError): self.transfer.read(lease["session"], lease["version"], 0, 512)
        new = self.transfer.attach()
        self.assertNotEqual(new["version"], lease["version"])
        current = self.transfer.claim("board")
        self.assertNotEqual(current["session"], lease["session"])

    def test_http_both_luns_long_connection_and_auth(self):
        self.start_http()
        status, manifest = self.request("GET", "/api/manifest")
        self.assertEqual(status, 200)
        self.assertEqual(manifest["iso"]["sector_size"], 2048)
        self.assertEqual(manifest["transfer"]["sector_size"], 512)
        sock = self.connection.sock
        status, lease = self.request("POST", "/api/transfer/claim", {"device": "board"})
        self.assertEqual(status, 200)
        status, result = self.request("POST", f'/api/transfer/write?version={lease["version"]}&offset=512',
                                      b"B" * 512, lease["session"])
        self.assertEqual((status, result["written"]), (200, 512))
        status, data = self.request("GET", f'/api/block?lun=1&version={lease["version"]}&offset=512&length=512', session=lease["session"])
        self.assertEqual((status, data), (200, b"B" * 512))
        status, data = self.request("GET", f'/api/block?lun=0&version={self.initial["version"]}&offset=0&length=16384')
        self.assertEqual((status, data), (200, self.iso.read_bytes()[:16384]))
        self.assertIs(self.connection.sock, sock)
        status, _ = self.request("GET", "/api/manifest", authorized=False)
        self.assertEqual(status, 401)

    def test_browser_mount_and_traversal(self):
        self.start_http()
        status, listing = self.request("GET", "/api/library")
        self.assertEqual(status, 200)
        self.assertEqual(listing["items"][0]["name"], self.iso.name)
        from urllib.parse import quote
        old_refresh = self.server.refresh
        status, mounted = self.request("POST", "/api/mount", {"path": self.iso.name})
        self.assertEqual(status, 200)
        self.assertNotEqual(mounted["refresh"], old_refresh)
        status, _ = self.request("GET", "/api/library?path=" + quote("../"))
        self.assertEqual(status, 400)
        status, _ = self.request("POST", "/api/mount", {"path": "../outside.iso"})
        self.assertEqual(status, 400)
        status, manifest = self.request("POST", "/api/eject", {})
        self.assertEqual(status, 200)
        self.assertIsNone(manifest["iso"])

    def test_upload_limits_and_owner_conflict(self):
        self.start_http(max_upload=1024)
        status, _ = self.request("POST", "/api/transfer/upload?name=BIOS.bin", b"x" * 1025)
        self.assertEqual(status, 413)
        status, _ = self.request("POST", "/api/transfer/upload?name=BIOS.bin", b"x")
        self.assertEqual(status, 409)
        status, _ = self.request("POST", "/api/transfer/upload?name=..%2Fa", b"x")
        self.assertEqual(status, 400)

    def test_simultaneous_reads_do_not_mix_luns(self):
        lease = self.transfer.claim("board")
        def read(index):
            if index % 2: return self.transfer.read(lease["session"], lease["version"], 0, 16384)
            return self.store.read_block(self.initial["version"], 0, 16384)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            result = list(pool.map(read, range(30)))
        for i, data in enumerate(result): self.assertEqual(data, b"X" * 16384 if i % 2 else self.iso.read_bytes()[:16384])

    def test_keyboard_mouse_and_power_proxy(self):
        calls = []
        class Device(BaseHTTPRequestHandler):
            def do_POST(self):
                calls.append((self.path, self.headers.get("Authorization"),
                              json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                data = b'{"queued":true}'
                self.send_response(202)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            def log_message(self, *args): pass
        device = ThreadingHTTPServer(("127.0.0.1", 0), Device)
        thread = threading.Thread(target=device.serve_forever, daemon=True)
        thread.start()
        self.start_http(device_url=f"http://127.0.0.1:{device.server_port}")
        try:
            requests = [("/hid/keyboard", {"modifiers": 0, "keys": [4]}),
                        ("/hid/mouse", {"buttons": 0, "x": 10, "y": -10, "wheel": 0}),
                        ("/control/power", {"button": "pwr", "duration_ms": 200})]
            for path, body in requests:
                status, result = self.request("POST", "/api" + path, body)
                self.assertEqual((status, result), (200, {"queued": True}))
            self.assertEqual(calls, [(path, f"Bearer {TOKEN}", body) for path, body in requests])
        finally:
            device.shutdown()
            device.server_close()
            thread.join()

    @unittest.skipUnless(all(shutil.which(x) for x in ("mformat", "mcopy", "mdir")), "本机未安装 mtools")
    def test_real_bidirectional_files(self):
        transfer = TransferDisk(self.root / "real-files.img", size_mib=32)
        lease = transfer.claim("board")
        # 模拟主机已安全弹出，把文件盘交还服务器。
        transfer.request_server()
        transfer.release(lease["session"])
        payload = self.root / "bios.bin"
        payload.write_bytes(bytes(range(256)) * 100)
        transfer.upload(payload, "BIOS-upgrade.bin")
        self.assertIn("BIOS-upgrade.bin", transfer.list_files())
        extracted = self.root / "extracted.bin"
        transfer.download("BIOS-upgrade.bin", extracted)
        self.assertEqual(extracted.read_bytes(), payload.read_bytes())
        transfer.attach()
        next_lease = transfer.claim("board")
        self.assertNotEqual(lease["version"], next_lease["version"])
        with self.assertRaises(OwnershipError): transfer.upload(payload, "bad.bin")


if __name__ == "__main__": unittest.main()
