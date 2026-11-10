#pragma once

// Loads default.xex into guest memory and binds its imports. Returns the entry point (0 on failure).
uint32_t LdrLoadModule(const std::filesystem::path& path);

// The executable's static TLS (XEX_HEADER_TLS_INFO): its __declspec(thread) data's template
// (raw bytes in the image) and size, which every thread's TLS block starts with. False without one.
bool LdrGetTlsTemplate(uint32_t& rawAddress, uint32_t& rawSize, uint32_t& dataSize);
