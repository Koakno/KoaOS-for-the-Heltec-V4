#!/usr/bin/env python3
"""Route wasm3's allocator through KoaOS's PSRAM hooks (koa_wasm_calloc/realloc/free).

Run once after installing the "Wasm3" library (Arduino Library Manager):
    python3 patch_wasm3.py                 # finds Arduino/libraries/Wasm3 in the usual places
    python3 patch_wasm3.py /path/to/Wasm3  # or give the library folder
Safe to run twice. Works with the Library Manager version (m3_Malloc / m3_FreeImpl / m3_Realloc)
and with upstream wasm3 master (m3_Malloc_Impl / m3_Free_Impl / m3_Realloc_Impl).
"""
import os, sys

MARK = "/* KOAOS: allocator hooks */"
DECL = (MARK + "\n"
        "extern void* koa_wasm_calloc (size_t n);\n"
        "extern void* koa_wasm_realloc (void* p, size_t newSize, size_t oldSize);\n"
        "extern void  koa_wasm_free (void* p);\n")

def candidates():
    h = os.path.expanduser("~")
    for d in (os.path.join(h, "Arduino", "libraries"),
              os.path.join(h, "Documents", "Arduino", "libraries"),
              os.path.join(h, "OneDrive", "Documents", "Arduino", "libraries")):
        yield os.path.join(d, "Wasm3")

def main():
    lib = sys.argv[1] if len(sys.argv) > 1 else next((c for c in candidates() if os.path.isdir(c)), None)
    if not lib or not os.path.isdir(lib):
        sys.exit("Wasm3 library folder not found; pass it as an argument")
    path = os.path.join(lib, "src", "m3_core.c")
    s = open(path, encoding="utf-8").read()
    if MARK in s:
        print("already patched:", path); return

    # (old, new) pairs; each old string must exist exactly once for its variant
    lm = [("void * ptr = calloc (i_size, 1);",                      "void * ptr = koa_wasm_calloc (i_size);"),
          ("free (io_ptr);",                                        "koa_wasm_free (io_ptr);"),
          ("void * newPtr = realloc (i_ptr, i_newSize);",           "void * newPtr = koa_wasm_realloc (i_ptr, i_newSize, i_oldSize);")]
    up = [("return calloc (i_size, 1);",                            "return koa_wasm_calloc (i_size);"),
          ("free (i_ptr);",                                          "koa_wasm_free (i_ptr);"),
          ("realloc (i_ptr, i_newSize)",                            "koa_wasm_realloc (i_ptr, i_newSize, i_oldSize)")]
    for pairs in (lm, up):
        if all(s.count(o) == 1 for o, _ in pairs):
            for o, n in pairs: s = s.replace(o, n)
            break
    else:
        sys.exit("m3_core.c doesn't look like a version I know; patch the three allocator functions by hand")

    # declarations go after the first #include block
    idx = s.index('#include')
    end = s.rfind('#include', 0, s.index('\n\n', idx) if '\n\n' in s[idx:] else len(s))
    nl = s.index('\n', end) + 1
    s = s[:nl] + DECL + s[nl:]
    open(path + ".orig", "w", encoding="utf-8").write(open(path, encoding="utf-8").read()) if not os.path.exists(path + ".orig") else None
    open(path, "w", encoding="utf-8").write(s)
    print("patched:", path)

main()
