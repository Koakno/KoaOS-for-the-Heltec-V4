#!/usr/bin/env python3
"""Drives the meshcore app (built for the host with tests/mc_mock.c) with packets built independently
from the MeshCore spec, and checks what it prints and transmits.   Usage: python3 tests/mc_app_test.py"""
import hashlib, hmac, os, subprocess, sys, tempfile, struct
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import serialization as S

NOW = 1760000000
P = 2**255 - 19
def pubraw(k): return k.public_key().public_bytes(S.Encoding.Raw, S.PublicFormat.Raw)
def expand(seed):
    h = bytearray(hashlib.sha512(seed).digest()); h[0] &= 248; h[31] &= 63; h[31] |= 64
    return bytes(h), pubraw(ed25519.Ed25519PrivateKey.from_private_bytes(seed))
def mont(pub):
    y = int.from_bytes(pub, "little") & ((1 << 255) - 1)
    return ((1 + y) * pow(1 - y, -1, P) % P).to_bytes(32, "little")
def ecdh(prv64, their_pub):
    return x25519.X25519PrivateKey.from_private_bytes(prv64[:32]).exchange(x25519.X25519PublicKey.from_public_bytes(mont(their_pub)))
def aes(key16, data, enc):
    c = Cipher(algorithms.AES(key16), modes.ECB()); x = c.encryptor() if enc else c.decryptor(); return x.update(data) + x.finalize()
def enc_mac(secret32, plain):
    plain += b"\0" * (-len(plain) % 16)
    ct = aes(secret32[:16], plain, True)
    return hmac.new(secret32, ct, hashlib.sha256).digest()[:2] + ct
def dec_mac(secret32, blob):
    mac, ct = blob[:2], blob[2:]
    assert hmac.new(secret32, ct, hashlib.sha256).digest()[:2] == mac, "MAC mismatch"
    return aes(secret32[:16], ct, False)
def header(route, ptype): return bytes([(ptype << 2) | route])
def packet(route, ptype, payload, path=b"", pathlen_byte=None, transport=b""):
    pl = len(path) if pathlen_byte is None else pathlen_byte
    return header(route, ptype) + transport + bytes([pl]) + path + payload
def advert(seed, name, ts, atype=1, bad=False):
    prv, pub = expand(seed); app = bytes([atype | 0x80]) + name.encode()
    sig = ed25519.Ed25519PrivateKey.from_private_bytes(seed).sign(pub + struct.pack("<I", ts) + app)
    if bad: sig = bytes([sig[0] ^ 1]) + sig[1:]
    return packet(1, 4, pub + struct.pack("<I", ts) + sig + app)

PUBLIC = bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")
def chan_secret(key16): return key16 + b"\0" * 16
def grp(key16, text, ts=NOW, **kw):
    pl = bytes([hashlib.sha256(key16).digest()[0]]) + enc_mac(chan_secret(key16), struct.pack("<IB", ts, 0) + text.encode())
    return packet(1, 5, pl, **kw)

me_seed = bytes(range(1, 33)); me_prv, me_pub = expand(me_seed)
al_seed = bytes(range(101, 133)); al_prv, al_pub = expand(al_seed)
mal_seed = bytes(range(201, 233))
rep_seed = bytes(range(50, 82))
# make sure our test identity does not use a reserved hash byte
assert me_pub[0] not in (0, 255) and al_pub[0] not in (0, 255)

tag = hashlib.sha256(b"#test").digest()[:16]
dm_text = "hi koa, it's Alice"
dm_plain = struct.pack("<IB", NOW - 5, 0) + dm_text.encode()
al_secret = ecdh(al_prv, me_pub)
dm_pkt = packet(1, 2, bytes([me_pub[0], al_pub[0]]) + enc_mac(al_secret, dm_plain), path=bytes([0x5a]))
expected_ack_for_dm = hashlib.sha256(dm_plain + al_pub).digest()[:4]

script = []
def KEY(t): script.append("KEY " + t)
def RX(b): script.append("RX " + b.hex())
KEY("/verify on")
RX(advert(al_seed, "Alice", NOW - 100))
RX(advert(mal_seed, "Mallory", NOW - 50, bad=True))
RX(advert(rep_seed, "Hill Repeater", NOW - 60, atype=2))
RX(advert(al_seed, "Alice", NOW - 200))                      # older than the stored one: ignored
RX(grp(PUBLIC, "Alice: hello mesh"))
RX(grp(PUBLIC, "Alice: hello mesh"))                          # duplicate (e.g. via another repeater): printed once
KEY("/join #test")
RX(grp(tag, "Bob: on the hashtag channel", path=bytes([1, 2, 3])))
RX(dm_pkt)
KEY("/contacts")
KEY("/msg Alice hi there")
script.append("MARK1")
KEY("/use 1")
KEY("hello from koa")
KEY("/adv")
RX(grp(PUBLIC, "Carol: two-byte hash path", path=bytes([1, 2, 3, 4]), pathlen_byte=0x42))      # 1.14-style: 2 hops x 2 bytes
RX(grp(PUBLIC, "Dave: via transport flood", transport=b"\x01\x00\x02\x00") .replace(b"\x15", b"\x14", 1))  # route 0 + transport codes
for junk in (b"\x00", os.urandom(1), os.urandom(7), os.urandom(60), b"\x15\xff" + os.urandom(20), b"\x15\x00", b"\x11\x00" + b"\x00" * 50):
    RX(junk)
KEY("/msg Alice second one")
script.append("MARK2")
KEY("/stats")
KEY("/quit")

def run(script_lines, data):
    sf = os.path.join(data, "..", "script.txt")
    with open(sf, "w") as f: f.write("\n".join(l for l in script_lines if not l.startswith("MARK")) + "\n")
    env = dict(os.environ, MC_SCRIPT=sf, MC_DATA=data, MC_NOW=str(NOW), ASAN_OPTIONS="detect_leaks=0")
    return subprocess.run(["./mc_app_host"], env=env, capture_output=True, text=True, timeout=120)

ok = True
def chk(name, cond, detail=""):
    global ok; ok &= bool(cond); print(("PASS " if cond else "FAIL ") + name + ("" if cond else "   " + detail))

with tempfile.TemporaryDirectory() as d:
    data = os.path.join(d, "data"); os.mkdir(data)
    with open(os.path.join(data, "identity"), "wb") as f: f.write(me_prv + me_pub)     # pre-seed so we know our key
    r = run(script, data)
    out = r.stdout
    if r.returncode != 0: print(r.stderr[-3000:])
    lines = out.splitlines()
    chk("exits cleanly (no sanitizer errors)", r.returncode == 0 and "EXIT 0" in out and "ERROR: AddressSanitizer" not in r.stderr and "runtime error" not in r.stderr, r.stderr[-800:])
    chk("US preset applied (910.525 / 62.5 / SF7 / CR5, sync 0x12, preamble 16)", any(l.startswith("RADIO 910.525 62.5 7 5 0x12") and l.endswith(" 16") for l in lines))
    chk("new node advert listed", "[advert] NEW Alice (chat)" in out)
    chk("repeater advert listed as repeater", "NEW Hill Repeater (repeater)" in out)
    chk("bad advert signature rejected", "BAD signature" in out and "Mallory" not in out.replace("BAD signature ignored", ""))
    chk("replayed/older advert ignored", out.count("Alice (chat)") == 1)
    chk("public channel message decrypted", "[Public] Alice: hello mesh" in out)
    chk("duplicate packet printed once", out.count("[Public] Alice: hello mesh") == 1)
    chk("#hashtag channel derived + decrypted", "[#test] Bob: on the hashtag channel" in out)
    chk("direct message decrypted (ECDH)", f"[DM] Alice: {dm_text}" in out)
    chk("2-byte-hash path header parsed", "[Public] Carol: two-byte hash path" in out)
    chk("transport-flood route parsed", "[Public] Dave: via transport flood" in out)
    txs = [bytes.fromhex(l[3:]) for l in lines if l.startswith("TX ")]
    acks = [t for t in txs if (t[0] >> 2) & 15 == 3]
    chk("ack sent for the DM with the right 4-byte checksum", len(acks) == 1 and acks[0][2:] == expected_ack_for_dm, str(acks))
    dms = [t for t in txs if (t[0] >> 2) & 15 == 2]
    chk("two DMs transmitted", len(dms) == 2)
    if dms:
        t = dms[0]; pl = t[2:]
        chk("DM: flood route, path empty, dest/src hash bytes right", t[0] & 3 == 1 and t[1] == 0 and pl[0] == al_pub[0] and pl[1] == me_pub[0])
        try:
            pt = dec_mac(al_secret, pl[2:]).rstrip(b"\0")
            ts, fl = struct.unpack("<IB", pt[:5])
            chk("DM: Alice's side can decrypt it (MAC ok, plaintext right)", pt[5:] == b"hi there" and fl == 0 and ts == NOW, repr(pt))
            my_ack = hashlib.sha256(pt[:5] + b"hi there" + me_pub).digest()[:4]
        except Exception as e:
            chk("DM: Alice's side can decrypt it", False, str(e)); my_ack = b""
    grps = [t for t in txs if (t[0] >> 2) & 15 == 5]
    chk("channel message transmitted on #test? no: /use 1 selects Public", len(grps) == 1 and grps[0][2] == hashlib.sha256(PUBLIC).digest()[0])
    if grps:
        pt = dec_mac(chan_secret(PUBLIC), grps[0][3:]).rstrip(b"\0")
        chk("channel message decrypts: 'KoaOS: hello from koa'", pt[5:] == b"KoaOS: hello from koa" and struct.unpack("<I", pt[:4])[0] == NOW, repr(pt))
    advs = [t for t in txs if (t[0] >> 2) & 15 == 4]
    chk("own advert transmitted", len(advs) == 1)
    if advs:
        pl = advs[0][2:]; pub, ts, sig, app = pl[:32], struct.unpack("<I", pl[32:36])[0], pl[36:100], pl[100:]
        try:
            ed25519.Ed25519PublicKey.from_public_bytes(pub).verify(sig, pub + pl[32:36] + app); good = True
        except Exception: good = False
        chk("own advert: our pubkey, timestamp, name flags, signature verifies with python", pub == me_pub and ts == NOW and app == bytes([0x81]) + b"KoaOS" and good)
    # --- second pass: delivery confirmation (ack as its own packet, and ack riding in a PATH packet)
    data2 = os.path.join(d, "data2"); os.mkdir(data2)
    for fn in ("identity", "contacts", "config", "channels"):
        if os.path.exists(os.path.join(data, fn)): open(os.path.join(data2, fn), "wb").write(open(os.path.join(data, fn), "rb").read())
    s2 = ["KEY /msg Alice ping", "KEY /msg Alice pong"]
    # compute the acks the app should expect: needs the timestamps/text it will use -> same NOW, flags 0
    ack_ping = hashlib.sha256(struct.pack("<IB", NOW, 0) + b"ping" + me_pub).digest()[:4]
    ack_pong = hashlib.sha256(struct.pack("<IB", NOW, 0) + b"pong" + me_pub).digest()[:4]
    s2.append("RX " + packet(1, 3, ack_ping).hex())
    path_plain = bytes([2, 0x11, 0x22, 3]) + ack_pong                       # path_len, path, extra_type=ACK, extra
    s2.append("RX " + packet(2, 8, bytes([me_pub[0], al_pub[0]]) + enc_mac(al_secret, path_plain), path=b"").hex())
    s2.append("RX " + packet(1, 3, b"\xde\xad\xbe\xef").hex())              # unknown ack: ignored
    s2.append("KEY /quit")
    r2 = run(s2, data2)
    chk("delivery confirmed by a plain ACK packet", "delivered to Alice" in r2.stdout)
    chk("delivery confirmed by an ack carried in a PATH packet", r2.stdout.count("delivered to Alice") == 2, r2.stdout[-600:])
    chk("saved contacts/identity/channels survive a restart", "NEW" not in r2.stdout and r2.returncode == 0)
    # --- restart with the saved state: the identity must be re-used, not regenerated
    chk("identity file re-used", open(os.path.join(data, "identity"), "rb").read() == me_prv + me_pub)

print("\nALL PASS" if ok else "\nSOME FAILED")
if not ok: print("---- app output ----\n" + out)
sys.exit(0 if ok else 1)
