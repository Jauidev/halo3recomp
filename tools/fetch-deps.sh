#!/usr/bin/env bash
# Downloads the third-party code the public repository doesn't carry, at the commits the port was
# built with, and applies the port's changes (patches/). Run from the repository's root in Git Bash.
set -euo pipefail

fetch() { # folder, URL, commit
    [ -e "$1/.git" ] && return
    git clone "$2" "$1"
    git -C "$1" checkout "$3"
    git -C "$1" submodule update --init --recursive
}

fetch third_party/xenia-gpu https://github.com/xenia-canary/xenia-canary e6cd2e4
git -C third_party/xenia-gpu apply --ignore-whitespace "$PWD/patches/xenia-gpu.patch"
fetch third_party/XenonRecomp https://github.com/hedge-dev/XenonRecomp.git ddd128b
git -C third_party/XenonRecomp apply --ignore-whitespace "$PWD/patches/XenonRecomp.patch"
fetch third_party/o1heap https://github.com/pavel-kirienko/o1heap.git 388a73f
fetch third_party/unordered_dense https://github.com/martinus/unordered_dense.git 7c58a8b
fetch third_party/libadrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce

T=third_party/rexglue/thirdparty
fetch $T/XenosRecomp https://github.com/sonicnext-dev/XenosRecomp.git fb32631ee3
fetch $T/cli11 https://github.com/CLIUtils/CLI11 bfffd37e1f
fetch $T/dxc-bin https://github.com/renderbag/dxc-bin.git 737ac9f51c
fetch $T/simde https://github.com/simd-everywhere/simde.git 71fd833d96
fetch $T/smol-v https://github.com/aras-p/smol-v 9dd54c379a
fetch $T/spdlog https://github.com/gabime/spdlog.git 79524ddd08
fetch $T/spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git 04f10f650d
fetch $T/tomlplusplus https://github.com/marzer/tomlplusplus.git 30172438ce
fetch $T/volk https://github.com/zeux/volk.git 0b17a763ba
fetch $T/vulkan-headers https://github.com/KhronosGroup/Vulkan-Headers.git 49f1a381e2
fetch $T/zstd https://github.com/facebook/zstd.git f7a8bb1263
