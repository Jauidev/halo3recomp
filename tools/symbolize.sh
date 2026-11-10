#!/bin/sh
# Symbolizes the "#NN +0xOFFSET" frames of a crash report printed by runtime/os/crash.cpp.
# Usage: tools/symbolize.sh <log> [binary]   (default binary: build/x64-gta4/gta4.exe)
# Frames outside the binary (ntdll, the CRT DLLs) come out as wrong names or ??.
LOG=$1
BIN=${2:-build/x64-gta4/gta4.exe}
SYM="$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe"
grep -E '^  (#[0-9]+ \+0x|\.\.\.)' "$LOG" | while read -r idx off rest; do
    case $idx in
        ...) echo "  ... $off $rest"; continue ;;
    esac
    name=$("$SYM" --obj="$BIN" --relative-address --no-inlines "${off#+}" | head -1)
    echo "  $idx $off $name"
done
