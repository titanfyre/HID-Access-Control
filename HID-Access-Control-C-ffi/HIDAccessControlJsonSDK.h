/* SPDX-License-Identifier: MIT */
#ifndef HID_ACCESS_CONTROL_CSDK_H
#define HID_ACCESS_CONTROL_CSDK_H
#include <stdint.h>
#if !defined(_WIN32) || !defined(_WIN64)
#error This SDK requires Windows x64.
#endif
#ifdef __cplusplus
#define HID_ACCESS_CONTROL_CSDK_NOEXCEPT noexcept
extern "C" {
#else
#define HID_ACCESS_CONTROL_CSDK_NOEXCEPT
#endif
#if defined(HID_ACCESS_CONTROL_CSDK_BUILD)
#define HID_ACCESS_CONTROL_CSDK_API __declspec(dllexport)
#else
#define HID_ACCESS_CONTROL_CSDK_API __declspec(dllimport)
#endif
/* open is inert. One active session per loaded product DLL.
   call accepts length-delimited UTF-8 JSON and returns an allocated UTF-8
   response, or NULL on allocation failure. Always free with this same DLL.
   Session/operation handles are scoped to the client that returned them. */
HID_ACCESS_CONTROL_CSDK_API uint32_t hid_access_control_json_open(uint64_t *session) HID_ACCESS_CONTROL_CSDK_NOEXCEPT;
HID_ACCESS_CONTROL_CSDK_API uint32_t hid_access_control_json_close(uint64_t session) HID_ACCESS_CONTROL_CSDK_NOEXCEPT;
HID_ACCESS_CONTROL_CSDK_API char *hid_access_control_json_call(uint64_t session,const char *request,uint32_t length) HID_ACCESS_CONTROL_CSDK_NOEXCEPT;
HID_ACCESS_CONTROL_CSDK_API void hid_access_control_json_free(char *response) HID_ACCESS_CONTROL_CSDK_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef HID_ACCESS_CONTROL_CSDK_NOEXCEPT
#endif
