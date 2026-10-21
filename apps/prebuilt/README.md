# Prebuilt apps

| App | Needs | What it does |
|---|---|---|
| hellowasm | nothing | prints a banner; smallest example |
| gpswatch | gps | shows GPS fix information |
| webget | wifi | fetches a URL over HTTP/HTTPS |
| meshcore | lora | MeshCore chat/companion node (see `koaos-sdk/apps/meshcore/README.md`) |

meshcore, webget, gpswatch and hellowasm are ABI v1 packages , which run unchanged on firmware 0.10 (ABI 2). `iodemo` (GPIO/I2C) is not prebuilt: run `make iodemo` in `koaos-sdk/` and copy `dist/iodemo.esp` here if you want it. Serve this folder and install from the board:
```
python3 ../../koaos-sdk/tools/repo_server.py . 8000
# on the board:
install -s <host-ip>:8000
install -l
install meshcore
```
`meshcore` needs firmware 0.9 or newer (48 KB interpreter stack). The other three were built with the
original `-O2` flags; rebuild them with `make` in `koaos-sdk/` to get the current flags.
