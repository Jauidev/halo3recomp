#pragma once

// Windows (PC build only). windows.h defines near, far and small as macros, and later
// Windows headers need them, so they stay; renderer sources that use those words as names are
// patched instead (marked xport). Including windows.h first keeps that order stable.
#include <windows.h>
