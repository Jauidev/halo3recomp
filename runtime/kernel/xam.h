#pragma once
#include <xbox.h>

#define MSGID(Area, Number) (uint32_t)((uint16_t)(Area) << 16 | (uint16_t)(Number))
#define MSG_AREA(msgid)     (((msgid) >> 16) & 0xFFFF)
#define MSG_NUMBER(msgid)   ((msgid) & 0xFFFF)

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name);
void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root);
// registers the saves already in the save folder, so the game's save enumeration lists
// them after a restart. Call once at startup.
void XamRegisterSaves();

std::string_view XamGetRootPath(const std::string_view& root);
void XamRootCreate(const std::string_view& root, const std::string_view& path);

uint32_t XamNotifyCreateListener(uint64_t qwAreas);
void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam); // i made it the fuck up
bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam);

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped);

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum);
uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped);
uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
    uint32_t extraSize, uint32_t itemCount, uint32_t flags, be<uint32_t>* handle);
uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* structure);
uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* address);
uint32_t XamFree(uint32_t address);
uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t flags,
    uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle);

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped);
uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData);
uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped);
uint32_t XamContentDelete(uint32_t dwUserIndex, const XCONTENT_DATA* pContentData, XXOVERLAPPED* pOverlapped);
uint32_t XamContentFlush(const char* szRootName, XXOVERLAPPED* pOverlapped);
uint32_t XamContentSetThumbnail(uint32_t dwUserIndex, const XCONTENT_DATA* pContentData,
    const uint8_t* pbThumbnail, uint32_t cbThumbnail, XXOVERLAPPED* pOverlapped);

uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps);
uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state);
uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration);
uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, void* keystroke);
