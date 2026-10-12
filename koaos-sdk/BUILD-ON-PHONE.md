# Building the .esp with only a phone

Apps are separate .wasm packages that the V4 downloads over Wi-Fi, so you only need a way to run `make`.
(meshcore needs firmware 0.9 or newer.)

## Option A: GitHub Actions (nothing to install)
1. Put this repo on GitHub (the website works in a phone browser).
2. Open the repo's Actions tab, pick "build", Run workflow.
3. Download the `esp-packages` artifact: a zip of every `.esp`.

## Option B: Termux
```
pkg install clang lld make python git
clang --print-targets | grep wasm     # must list wasm32; if not, use option A
cd koaos-sdk && make meshcore
python3 tools/repo_server.py dist 8000
```
Then on the V4 (same Wi-Fi or your phone's hotspot):
```
install -s <phone-ip>:8000
install meshcore
meshcore
```
