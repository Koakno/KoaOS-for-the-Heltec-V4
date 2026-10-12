# Wasm3 (KoaOS patched)

This is wasm3 0.5.0 (MIT, see `LICENSE`) with one change in `src/m3_core.c`: the three allocator functions
(`m3_Malloc`, `m3_FreeImpl`, `m3_Realloc`) call `koa_wasm_calloc`, `koa_wasm_free` and `koa_wasm_realloc`,
which `KoaOS.ino` implements on top of PSRAM.

Install: copy this folder to `Arduino/libraries/Wasm3`. Keep only one Wasm3 folder.
Updating Wasm3 from the Library Manager overwrites the patch; re-run `tools/patch_wasm3.py <path-to-Wasm3>`.
Without the patch KoaOS refuses to start apps ("allocator not patched").
Examples and PlatformIO files from the original library were removed to keep this small.
