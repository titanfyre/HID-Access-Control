#pragma once
#ifdef _KERNEL_MODE
typedef USHORT uint16_t;
typedef ULONG uint32_t;
typedef ULONGLONG uint64_t;
#else
#include <stdint.h>
#endif

// Public wire ABI. No pointers, variable allocations, or platform-sized fields.
#define HA_VERSION 1u
#define HA_MAX_DEVICES 128u
#define HA_MAX_RULES 32u
#define HA_MAX_APPS 32u
#define HA_PATH_CHARS 512u
#define HA_NAME_CHARS 128u
#define HA_EVENT_BATCH 32u
#define HA_CONTROL_PATH L"\\\\.\\TitanHidAccess"
#define HA_SERVICE_NAME L"TitanHidAccess"
#define HA_IOCTL(N) ((0x22u << 16) | (3u << 14) | ((N) << 2))
#define HA_IOCTL_STATUS HA_IOCTL(0xA10u)
#define HA_IOCTL_DEVICES HA_IOCTL(0xA11u)
#define HA_IOCTL_APPLY HA_IOCTL(0xA12u)
#define HA_IOCTL_RESET HA_IOCTL(0xA13u)
#define HA_IOCTL_EVENTS HA_IOCTL(0xA14u)
#define HA_MODE_ALLOW_LISTED 0u
#define HA_MODE_DENY_LISTED 1u
#define HA_MODE_BLOCK_ALL 2u
#define HA_DEVICE_SUPPORTED 1u
#define HA_DEVICE_PROTECTED 2u
#define HA_DEVICE_HID 4u
#define HA_DEVICE_XUSB 8u
#define HA_DEVICE_GIP 16u
#define HA_STATUS_ACTIVE 1u
#define HA_STATUS_EXPIRED 2u
#define HA_EVENT_DENIED 1u
#define HA_REASON_NO_RULE 0u
#define HA_REASON_LIST_ALLOW 1u
#define HA_REASON_LIST_DENY 2u
#define HA_REASON_BLOCK_ALL 3u
#define HA_REASON_UNKNOWN_IMAGE 4u
#define HA_REASON_KERNEL 5u
#define HA_REASON_DEMO_EXPIRED 6u

#pragma pack(push, 8)
typedef struct HA_HEADER { uint32_t size, version; } HA_HEADER;
typedef struct HA_RULE {
    uint64_t device_id;
    uint32_t mode, reserved;
    uint64_t applications;
} HA_RULE;
typedef struct HA_APP { uint16_t path[HA_PATH_CHARS]; } HA_APP;
typedef struct HA_POLICY {
    uint32_t size, version, enabled, rule_count, app_count, reserved;
    HA_RULE rules[HA_MAX_RULES];
    HA_APP apps[HA_MAX_APPS];
} HA_POLICY;
typedef struct HA_STATUS {
    uint32_t size, version, flags, device_count;
    uint64_t generation, remaining_100ns, allowed_opens, denied_opens, dropped_events;
} HA_STATUS;
typedef struct HA_DEVICE {
    uint64_t id;
    uint32_t flags;
    uint16_t vid, pid;
    uint16_t instance_id[HA_PATH_CHARS];
    uint16_t name[HA_NAME_CHARS];
} HA_DEVICE;
typedef struct HA_DEVICES {
    uint32_t size, version, count, reserved;
    HA_DEVICE devices[HA_MAX_DEVICES];
} HA_DEVICES;
typedef struct HA_EVENT {
    uint64_t sequence, timestamp_100ns, device_id;
    uint32_t process_id, flags, reason, reserved;
    uint16_t application[HA_PATH_CHARS];
} HA_EVENT;
typedef struct HA_EVENTS {
    uint32_t size, version, count, reserved;
    uint64_t dropped;
    HA_EVENT events[HA_EVENT_BATCH];
} HA_EVENTS;
#pragma pack(pop)
