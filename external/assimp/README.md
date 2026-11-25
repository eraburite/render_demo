# assimp headers (vendored)

Headers only — no library, no import lib, no DLL.

## Why here

`examples/common/AssimpModelLoader.cpp` talks to assimp's **C API** and reads
`aiScene` / `aiMesh` / `aiMaterial` / `aiTexture` struct fields directly.  Those
structs are ABI, so the build must be pinned to one header set: compiling against
whatever assimp happens to be installed on the machine is how you get a loader
that works locally and reads garbage elsewhere.

This is the same arrangement as `external/tinygltf/`, and it is why `.gitignore`
carries an explicit exception for this directory (see `/external/*` there).

## Provenance

* Copied verbatim from LearnOpenGL's vendored copy: `D:/LearnOpenGL/includes/assimp/`.
* Matches the prebuilt `assimp-vc140-mt.dll` (assimp 5.x, vc140 toolset) that the
  same distribution ships in `lib/` + `dlls/`.

## Runtime

The DLL is **not** distributed with this repository (it is ~14 MB).  At runtime
`AssimpModelLoader` binds it dynamically (`LoadLibraryW` + `GetProcAddress`), so a
missing DLL only disables the assimp-backed formats — every other example keeps
working.  Search order and configuration are described in DESIGN.md §19.2:

1. `RENDER_DEMO_ASSIMP_DLL` environment variable
2. `assimp-vc140-mt.dll` next to the executable / on `PATH` (CMake copies it
   there as a POST_BUILD step when it can find it)
3. `RENDER_DEMO_ASSIMP_DLL_HINT`, injected at compile time by CMake from
   `RENDER_DEMO_ASSIMP_DIR`

## License

BSD-3-Clause, Copyright (c) 2006-2016, assimp team.  The full text is reproduced
at the top of `include/assimp/version.h` and in every other header here; it is not
duplicated in this file.
