#include <ntifs.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <initguid.h>
#include <devpkey.h>
#include "ClockProtocol.h"
#include "../Protocol/HidAccessProtocol.h"

#define HA_POOL 'cAiH'
#define HA_RING 64u
C_ASSERT(sizeof(HA_HEADER)==8);
C_ASSERT(sizeof(HA_RULE)==24);
C_ASSERT(sizeof(HA_STATUS)==56);
C_ASSERT(FIELD_OFFSET(HA_DEVICE,instance_id)==16);
C_ASSERT(sizeof(HA_DEVICE)==1296);
C_ASSERT(FIELD_OFFSET(HA_EVENT,application)==40);
typedef struct DEVICE_CONTEXT {BOOLEAN control,registered;WDFTIMER timer;HA_DEVICE record;} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT,DeviceContext)
typedef struct FILE_CONTEXT {PEPROCESS owner;BOOLEAN closing;} FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT,FileContext)
static WDFWAITLOCK gate;
static DEVICE_CONTEXT* devices[HA_MAX_DEVICES];
static HA_POLICY policy;
static HA_EVENT ring[HA_RING];
static ULONG ringHead,ringCount;
static volatile LONG active;
static volatile LONG64 nextId,generation,allowedCount,deniedCount,droppedCount,sequence;
static ULONGLONG demoFirst;
static PEPROCESS ownerProcess;
static PFILE_OBJECT clockFile;
static PDEVICE_OBJECT clockDevice;
static BOOLEAN notifyRegistered;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DRIVER_UNLOAD DriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;
EVT_WDF_DEVICE_FILE_CREATE FileCreate;
EVT_WDF_FILE_CLEANUP FileCleanup;
EVT_WDF_FILE_CLOSE FileClose;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL IoControl;
EVT_WDF_TIMER DemoTimer;
EVT_WDF_REQUEST_COMPLETION_ROUTINE ForwardCreateComplete;

static ULONGLONG Now(void){ULONGLONG qpc;return KeQueryInterruptTimePrecise(&qpc);}
static void HaLock(void){(void)WdfWaitLockAcquire(gate,NULL);}
static void HaUnlock(void){WdfWaitLockRelease(gate);}
static void ResetLocked(void){InterlockedExchange(&active,0);RtlZeroMemory(&policy,sizeof(policy));InterlockedIncrement64(&generation);}
static BOOLEAN OwnerValidLocked(const FILE_CONTEXT* file){return file&&!file->closing&&ownerProcess==file->owner;}
static BOOLEAN ExpiredLocked(void){ULONGLONG now=Now();return demoFirst&&(now<demoFirst||now-demoFirst>=HC_DURATION);}
static void CopyWide(uint16_t* dest,ULONG capacity,const WCHAR* source,ULONG chars){ULONG i;for(i=0;i+1<capacity&&i<chars&&source[i];++i)dest[i]=(uint16_t)source[i];dest[i]=0;}
static ULONG LengthWide(const uint16_t* text,ULONG capacity){ULONG i;for(i=0;i<capacity&&text[i];++i){}return i;}
static BOOLEAN Equal(const WCHAR* a,const WCHAR* b){UNICODE_STRING aa,bb;RtlInitUnicodeString(&aa,a);RtlInitUnicodeString(&bb,b);return RtlEqualUnicodeString(&aa,&bb,TRUE);}

static PVOID Property(PDEVICE_OBJECT pdo,DEVICE_REGISTRY_PROPERTY key,ULONG* bytes){
    ULONG needed=0;PVOID data;NTSTATUS status=IoGetDeviceProperty(pdo,key,0,NULL,&needed);
    if((status!=STATUS_BUFFER_TOO_SMALL&&status!=STATUS_BUFFER_OVERFLOW)||needed<sizeof(WCHAR)||needed>32768)return NULL;
    data=ExAllocatePool2(POOL_FLAG_PAGED,needed+2*sizeof(WCHAR),HA_POOL);if(!data)return NULL;
    status=IoGetDeviceProperty(pdo,key,needed,data,&needed);
    if(!NT_SUCCESS(status)||needed>32768){ExFreePoolWithTag(data,HA_POOL);return NULL;}
    ((WCHAR*)data)[needed/sizeof(WCHAR)]=0;((WCHAR*)data)[needed/sizeof(WCHAR)+1]=0;*bytes=needed;return data;
}
static BOOLEAN HasMulti(const WCHAR* data,ULONG bytes,const WCHAR* target){
    ULONG offset=0,count=bytes/sizeof(WCHAR);
    while(offset<count&&data[offset]){ULONG begin=offset;while(offset<count&&data[offset])++offset;if(offset==count)return FALSE;if(Equal(data+begin,target))return TRUE;++offset;}return FALSE;
}
static USHORT Hex4(const WCHAR* text){ULONG i,value=0;for(i=0;i<4;++i){WCHAR c=text[i];ULONG n;if(c>=L'0'&&c<=L'9')n=c-L'0';else if(c>=L'A'&&c<=L'F')n=c-L'A'+10;else if(c>=L'a'&&c<=L'f')n=c-L'a'+10;else return 0;value=(value<<4)|n;}return (USHORT)value;}
static void VidPid(HA_DEVICE* record){ULONG i,n=LengthWide(record->instance_id,HA_PATH_CHARS);for(i=0;i+8<=n;++i){const WCHAR* p=(const WCHAR*)record->instance_id+i;if((p[0]==L'V'||p[0]==L'v')&&(p[1]==L'I'||p[1]==L'i')&&(p[2]==L'D'||p[2]==L'd')&&p[3]==L'_')record->vid=Hex4(p+4);if((p[0]==L'P'||p[0]==L'p')&&(p[1]==L'I'||p[1]==L'i')&&(p[2]==L'D'||p[2]==L'd')&&p[3]==L'_')record->pid=Hex4(p+4);}}
static BOOLEAN IsRoot(PDEVICE_OBJECT pdo){ULONG bytes=0;WCHAR* ids=Property(pdo,DevicePropertyHardwareID,&bytes);BOOLEAN result=ids&&HasMulti(ids,bytes,L"ROOT\\TITANHIDACCESS");if(ids)ExFreePoolWithTag(ids,HA_POOL);return result;}
static void Describe(PDEVICE_OBJECT pdo,HA_DEVICE* record){
    ULONG bytes=0;WCHAR* value;
    // Instance ID is retrieved from the exact PDO, not parsed from its name.
    {
        DEVPROPTYPE type=0;ULONG needed=0;NTSTATUS status;
        status=IoGetDevicePropertyData(pdo,&DEVPKEY_Device_InstanceId,LOCALE_NEUTRAL,0,0,NULL,&needed,&type);
        if((status==STATUS_BUFFER_TOO_SMALL||status==STATUS_BUFFER_OVERFLOW)&&needed>=2&&needed<=HA_PATH_CHARS*sizeof(WCHAR)){
            WCHAR* id=ExAllocatePool2(POOL_FLAG_PAGED,needed+sizeof(WCHAR),HA_POOL);
            if(id){status=IoGetDevicePropertyData(pdo,&DEVPKEY_Device_InstanceId,LOCALE_NEUTRAL,0,needed,id,&needed,&type);if(NT_SUCCESS(status)&&type==DEVPROP_TYPE_STRING)CopyWide(record->instance_id,HA_PATH_CHARS,id,needed/sizeof(WCHAR));ExFreePoolWithTag(id,HA_POOL);}
        }
    }
    value=Property(pdo,DevicePropertyFriendlyName,&bytes);if(!value)value=Property(pdo,DevicePropertyDeviceDescription,&bytes);
    if(value){CopyWide(record->name,HA_NAME_CHARS,value,bytes/sizeof(WCHAR));ExFreePoolWithTag(value,HA_POOL);}
    value=Property(pdo,DevicePropertyClassGuid,&bytes);
    if(value){
        if(Equal(value,L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}"))record->flags=HA_DEVICE_HID|HA_DEVICE_PROTECTED;
        else if(Equal(value,L"{D61CA365-5AF4-4486-998B-9DB4734C6CA3}"))record->flags=HA_DEVICE_XUSB|HA_DEVICE_SUPPORTED;
        else if(Equal(value,L"{05F5CFE2-4733-4950-A6BB-07AAD01A3A84}"))record->flags=HA_DEVICE_GIP|HA_DEVICE_SUPPORTED;
        ExFreePoolWithTag(value,HA_POOL);
    }
    if(record->flags&HA_DEVICE_HID){
        const DEVICE_REGISTRY_PROPERTY keys[]={DevicePropertyHardwareID,DevicePropertyCompatibleIDs};
        BOOLEAN critical=FALSE,gamepad=FALSE;ULONG keyIndex;
        // Windows HID collection PDOs can advertise usage IDs in HardwareID
        // while CompatibleIDs is empty. Protect critical usages in either list.
        for(keyIndex=0;keyIndex<RTL_NUMBER_OF(keys);++keyIndex){
            value=Property(pdo,keys[keyIndex],&bytes);
            if(value){
                critical|=HasMulti(value,bytes,L"HID_DEVICE_SYSTEM_KEYBOARD")||HasMulti(value,bytes,L"HID_DEVICE_SYSTEM_MOUSE")||HasMulti(value,bytes,L"HID_DEVICE_UP:0001_U:0006")||HasMulti(value,bytes,L"HID_DEVICE_UP:0001_U:0002");
                gamepad|=HasMulti(value,bytes,L"HID_DEVICE_UP:0001_U:0004")||HasMulti(value,bytes,L"HID_DEVICE_UP:0001_U:0005");
                ExFreePoolWithTag(value,HA_POOL);
            }
        }
        if(gamepad&&!critical)record->flags=HA_DEVICE_HID|HA_DEVICE_SUPPORTED;
    }
    if(!record->instance_id[0])record->flags=(record->flags&~HA_DEVICE_SUPPORTED)|HA_DEVICE_PROTECTED;
    VidPid(record);
}

static NTSTATUS ClockConnect(BOOLEAN load){
    HC_IDENTITY* identity;NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name,HC_DEVICE);
    if(clockFile)return STATUS_SUCCESS;
    status=IoGetDeviceObjectPointer((PUNICODE_STRING)&name,FILE_READ_DATA|FILE_WRITE_DATA,&clockFile,&clockDevice);
    if(!NT_SUCCESS(status)&&load){DECLARE_CONST_UNICODE_STRING(service,HC_SERVICE);(void)ZwLoadDriver((PUNICODE_STRING)&service);status=IoGetDeviceObjectPointer((PUNICODE_STRING)&name,FILE_READ_DATA|FILE_WRITE_DATA,&clockFile,&clockDevice);}
    if(!NT_SUCCESS(status)){clockFile=NULL;clockDevice=NULL;return status;}
    identity=IoGetDriverObjectExtension(clockDevice->DriverObject,HC_EXTENSION);
    if(!identity||identity->size!=sizeof(*identity)||identity->version!=HC_VERSION||identity->owner!=clockDevice->DriverObject||identity->device!=clockDevice){ObDereferenceObject(clockFile);clockFile=NULL;clockDevice=NULL;return STATUS_OBJECT_TYPE_MISMATCH;}
    return STATUS_SUCCESS;
}
static NTSTATUS ClockRead(BOOLEAN commit,HC_RESULT* result){
    HC_REQUEST request={sizeof(request),HC_VERSION,commit?1u:0u,0};KEVENT event;IO_STATUS_BLOCK io={0};PIRP irp;NTSTATUS status;
    status=ClockConnect(commit);if(!NT_SUCCESS(status))return status;
    RtlZeroMemory(result,sizeof(*result));KeInitializeEvent(&event,NotificationEvent,FALSE);
    irp=IoBuildDeviceIoControlRequest(HC_IOCTL,clockDevice,&request,sizeof(request),result,sizeof(*result),TRUE,&event,&io);if(!irp)return STATUS_INSUFFICIENT_RESOURCES;
    status=IoCallDriver(clockDevice,irp);if(status==STATUS_PENDING){KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);status=io.Status;}
    if(NT_SUCCESS(status)&&(io.Information!=sizeof(*result)||result->size!=sizeof(*result)||result->version!=HC_VERSION||result->duration!=HC_DURATION||result->started>1||result->expired>1||(!result->started&&(result->first||result->expired))))return STATUS_DATA_ERROR;
    return status;
}
static DEVICE_CONTEXT* FindDevice(ULONGLONG id){ULONG i;for(i=0;i<HA_MAX_DEVICES;++i)if(devices[i]&&devices[i]->record.id==id)return devices[i];return NULL;}
static NTSTATUS ValidatePolicy(const HA_POLICY* input){
    ULONG i,j,k;
    if(input->size!=sizeof(*input)||input->version!=HA_VERSION||input->enabled>1||input->reserved||input->rule_count>HA_MAX_RULES||input->app_count>HA_MAX_APPS)return STATUS_INVALID_PARAMETER;
    if(!input->enabled&&input->rule_count)return STATUS_INVALID_PARAMETER;
    for(i=0;i<input->app_count;++i){
        ULONG length=LengthWide(input->apps[i].path,HA_PATH_CHARS);UNICODE_STRING path,prefix;
        if(length<9||length>=HA_PATH_CHARS)return STATUS_INVALID_PARAMETER;
        path.Buffer=(PWCHAR)input->apps[i].path;path.Length=(USHORT)(length*sizeof(WCHAR));path.MaximumLength=path.Length;RtlInitUnicodeString(&prefix,L"\\Device\\");
        if(!RtlPrefixUnicodeString(&prefix,&path,TRUE))return STATUS_INVALID_PARAMETER;
        for(j=0;j<length;++j)if(input->apps[i].path[j]<32)return STATUS_INVALID_PARAMETER;
        for(j=length+1;j<HA_PATH_CHARS;++j)if(input->apps[i].path[j])return STATUS_INVALID_PARAMETER;
        for(j=0;j<i;++j)if(Equal((const WCHAR*)input->apps[i].path,(const WCHAR*)input->apps[j].path))return STATUS_INVALID_PARAMETER;
    }
    for(i=0;i<input->rule_count;++i){
        DEVICE_CONTEXT* context=FindDevice(input->rules[i].device_id);
        if(!context||!(context->record.flags&HA_DEVICE_SUPPORTED)||(context->record.flags&HA_DEVICE_PROTECTED))return STATUS_ACCESS_DENIED;
        if(input->rules[i].reserved||input->rules[i].mode>HA_MODE_BLOCK_ALL||input->rules[i].applications>>input->app_count)return STATUS_INVALID_PARAMETER;
        if(input->rules[i].mode==HA_MODE_BLOCK_ALL&&input->rules[i].applications)return STATUS_INVALID_PARAMETER;
        for(k=0;k<i;++k)if(input->rules[k].device_id==input->rules[i].device_id)return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}
static void ProcessNotify(PEPROCESS process,HANDLE id,PPS_CREATE_NOTIFY_INFO info){
    UNREFERENCED_PARAMETER(id);
    if(!info){HaLock();if(ownerProcess==process){ResetLocked();ownerProcess=NULL;}HaUnlock();}
}
void DemoTimer(WDFTIMER timer){UNREFERENCED_PARAMETER(timer);HaLock();if(InterlockedCompareExchange(&active,0,0)&&ExpiredLocked())ResetLocked();HaUnlock();}
void DeviceCleanup(WDFOBJECT object){
    DEVICE_CONTEXT* context=DeviceContext(object);ULONG i;
    HaLock();
    if(context->control){ResetLocked();ownerProcess=NULL;}
    else if(context->registered){for(i=0;i<HA_MAX_DEVICES;++i)if(devices[i]==context){devices[i]=NULL;break;}}
    HaUnlock();
}
void FileCleanup(WDFFILEOBJECT object){FILE_CONTEXT* file=FileContext(object);HaLock();file->closing=TRUE;if(ownerProcess==file->owner){ResetLocked();ownerProcess=NULL;}HaUnlock();}
void FileClose(WDFFILEOBJECT object){FILE_CONTEXT* file=FileContext(object);if(file->owner){ObDereferenceObject(file->owner);file->owner=NULL;}}
void ForwardCreateComplete(WDFREQUEST request,WDFIOTARGET target,PWDF_REQUEST_COMPLETION_PARAMS completion,WDFCONTEXT context){
    UNREFERENCED_PARAMETER(target);UNREFERENCED_PARAMETER(context);
    // KMDF must post-process CREATE completion when it owns a WDFFILEOBJECT.
    // Forward the exact lower-stack result without inspecting any report data.
    WdfRequestCompleteWithInformation(request,completion->IoStatus.Status,completion->IoStatus.Information);
}
static void ForwardCreate(WDFDEVICE device,WDFREQUEST request){
    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request,ForwardCreateComplete,NULL);
    if(!WdfRequestSend(request,WdfDeviceGetIoTarget(device),WDF_NO_SEND_OPTIONS))WdfRequestComplete(request,WdfRequestGetStatus(request));
}
void FileCreate(WDFDEVICE device,WDFREQUEST request,WDFFILEOBJECT object){
    DEVICE_CONTEXT* context=DeviceContext(device);PIRP irp=WdfRequestWdmGetIrp(request);PEPROCESS process=IoGetRequestorProcess(irp);PUNICODE_STRING image=NULL;BOOLEAN deny=FALSE,matched=FALSE;ULONG reason=HA_REASON_NO_RULE,i;HA_RULE* rule=NULL;ULONGLONG appMask=0;
    if(context->control){
        FILE_CONTEXT* file=FileContext(object);PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(irp);
        PUNICODE_STRING name=WdfFileObjectGetFileName(object);
        ACCESS_MASK access=stack->Parameters.Create.SecurityContext?stack->Parameters.Create.SecurityContext->DesiredAccess:0;
        GENERIC_MAPPING mapping={FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
        RtlMapGenericMask(&access,&mapping);
        // Zero-access opens must not reserve the exclusive policy owner. The
        // control API has no file namespace; suffix opens are never meaningful.
        if(!process||(access&(FILE_READ_DATA|FILE_WRITE_DATA))!=(FILE_READ_DATA|FILE_WRITE_DATA)||(name&&name->Length)){
            WdfRequestComplete(request,STATUS_ACCESS_DENIED);return;
        }
        ObReferenceObject(process);file->owner=process;file->closing=FALSE;HaLock();ownerProcess=process;HaUnlock();WdfRequestComplete(request,STATUS_SUCCESS);return;
    }
    if(!(context->record.flags&HA_DEVICE_SUPPORTED)||irp->RequestorMode==KernelMode||!InterlockedCompareExchange(&active,0,0)){ForwardCreate(device,request);return;}
    if(process)(void)SeLocateProcessImageName(process,&image);
    HaLock();
    if(InterlockedCompareExchange(&active,0,0)&&ExpiredLocked())ResetLocked();
    if(InterlockedCompareExchange(&active,0,0))for(i=0;i<policy.rule_count;++i)if(policy.rules[i].device_id==context->record.id){rule=&policy.rules[i];break;}
    if(rule){
        matched=TRUE;
        if(image)for(i=0;i<policy.app_count;++i){UNICODE_STRING wanted;RtlInitUnicodeString(&wanted,(const WCHAR*)policy.apps[i].path);if(RtlEqualUnicodeString(image,&wanted,TRUE))appMask|=1ull<<i;}
        if(rule->mode==HA_MODE_BLOCK_ALL){deny=TRUE;reason=HA_REASON_BLOCK_ALL;}
        else if(rule->mode==HA_MODE_ALLOW_LISTED){deny=(rule->applications&appMask)==0;reason=deny?HA_REASON_LIST_DENY:HA_REASON_LIST_ALLOW;}
        else{deny=(rule->applications&appMask)!=0;reason=deny?HA_REASON_LIST_DENY:HA_REASON_LIST_ALLOW;}
        if(!image)reason=HA_REASON_UNKNOWN_IMAGE;
        {
            ULONG index=(ringHead+ringCount)%HA_RING;HA_EVENT* event;
            if(ringCount==HA_RING){index=ringHead;ringHead=(ringHead+1)%HA_RING;InterlockedIncrement64(&droppedCount);}else ++ringCount;
            event=&ring[index];RtlZeroMemory(event,sizeof(*event));event->sequence=(ULONGLONG)InterlockedIncrement64(&sequence);event->timestamp_100ns=Now();event->device_id=context->record.id;event->process_id=IoGetRequestorProcessId(irp);event->flags=deny?HA_EVENT_DENIED:0;event->reason=reason;
            if(image)CopyWide(event->application,HA_PATH_CHARS,image->Buffer,image->Length/sizeof(WCHAR));
        }
    }
    HaUnlock();if(image)ExFreePool(image);
    if(matched){if(deny)InterlockedIncrement64(&deniedCount);else InterlockedIncrement64(&allowedCount);}
    if(deny)WdfRequestComplete(request,STATUS_ACCESS_DENIED);else ForwardCreate(device,request);
}

void IoControl(WDFQUEUE queue,WDFREQUEST request,size_t outputLength,size_t inputLength,ULONG code){
    WDFDEVICE device=WdfIoQueueGetDevice(queue);DEVICE_CONTEXT* context=DeviceContext(device);WDFFILEOBJECT object=WdfRequestGetFileObject(request);FILE_CONTEXT* file=object?FileContext(object):NULL;NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;size_t bytes=0;PVOID input=NULL,output=NULL;ULONG i;
    if(!file||IoGetRequestorProcess(WdfRequestWdmGetIrp(request))!=file->owner){WdfRequestComplete(request,STATUS_ACCESS_DENIED);return;}
    HaLock();status=OwnerValidLocked(file)?STATUS_SUCCESS:STATUS_FILE_CLOSED;HaUnlock();
    if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
    status=WdfRequestRetrieveInputBuffer(request,sizeof(HA_HEADER),&input,NULL);if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
    if(code==HA_IOCTL_APPLY){
        HC_RESULT budget;
        if(inputLength!=sizeof(HA_POLICY)||outputLength){WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE);return;}
        HaLock();status=ValidatePolicy((const HA_POLICY*)input);HaUnlock();
        if(NT_SUCCESS(status)&&((HA_POLICY*)input)->enabled&&((HA_POLICY*)input)->rule_count)status=ClockConnect(TRUE);
        if(NT_SUCCESS(status)){
            HaLock();status=ValidatePolicy((const HA_POLICY*)input);
            if(NT_SUCCESS(status)&&OwnerValidLocked(file)){
                if(!((HA_POLICY*)input)->enabled||!((HA_POLICY*)input)->rule_count)ResetLocked();
                else{
                    status=ClockRead(TRUE,&budget);
                    if(NT_SUCCESS(status)&&budget.expired)status=STATUS_LICENSE_VIOLATION;
                    if(NT_SUCCESS(status)){
                        ULONGLONG now=Now(),remaining;
                        demoFirst=budget.first;remaining=now>=demoFirst&&now-demoFirst<HC_DURATION?HC_DURATION-(now-demoFirst):0;
                        if(!remaining)status=STATUS_LICENSE_VIOLATION;
                        else{RtlCopyMemory(&policy,input,sizeof(policy));InterlockedIncrement64(&generation);InterlockedExchange(&active,1);WdfTimerStart(context->timer,-(LONGLONG)remaining);}
                    }
                }
            }else if(NT_SUCCESS(status))status=STATUS_FILE_CLOSED;
            HaUnlock();
        }
        WdfRequestComplete(request,status);return;
    }
    if(inputLength!=sizeof(HA_HEADER)||((HA_HEADER*)input)->size!=sizeof(HA_HEADER)||((HA_HEADER*)input)->version!=HA_VERSION){WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
    if(code==HA_IOCTL_RESET){if(outputLength){WdfRequestComplete(request,STATUS_INVALID_BUFFER_SIZE);return;}HaLock();status=OwnerValidLocked(file)?STATUS_SUCCESS:STATUS_FILE_CLOSED;if(NT_SUCCESS(status))ResetLocked();HaUnlock();}
    else if(code==HA_IOCTL_STATUS){
        HA_STATUS* result;HC_RESULT budget;NTSTATUS clockStatus=ClockRead(FALSE,&budget);
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(HA_STATUS),&output,NULL);
        if(NT_SUCCESS(status)){
            result=output;RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=HA_VERSION;
            HaLock();if(!OwnerValidLocked(file)){HaUnlock();WdfRequestComplete(request,STATUS_FILE_CLOSED);return;}if(NT_SUCCESS(clockStatus)&&budget.started)demoFirst=budget.first;if(InterlockedCompareExchange(&active,0,0)&&ExpiredLocked())ResetLocked();if(active)result->flags|=HA_STATUS_ACTIVE;
            if(ExpiredLocked())result->flags|=HA_STATUS_EXPIRED;
            for(i=0;i<HA_MAX_DEVICES;++i)if(devices[i])++result->device_count;
            {ULONGLONG now=Now();result->remaining_100ns=!demoFirst?HC_DURATION:(now>=demoFirst&&now-demoFirst<HC_DURATION?HC_DURATION-(now-demoFirst):0);}
            result->generation=(ULONGLONG)generation;result->allowed_opens=(ULONGLONG)allowedCount;result->denied_opens=(ULONGLONG)deniedCount;result->dropped_events=(ULONGLONG)droppedCount;HaUnlock();bytes=sizeof(*result);
        }
    }else if(code==HA_IOCTL_DEVICES){
        HA_DEVICES* result;status=WdfRequestRetrieveOutputBuffer(request,sizeof(HA_DEVICES),&output,NULL);
        if(NT_SUCCESS(status)){result=output;RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=HA_VERSION;HaLock();if(!OwnerValidLocked(file)){HaUnlock();WdfRequestComplete(request,STATUS_FILE_CLOSED);return;}for(i=0;i<HA_MAX_DEVICES;++i)if(devices[i])RtlCopyMemory(&result->devices[result->count++],&devices[i]->record,sizeof(HA_DEVICE));HaUnlock();bytes=sizeof(*result);}
    }else if(code==HA_IOCTL_EVENTS){
        HA_EVENTS* result;status=WdfRequestRetrieveOutputBuffer(request,sizeof(HA_EVENTS),&output,NULL);
        if(NT_SUCCESS(status)){result=output;RtlZeroMemory(result,sizeof(*result));result->size=sizeof(*result);result->version=HA_VERSION;HaLock();if(!OwnerValidLocked(file)){HaUnlock();WdfRequestComplete(request,STATUS_FILE_CLOSED);return;}while(ringCount&&result->count<HA_EVENT_BATCH){RtlCopyMemory(&result->events[result->count++],&ring[ringHead],sizeof(HA_EVENT));ringHead=(ringHead+1)%HA_RING;--ringCount;}result->dropped=(ULONGLONG)droppedCount;HaUnlock();bytes=sizeof(*result);}
    }else status=STATUS_INVALID_DEVICE_REQUEST;
    WdfRequestCompleteWithInformation(request,status,NT_SUCCESS(status)?bytes:0);
}

NTSTATUS DeviceAdd(WDFDRIVER driver,PWDFDEVICE_INIT init){
    WDFDEVICE device;DEVICE_CONTEXT* context;PDEVICE_OBJECT pdo=WdfFdoInitWdmGetPhysicalDevice(init);BOOLEAN control=IsRoot(pdo);WDF_OBJECT_ATTRIBUTES attributes,fileAttributes;WDF_FILEOBJECT_CONFIG files;NTSTATUS status;ULONG i;
    UNREFERENCED_PARAMETER(driver);
    if(control){
        DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\TitanHidAccess");
        DECLARE_CONST_UNICODE_STRING(security,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        const GUID securityClass={0x77bfc52f,0x30d2,0x4ccd,{0x8c,0xaa,0xd3,0x27,0xea,0xdb,0xc5,0x9a}};
        WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);WdfDeviceInitSetExclusive(init,TRUE);WdfDeviceInitSetDeviceClass(init,&securityClass);
        WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
        status=WdfDeviceInitAssignName(init,&name);if(!NT_SUCCESS(status))return status;
        status=WdfDeviceInitAssignSDDLString(init,&security);if(!NT_SUCCESS(status))return status;
    }else WdfFdoInitSetFilter(init);
    WDF_FILEOBJECT_CONFIG_INIT(&files,FileCreate,control?FileClose:NULL,control?FileCleanup:NULL);
    files.AutoForwardCleanupClose=control?WdfFalse:WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes,FILE_CONTEXT);fileAttributes.ExecutionLevel=WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(init,&files,control?&fileAttributes:WDF_NO_OBJECT_ATTRIBUTES);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,DEVICE_CONTEXT);attributes.ExecutionLevel=WdfExecutionLevelPassive;attributes.SynchronizationScope=WdfSynchronizationScopeNone;attributes.EvtCleanupCallback=DeviceCleanup;
    status=WdfDeviceCreate(&init,&attributes,&device);if(!NT_SUCCESS(status))return status;
    context=DeviceContext(device);context->control=control;
    if(control){
        WDF_IO_QUEUE_CONFIG queue;WDF_TIMER_CONFIG timer;WDF_OBJECT_ATTRIBUTES timerAttributes;DECLARE_CONST_UNICODE_STRING(link,L"\\DosDevices\\TitanHidAccess");
        status=WdfDeviceCreateSymbolicLink(device,&link);if(!NT_SUCCESS(status))return status;
        WDF_TIMER_CONFIG_INIT(&timer,DemoTimer);timer.AutomaticSerialization=FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);timerAttributes.ParentObject=device;timerAttributes.ExecutionLevel=WdfExecutionLevelPassive;
        status=WdfTimerCreate(&timer,&timerAttributes,&context->timer);if(!NT_SUCCESS(status))return status;
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchSequential);queue.EvtIoDeviceControl=IoControl;return WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    }
    Describe(pdo,&context->record);context->record.id=(ULONGLONG)InterlockedIncrement64(&nextId);
    HaLock();for(i=0;i<HA_MAX_DEVICES;++i)if(!devices[i]){devices[i]=context;context->registered=TRUE;break;}HaUnlock();
    // Exhausting inventory capacity keeps this untracked stack transparent.
    if(!context->registered)context->record.flags=HA_DEVICE_PROTECTED;
    return STATUS_SUCCESS;
}
void DriverUnload(WDFDRIVER driver){UNREFERENCED_PARAMETER(driver);if(notifyRegistered){(void)PsSetCreateProcessNotifyRoutineEx(ProcessNotify,TRUE);notifyRegistered=FALSE;}if(clockFile){ObDereferenceObject(clockFile);clockFile=NULL;clockDevice=NULL;}}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING registry){
    WDF_DRIVER_CONFIG config;WDFDRIVER framework;WDF_OBJECT_ATTRIBUTES attributes,lockAttributes;NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config,DeviceAdd);config.EvtDriverUnload=DriverUnload;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ExecutionLevel=WdfExecutionLevelPassive;
    status=WdfDriverCreate(driver,registry,&attributes,&config,&framework);if(!NT_SUCCESS(status))return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);lockAttributes.ParentObject=framework;
    status=WdfWaitLockCreate(&lockAttributes,&gate);if(!NT_SUCCESS(status))return status;
    nextId=(LONG64)Now();status=PsSetCreateProcessNotifyRoutineEx(ProcessNotify,FALSE);if(NT_SUCCESS(status))notifyRegistered=TRUE;return status;
}
