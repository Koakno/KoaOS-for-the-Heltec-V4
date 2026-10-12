# meshcore -- MeshCore client for KoaOS (Heltec V4)

A KoaOS app (wasm, ABI v1) that speaks the MeshCore over-the-air protocol directly through the
OS's LoRa calls. It is a chat/companion node: it listens, shows who is around, reads and sends
channel messages, and sends/receives direct messages. It does not repeat other people's traffic.

## Build and install
```
cd koaos-sdk
make meshcore          # needs clang + wasm-ld (apt install clang lld)
make serve             # then on the V4:  install -s <pc-ip>:8000 ; install meshcore ; meshcore
make mctest            # native tests, no wasm tools needed (needs gcc + python3-cryptography)
```
Manifest: `requires=lora`. The radio is exclusive while the app runs.

## Using it
Type text to send it to the current channel. Commands start with `/`:
```
/help  /me  /name <n>  /adv  /contacts  /msg <#|name> <text>
/ch  /use <n>  /join #name  /addch <name> <32 hex>
/radio [mhz bw sf cr]  /power <dBm>  /sync <hex>  /verify on|off  /time <unix>  /stats  /quit
```
Defaults: MeshCore "USA/Canada (Recommended)" = 910.525 MHz, BW 62.5, SF7, CR 4/5, sync 0x12,
preamble 16, 10 dBm. Your identity, name, radio settings, channels and contacts are saved in the
app's private /data/meshcore/ folder. The "Public" channel is built in; `/join #name` derives a
hashtag channel key (SHA-256 of the name, name typed exactly as the others type it, including `#`).
Direct messages are sent only to chat nodes you have heard an advert from.

Clock: adverts and messages carry timestamps. If KoaOS has no GPS/Wi-Fi time, use `/time <unix>`.

## What is implemented
Packet header/route/path parsing (flood, direct, transport codes, 1-3 byte path hashes), dedup,
ADVERT (parse, optional Ed25519 verify, own signed advert), GRP_TXT (public, #hashtag, PSK channels),
TXT_MSG with X25519 ECDH + AES-128-ECB + 2-byte HMAC, ACK and PATH-with-ack for delivery receipts.

## Not implemented
Repeating, learned direct routes (DMs go out as flood), retries, GPS location in adverts,
room-server / repeater login and remote CLI, group data, phone-app (BLE/USB companion) protocol.

## Testing status
Verified natively (`make mctest`, AddressSanitizer + UBSan): SHA-512, Ed25519 keys/signatures and the
Ed25519->X25519 ECDH match python-cryptography; the whole app run against scripted radio traffic
built independently in Python from the MeshCore packet/payload docs (adverts, bad signature, channel
messages, DMs, acks, PATH ack, malformed packets), including what the app transmits.

Also verified: it compiles to wasm and runs on a Heltec V4 (identity creation, UI and commands). Needs firmware 0.9+.

NOT yet verified in this repo: interoperability with real MeshCore nodes over the air (the radio settings, sync word and
ack format below are assumptions to confirm), and how long the crypto takes in the interpreter (identity creation and each
advert signature / first DM to a contact are slow; adverts are not signature-checked unless `/verify on`).

Assumptions to confirm on air (all adjustable or isolated in main.c): LoRa sync word 0x12 and preamble 16
(MeshCore defaults as far as I know); ack checksum = first 4 bytes of SHA-256(timestamp|flags|text|sender pubkey);
node hash bytes 0x00 / 0xFF are reserved; 1-3 byte path-hash length encoding.
