"""Synthetic nonbootable images and disposable signed state; no camera access."""
import struct,sys,zlib,tempfile,unittest
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import sign_update as SIGN
KEY=Ed25519PrivateKey.from_private_bytes(bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'))
def kernel():
    payload = b"synthetic test fixture, not an executable kernel"
    header = bytearray(64)
    struct.pack_into(">I", header, 0, 0x27051956)
    struct.pack_into(">I", header, 12, len(payload))
    struct.pack_into(">II", header, 16, 0xA0008000, 0xA0008000)
    struct.pack_into(">I", header, 24, zlib.crc32(payload))
    header[28:31] = bytes((5, 2, 2))
    struct.pack_into(">I", header, 4, zlib.crc32(header))
    return bytes(header) + payload


def rootfs():
    data = bytearray(96)
    data[:4] = b"hsqs"
    struct.pack_into("<H", data, 20, 4)
    struct.pack_into("<HH", data, 28, 4, 0)
    struct.pack_into("<Q", data, 40, len(data))
    return bytes(data)


def package(records=None):
    if records is None:
        records = [(2, kernel()), (3, rootfs())]
    header = b"Fuhan8626V100".ljust(20, b"\0") + struct.pack(">I", 0xABCD1234) + bytes(8)
    return header + b"".join(struct.pack(">III", kind, len(data), 0) + data
                             for kind, data in records)


NAMES = ("bootstrap", "uboot-env", "uboot", "kernel", "rootfs", "config")
SIZES = (0x40000, 0x10000, 0x40000, 0x1d0000, 0x550000, 0x50000)
MAP = "dev:    size   erasesize  name\n" + "".join(
    f'mtd{i}: {size:08x} 00010000 "{name}"\n'
    for i, (name, size) in enumerate(zip(NAMES, SIZES)))


class ApplyFixtures(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="camera-ota-plan-")
        self.base = Path(self.temp.name)
        self.state, self.stage = self.base / "state", self.base / "stage"
        self.state.mkdir(mode=0o700)
        self.stage.mkdir(mode=0o700)
        self.pub = self.base / "public.key"
        self.pub.write_bytes(KEY.public_key().public_bytes(serialization.Encoding.Raw,
                                                          serialization.PublicFormat.Raw))
        self.pub.chmod(0o600)
        self.map = self.base / "mtd"
        self.map.write_text(MAP)
        self.fixture()

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self, records=None):
        self.capsule = SIGN.envelope(package(records), KEY, 2)
        for path, data in ((self.state / "accepted", b"1\n"),
                           (self.state / "pending", self.capsule[:192]),
                           (self.state / "lock", b""),
                           (self.stage / "pending.cap", self.capsule)):
            path.write_bytes(data)
            path.chmod(0o600)

