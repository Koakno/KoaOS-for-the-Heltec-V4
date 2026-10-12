import subprocess, hashlib, sys
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives import serialization as S
out = dict(l.split("=",1) for l in subprocess.check_output(["./mc_crypto_test"]).decode().split())
seed  = bytes((i*7+1)&255 for i in range(32)); seed2 = bytes((255-i*5)&255 for i in range(32))
raw = lambda k: k.public_key().public_bytes(S.Encoding.Raw, S.PublicFormat.Raw)
ok = True
def chk(name, got, exp):
    global ok
    good = (got == exp); ok &= good
    print(("PASS " if good else "FAIL ")+name+("" if good else f"\n  got {got}\n  exp {exp}"))
chk("sha512 abc", out["sha512_abc"], hashlib.sha512(b"abc").hexdigest())
chk("sha512 300B streamed", out["sha512_300"], hashlib.sha512(bytes(i&255 for i in range(300))).hexdigest())
k1 = ed25519.Ed25519PrivateKey.from_private_bytes(seed); k2 = ed25519.Ed25519PrivateKey.from_private_bytes(seed2)
chk("ed25519 pubkey 1", out["pub1"], raw(k1).hex()); chk("ed25519 pubkey 2", out["pub2"], raw(k2).hex())
h = bytearray(hashlib.sha512(seed).digest()); h[0]&=248; h[31]&=63; h[31]|=64
chk("expanded private key layout (orlp)", out["prv1"], bytes(h).hex())
msg = b"hello meshcore advert"
chk("ed25519 signature == RFC8032 signature", out["sig1"], k1.sign(msg).hex())
chk("verify good sig", out["verify_ok"], "0"); chk("verify tampered sig rejected", out["verify_bad"], "-1")
# ECDH: Ed25519 -> X25519 conversion, compare with libsodium-style conversion done in python
p = 2**255-19
def to_mont(pub):
    y = int.from_bytes(pub,"little") & ((1<<255)-1)
    return ((1+y)*pow(1-y,-1,p) % p).to_bytes(32,"little")
xa = x25519.X25519PrivateKey.from_private_bytes(bytes(h[:32]))
exp = xa.exchange(x25519.X25519PublicKey.from_public_bytes(to_mont(raw(k2)))).hex()
chk("ecdh a->b matches python X25519", out["ecdh_a"], exp)
chk("ecdh symmetric", out["ecdh_a"], out["ecdh_b"])
print("ALL PASS" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
