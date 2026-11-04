#pragma once
#include <ntddk.h>
#define HC_VERSION 1u
#define HC_IOCTL CTL_CODE(0x8352u,0x800,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define HC_DEVICE L"\\Device\\TitanHidAccessClock1"
#define HC_SERVICE L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\TitanHidAccessClock"
#define HC_EXTENSION ((PVOID)(ULONG_PTR)0x48414331u)
#ifdef HA_LAB_SECONDS
#if HA_LAB_SECONDS < 1 || HA_LAB_SECONDS > 120
#error Private lab duration must be 1..120 seconds.
#endif
#define HC_SECONDS HA_LAB_SECONDS
#else
#define HC_SECONDS 3600u
#endif
#define HC_DURATION ((ULONGLONG)HC_SECONDS*10000000ull)
typedef struct HC_IDENTITY {ULONG size,version;PDRIVER_OBJECT owner;PDEVICE_OBJECT device;} HC_IDENTITY;
typedef struct HC_REQUEST {ULONG size,version,commit,reserved;} HC_REQUEST;
typedef struct HC_RESULT {ULONG size,version,started,expired;ULONGLONG first,duration;} HC_RESULT;
C_ASSERT(sizeof(HC_REQUEST)==16);
C_ASSERT(sizeof(HC_RESULT)==32);
