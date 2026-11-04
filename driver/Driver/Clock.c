#include <ntddk.h>
#include <wdmsec.h>
#include "ClockProtocol.h"

// Product-private boot budget. Kernel callers only; no reset, user link, polling,
// worker thread or unload callback. First activation starts the elapsed budget.
static FAST_MUTEX gate;
static BOOLEAN started;
static ULONGLONG first;
DRIVER_INITIALIZE DriverEntry;
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_INTERNAL_DEVICE_CONTROL)
DRIVER_DISPATCH ClockDispatch;
NTSTATUS ClockDispatch(PDEVICE_OBJECT device,PIRP irp) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status=STATUS_ACCESS_DENIED;ULONG_PTR bytes=0;
    UNREFERENCED_PARAMETER(device);
    if(stack->MajorFunction==IRP_MJ_CREATE && irp->RequestorMode==KernelMode)status=STATUS_SUCCESS;
    else if(stack->MajorFunction==IRP_MJ_CLOSE||stack->MajorFunction==IRP_MJ_CLEANUP)status=STATUS_SUCCESS;
    else if(stack->MajorFunction==IRP_MJ_INTERNAL_DEVICE_CONTROL&&irp->RequestorMode==KernelMode&&KeGetCurrentIrql()==PASSIVE_LEVEL&&stack->Parameters.DeviceIoControl.IoControlCode==HC_IOCTL){
        HC_REQUEST request;HC_RESULT result;ULONGLONG qpc,now;
        if(!irp->AssociatedIrp.SystemBuffer||stack->Parameters.DeviceIoControl.InputBufferLength!=sizeof(request)||stack->Parameters.DeviceIoControl.OutputBufferLength<sizeof(result))status=STATUS_INVALID_BUFFER_SIZE;
        else{
            RtlCopyMemory(&request,irp->AssociatedIrp.SystemBuffer,sizeof(request));
            if(request.size!=sizeof(request)||request.version!=HC_VERSION||request.commit>1||request.reserved)status=STATUS_INVALID_PARAMETER;
            else{
                ExAcquireFastMutex(&gate);now=KeQueryInterruptTimePrecise(&qpc);if(!now)now=1;
                if(request.commit&&!started){first=now;started=TRUE;}
                RtlZeroMemory(&result,sizeof(result));result.size=sizeof(result);result.version=HC_VERSION;
                result.started=started;result.first=first;result.duration=HC_DURATION;
                result.expired=started&&(now<first||now-first>=HC_DURATION);
                ExReleaseFastMutex(&gate);
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer,&result,sizeof(result));bytes=sizeof(result);status=STATUS_SUCCESS;
            }
        }
    }
    irp->IoStatus.Status=status;irp->IoStatus.Information=bytes;IoCompleteRequest(irp,IO_NO_INCREMENT);return status;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING registry) {
    PDEVICE_OBJECT device=NULL;HC_IDENTITY* identity=NULL;NTSTATUS status;
    const GUID securityClass={0xb397d43c,0x6825,0x4b65,{0x8a,0x03,0xad,0x98,0x30,0xb3,0xfa,0x52}};
    DECLARE_CONST_UNICODE_STRING(name,HC_DEVICE);
    DECLARE_CONST_UNICODE_STRING(security,L"D:P(A;;GA;;;SY)");
    UNREFERENCED_PARAMETER(registry);ExInitializeFastMutex(&gate);
    driver->MajorFunction[IRP_MJ_CREATE]=ClockDispatch;
    driver->MajorFunction[IRP_MJ_CLOSE]=ClockDispatch;
    driver->MajorFunction[IRP_MJ_CLEANUP]=ClockDispatch;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]=ClockDispatch;
    driver->DriverUnload=NULL;
    status=IoAllocateDriverObjectExtension(driver,HC_EXTENSION,sizeof(*identity),(PVOID*)&identity);if(!NT_SUCCESS(status))return status;
    status=IoCreateDeviceSecure(driver,0,(PUNICODE_STRING)&name,0x8352,FILE_DEVICE_SECURE_OPEN,FALSE,(PUNICODE_STRING)&security,&securityClass,&device);if(!NT_SUCCESS(status))return status;
    identity->size=sizeof(*identity);identity->version=HC_VERSION;identity->owner=driver;identity->device=device;
    device->Flags|=DO_BUFFERED_IO;device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
}
