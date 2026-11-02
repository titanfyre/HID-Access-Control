#include "Setup.h"
#include "../Protocol/HidAccessProtocol.h"
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <sddl.h>
#include <objbase.h>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <set>
#include <array>
#include <algorithm>
namespace fs=std::filesystem;
namespace {
constexpr wchar_t Hardware[]=L"ROOT\\TITANHIDACCESS";
constexpr const wchar_t* ClassIds[]={L"{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}",L"{D61CA365-5AF4-4486-998B-9DB4734C6CA3}",L"{05F5CFE2-4733-4950-A6BB-07AAD01A3A84}"};
void Check(LSTATUS error){if(error!=ERROR_SUCCESS)throw std::system_error(static_cast<int>(error),std::system_category());}
struct RegistryKey {HKEY value=nullptr;~RegistryKey(){if(value)RegCloseKey(value);}};
struct FilterValue {bool present=false;DWORD type=REG_NONE;std::vector<BYTE> bytes;};
bool Same(const FilterValue& a,const FilterValue& b){return a.present==b.present&&a.type==b.type&&a.bytes==b.bytes;}
FilterValue ReadFilters(HKEY key){
    FilterValue result;DWORD bytes=0;LSTATUS error=RegQueryValueExW(key,L"UpperFilters",nullptr,&result.type,nullptr,&bytes);
    if(error==ERROR_FILE_NOT_FOUND)return result;Check(error);
    if(bytes>65536||bytes%sizeof(wchar_t))throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Invalid UpperFilters byte size");
    result.present=true;result.bytes.resize(bytes);
    if(bytes){DWORD actual=bytes;Check(RegQueryValueExW(key,L"UpperFilters",nullptr,&result.type,result.bytes.data(),&actual));if(actual!=bytes)throw std::system_error(ERROR_RETRY,std::system_category(),"UpperFilters changed while reading");}
    return result;
}
std::vector<std::wstring> DecodeFilters(const FilterValue& value){
    std::vector<std::wstring> result;if(!value.present)return result;
    if(value.type!=REG_MULTI_SZ||value.bytes.size()%sizeof(wchar_t))throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"UpperFilters must be REG_MULTI_SZ");
    if(value.bytes.empty())return result;
    std::vector<wchar_t> chars(value.bytes.size()/sizeof(wchar_t));memcpy(chars.data(),value.bytes.data(),value.bytes.size());
    if(chars.back()!=0||(chars.size()>1&&chars[chars.size()-2]!=0))throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"UpperFilters is not terminated");
    size_t offset=0;while(offset<chars.size()&&chars[offset]){size_t end=offset;while(end<chars.size()&&chars[end])++end;if(end==chars.size())throw std::system_error(ERROR_INVALID_DATA,std::system_category());result.emplace_back(chars.data()+offset,end-offset);offset=end+1;}
    for(;offset<chars.size();++offset)if(chars[offset])throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"UpperFilters contains data after its terminator");
    return result;
}
FilterValue EncodeFilters(const std::vector<std::wstring>& filters){
    FilterValue result;if(filters.empty())return result;
    result.present=true;result.type=REG_MULTI_SZ;std::vector<wchar_t> chars;
    for(const auto& name:filters){if(name.empty()||name.find(L'\0')!=std::wstring::npos)throw std::system_error(ERROR_INVALID_DATA,std::system_category());chars.insert(chars.end(),name.begin(),name.end());chars.push_back(0);}
    chars.push_back(0);result.bytes.resize(chars.size()*sizeof(wchar_t));memcpy(result.bytes.data(),chars.data(),result.bytes.size());return result;
}
FilterValue ChangeFilters(const FilterValue& original,bool install){
    auto names=DecodeFilters(original);bool ours=std::any_of(names.begin(),names.end(),[](const auto& name){return _wcsicmp(name.c_str(),L"TitanHidAccess")==0;});
    if(install&&!ours)names.emplace_back(L"TitanHidAccess");
    if(!install)names.erase(std::remove_if(names.begin(),names.end(),[](const auto& name){return _wcsicmp(name.c_str(),L"TitanHidAccess")==0;}),names.end());
    if((install&&ours)||(!install&&!ours))return original;
    return EncodeFilters(names);
}
void WriteFilters(HKEY key,const FilterValue& value){
    if(value.present)Check(RegSetValueExW(key,L"UpperFilters",0,value.type,value.bytes.empty()?nullptr:value.bytes.data(),static_cast<DWORD>(value.bytes.size())));
    else{LSTATUS error=RegDeleteValueW(key,L"UpperFilters");if(error!=ERROR_FILE_NOT_FOUND)Check(error);}
}
struct FilterResult {bool changed=false;unsigned present=0;};
FilterResult Associate(bool install,bool* touched=nullptr){
    struct Entry {RegistryKey key;FilterValue before,after;bool changed=false;};
    std::array<Entry,3> entries;FilterResult result;
    for(size_t i=0;i<entries.size();++i){auto path=std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Class\\")+ClassIds[i];LSTATUS error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_QUERY_VALUE|KEY_SET_VALUE,&entries[i].key.value);
        if(error==ERROR_FILE_NOT_FOUND){if(i==0&&install)Check(error);continue;}Check(error);++result.present;entries[i].before=ReadFilters(entries[i].key.value);entries[i].after=ChangeFilters(entries[i].before,install);}
    try{
        for(auto& entry:entries)if(entry.key.value&&!Same(entry.before,entry.after)){
            if(!Same(ReadFilters(entry.key.value),entry.before))throw std::system_error(ERROR_RETRY,std::system_category(),"Class filters changed concurrently; retry installation");
            if(touched)*touched=true;
            WriteFilters(entry.key.value,entry.after);entry.changed=true;result.changed=true;
        }
    }catch(...){
        // Restore only our exact writes. Never overwrite another installer's concurrent edit.
        for(auto it=entries.rbegin();it!=entries.rend();++it)if(it->changed){try{if(Same(ReadFilters(it->key.value),it->after))WriteFilters(it->key.value,it->before);}catch(...){}}
        throw;
    }
    return result;
}
DWORD ResetSession(){
    HANDLE control=CreateFileW(HA_CONTROL_PATH,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(control==INVALID_HANDLE_VALUE){DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?0:error;}
    HA_HEADER header{sizeof(header),HA_VERSION};DWORD bytes=0;bool success=DeviceIoControl(control,HA_IOCTL_RESET,&header,sizeof(header),nullptr,0,&bytes,nullptr)!=FALSE;DWORD error=success?0:GetLastError();CloseHandle(control);return error;
}
bool ClockRunning(){
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)return false;SC_HANDLE service=OpenServiceW(scm,L"TitanHidAccessClock",SERVICE_QUERY_STATUS);bool running=false;
    if(service){SERVICE_STATUS state{};running=QueryServiceStatus(service,&state)&&state.dwCurrentState!=SERVICE_STOPPED;CloseServiceHandle(service);}CloseServiceHandle(scm);return running;
}
void RemoveOwnedService(const wchar_t* name,const wchar_t* binary){
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)throw std::system_error(static_cast<int>(GetLastError()),std::system_category());
    SC_HANDLE service=OpenServiceW(scm,name,SERVICE_QUERY_CONFIG|DELETE);
    if(!service){DWORD error=GetLastError();CloseServiceHandle(scm);if(error==ERROR_SERVICE_DOES_NOT_EXIST||error==ERROR_SERVICE_MARKED_FOR_DELETE)return;Check(error);}
    DWORD bytes=0;QueryServiceConfigW(service,nullptr,0,&bytes);DWORD error=GetLastError();
    if(error!=ERROR_INSUFFICIENT_BUFFER||bytes>65536){CloseServiceHandle(service);CloseServiceHandle(scm);throw std::system_error(error,std::system_category());}
    std::vector<BYTE> buffer(bytes);auto config=reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    bool ok=QueryServiceConfigW(service,config,bytes,&bytes)!=FALSE;error=GetLastError();
    if(ok){std::wstring image=config->lpBinaryPathName?config->lpBinaryPathName:L"";if(!image.empty()&&image.front()==L'"'&&image.back()==L'"')image=image.substr(1,image.size()-2);auto slash=image.find_last_of(L"\\/");std::wstring leaf=slash==std::wstring::npos?image:image.substr(slash+1);
        if(config->dwServiceType!=SERVICE_KERNEL_DRIVER||_wcsicmp(leaf.c_str(),binary)!=0){CloseServiceHandle(service);CloseServiceHandle(scm);throw std::system_error(ERROR_INVALID_DATA,std::system_category(),"Owned service identity does not match");}
        ok=DeleteService(service)!=FALSE;error=GetLastError();
    }
    CloseServiceHandle(service);CloseServiceHandle(scm);if(!ok&&error!=ERROR_SERVICE_MARKED_FOR_DELETE)Check(error);
}
struct Devices {HDEVINFO h=INVALID_HANDLE_VALUE;~Devices(){if(h!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(h);}};
bool Ours(HDEVINFO h,SP_DEVINFO_DATA& d){
    DWORD type=0,bytes=0;SetupDiGetDeviceRegistryPropertyW(h,&d,SPDRP_HARDWAREID,&type,nullptr,0,&bytes);
    if(type!=REG_MULTI_SZ||bytes<4||bytes>65536)return false;
    std::vector<wchar_t> ids(bytes/2+2,0);
    if(!SetupDiGetDeviceRegistryPropertyW(h,&d,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(ids.data()),bytes,nullptr))return false;
    for(const wchar_t* p=ids.data();*p;p+=wcslen(p)+1)if(_wcsicmp(p,Hardware)==0)return true;
    return false;
}
std::wstring Message(DWORD e){wchar_t* s=nullptr;FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,e,0,reinterpret_cast<LPWSTR>(&s),0,nullptr);std::wstring r=s?s:L"Operation failed";if(s)LocalFree(s);return r+L" (Windows error "+std::to_wstring(e)+L")";}
void Require(bool ok){if(!ok)throw std::system_error(static_cast<int>(GetLastError()?GetLastError():ERROR_GEN_FAILURE),std::system_category());}
bool MicrosoftCatalog(const fs::path& path){
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;LONG status=WinVerifyTrust(nullptr,&action,&data);bool accepted=false;
    if(status==ERROR_SUCCESS){auto provider=WTHelperProvDataFromStateData(data.hWVTStateData);auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;if(signer&&signer->csCertChain&&signer->pChainContext){wchar_t name[512]{};CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,512);
        if(_wcsicmp(name,L"Microsoft Windows Hardware Compatibility Publisher")==0){
            for(DWORD flags:std::array<DWORD,2>{0,MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG}){
                CERT_CHAIN_POLICY_PARA policy{sizeof(policy),flags,nullptr};CERT_CHAIN_POLICY_STATUS policyStatus{};policyStatus.cbSize=sizeof(policyStatus);
                if(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT,signer->pChainContext,&policy,&policyStatus)&&policyStatus.dwError==ERROR_SUCCESS){accepted=true;break;}
            }
        }
    }}
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);return accepted;
}
bool CatalogMember(const fs::path& directory,const wchar_t* name){
    const auto path=directory/name;const auto catalog=directory/L"TitanHidAccess.cat";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    struct File{HANDLE h;~File(){CloseHandle(h);}} close{file};
    HCATADMIN admin=nullptr;if(!CryptCATAdminAcquireContext2(&admin,nullptr,L"SHA256",nullptr,0))return false;
    struct Admin{HCATADMIN h;~Admin(){CryptCATAdminReleaseContext(h,0);}} release{admin};
    BYTE hash[64]{};DWORD bytes=sizeof(hash);if(!CryptCATAdminCalcHashFromFileHandle2(admin,file,&bytes,hash,0)||bytes>64)return false;
    wchar_t tag[129]{};const wchar_t* hex=L"0123456789ABCDEF";for(DWORD i=0;i<bytes;++i){tag[i*2]=hex[hash[i]>>4];tag[i*2+1]=hex[hash[i]&15];}
    WINTRUST_CATALOG_INFO member{};member.cbStruct=sizeof(member);member.pcwszCatalogFilePath=catalog.c_str();member.pcwszMemberTag=tag;member.pcwszMemberFilePath=path.c_str();member.hMemberFile=file;member.pbCalculatedFileHash=hash;member.cbCalculatedFileHash=bytes;member.hCatAdmin=admin;
    WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.dwUnionChoice=WTD_CHOICE_CATALOG;data.pCatalog=&member;data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;LONG result=WinVerifyTrust(nullptr,&action,&data);data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&data);return result==ERROR_SUCCESS;
}
bool OwnPackage(const fs::path& path){
    GUID classId{};wchar_t className[128]{};
    if(!SetupDiGetINFClassW(path.c_str(),&classId,className,128,nullptr)||!IsEqualGUID(classId,GUID_DEVCLASS_SYSTEM))return false;
    HINF inf=SetupOpenInfFileW(path.c_str(),nullptr,INF_STYLE_WIN4,nullptr);if(inf==INVALID_HANDLE_VALUE)return false;
    struct Inf{HINF h;~Inf(){SetupCloseInfFile(h);}} file{inf};INFCONTEXT line{};wchar_t value[512]{};
    if(!SetupFindFirstLineW(inf,L"Version",L"CatalogFile",&line)||!SetupGetStringFieldW(&line,1,value,512,nullptr)||_wcsicmp(value,L"TitanHidAccess.cat"))return false;
    if(!SetupFindFirstLineW(inf,L"Version",L"Provider",&line)||!SetupGetStringFieldW(&line,1,value,512,nullptr)||_wcsicmp(value,L"Titanfyre"))return false;
    if(!SetupFindFirstLineW(inf,L"Models.NTamd64.10.0...19041",nullptr,&line))return false;
    unsigned rows=0;
    do{if(++rows>32)return false;if(SetupGetStringFieldW(&line,2,value,512,nullptr)&&_wcsicmp(value,Hardware)==0)return true;}while(SetupFindNextLine(&line,&line));
    return false;
}
struct Temporary {
    fs::path directory;
    bool owned=false;
    ~Temporary(){if(!owned)return;for(auto name:{L"TitanHidAccess.inf",L"TitanHidAccess.cat",L"TitanHidAccess.sys",L"TitanHidAccessClock.sys"})DeleteFileW((directory/name).c_str());RemoveDirectoryW(directory.c_str());}
    void Create(){
        wchar_t windows[MAX_PATH]{};DWORD length=GetWindowsDirectoryW(windows,MAX_PATH);Require(length>0&&length<MAX_PATH);GUID guid{};Require(SUCCEEDED(CoCreateGuid(&guid)));wchar_t id[40]{};StringFromGUID2(guid,id,40);
        // Use a system-owned parent, not the initiating user's writable Temp tree.
        directory=fs::path(windows)/L"Temp"/(std::wstring(L"TitanHidAccess-")+id);
        struct SecureDescriptor {PSECURITY_DESCRIPTOR value=nullptr;~SecureDescriptor(){if(value)LocalFree(value);}} descriptor;
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&descriptor.value,nullptr)!=FALSE);
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor.value,FALSE};bool made=CreateDirectoryW(directory.c_str(),&security)!=FALSE;DWORD error=GetLastError();if(!made){SetLastError(error);Require(false);}
        owned=true;
        const wchar_t* names[]{L"TitanHidAccess.inf",L"TitanHidAccess.cat",L"TitanHidAccess.sys",L"TitanHidAccessClock.sys"};
        for(int i=0;i<4;++i){
            HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(101+i),RT_RCDATA);Require(resource!=nullptr);HGLOBAL loaded=LoadResource(nullptr,resource);Require(loaded!=nullptr);DWORD size=SizeofResource(nullptr,resource);void* bytes=LockResource(loaded);Require(bytes&&size&&size<16*1024*1024);
            HANDLE f=CreateFileW((directory/names[i]).c_str(),GENERIC_WRITE,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Require(f!=INVALID_HANDLE_VALUE);DWORD written=0;bool ok=WriteFile(f,bytes,size,&written,nullptr)&&written==size;error=GetLastError();CloseHandle(f);if(!ok){SetLastError(error);Require(false);}
        }
    }
};
Setup::Result Install(){
    if(!Setup::PayloadAvailable())return {ERROR_FILE_NOT_FOUND,false,L"No signed driver package is embedded in this build. Import the Microsoft-signed INF/CAT and both SYS files, then rebuild the self-contained installers. Nothing was installed."};
    Temporary temp;temp.Create();
    if(!OwnPackage(temp.directory/L"TitanHidAccess.inf"))return {ERROR_INVALID_DATA,false,L"Installation refused: the embedded package does not match the exact HID Access Control identity."};
    if(!MicrosoftCatalog(temp.directory/L"TitanHidAccess.cat"))return {ERROR_INVALID_IMAGE_HASH,false,L"Installation refused: the embedded driver catalog must have a trusted Microsoft Windows Hardware Compatibility Publisher signature. Use the complete, unmodified Microsoft-signed release bundle."};
    if(!CatalogMember(temp.directory,L"TitanHidAccess.inf")||!CatalogMember(temp.directory,L"TitanHidAccess.sys")||!CatalogMember(temp.directory,L"TitanHidAccessClock.sys"))return {ERROR_INVALID_IMAGE_HASH,false,L"Installation refused: the embedded INF or SYS is not a trusted member of its catalog."};
    DWORD reset=ResetSession();if(reset)return {reset,false,L"Close HID Access Control and all SDK sessions before reinstalling. "+Message(reset)};
    Devices devices;devices.h=SetupDiGetClassDevsW(&GUID_DEVCLASS_SYSTEM,nullptr,nullptr,0);Require(devices.h!=INVALID_HANDLE_VALUE);
    SP_DEVINFO_DATA selected{};selected.cbSize=sizeof(selected);bool found=false;
    for(DWORD i=0;;++i){SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);if(!SetupDiEnumDeviceInfo(devices.h,i,&d)){if(GetLastError()!=ERROR_NO_MORE_ITEMS)Require(false);break;}if(Ours(devices.h,d)){selected=d;found=true;break;}}
    bool created=false;
    if(!found){
        Require(SetupDiCreateDeviceInfoW(devices.h,L"System",&GUID_DEVCLASS_SYSTEM,L"HidAccess Sensor",nullptr,DICD_GENERATE_ID,&selected)!=FALSE);
        constexpr wchar_t ids[]=L"ROOT\\TitanHidAccess\0";
        Require(SetupDiSetDeviceRegistryPropertyW(devices.h,&selected,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(ids),sizeof(ids))!=FALSE);
        Require(SetupDiCallClassInstaller(DIF_REGISTERDEVICE,devices.h,&selected)!=FALSE);created=true;
    }
    BOOL reboot=FALSE;
    if(!UpdateDriverForPlugAndPlayDevicesW(nullptr,Hardware,(temp.directory/L"TitanHidAccess.inf").c_str(),INSTALLFLAG_NONINTERACTIVE,&reboot)){
        DWORD error=GetLastError();bool removed=!created||SetupDiCallClassInstaller(DIF_REMOVE,devices.h,&selected)!=FALSE;
        return {error,!removed,L"Driver installation failed. "+std::wstring(created?(removed?L"The new device node was removed. ":L"The new device node could not be removed; restart and run Uninstall. "):L"The existing device was retained. ")+L"Windows may retain an owned staged package; use Uninstall to finish cleanup. "+Message(error)};
    }
    try{auto filters=Associate(true);return {0,reboot!=FALSE||filters.changed,L"HID Access Control installed. Filter associations: "+std::to_wstring(filters.present)+L" available classes. Restart Windows when requested so existing devices load the filter. If Windows adds a new controller class later, run Install / reinstall again. Policy remains inactive until an application starts a session."};}
    catch(const std::system_error& error){return {static_cast<DWORD>(error.code().value()),true,L"The signed driver was installed, but class-filter association failed. Previous filter values were restored where unchanged by other installers. The installed driver was retained to avoid dangling filter references. Restart Windows, then retry Install / reinstall or Uninstall. "+Message(static_cast<DWORD>(error.code().value()))};}
}
Setup::Result Uninstall(){
    DWORD reset=ResetSession();if(reset)return {reset,false,L"Close HID Access Control and all SDK sessions before uninstalling. "+Message(reset)};
    bool restart=false;
    try {
    auto filters=Associate(false,&restart);bool clockRunning=ClockRunning();restart=restart||filters.changed||clockRunning;
    Devices devices;devices.h=SetupDiGetClassDevsW(&GUID_DEVCLASS_SYSTEM,nullptr,nullptr,0);Require(devices.h!=INVALID_HANDLE_VALUE);
    std::vector<SP_DEVINFO_DATA> targets;std::set<std::wstring> packages;
    for(DWORD i=0;;++i){SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);if(!SetupDiEnumDeviceInfo(devices.h,i,&d)){if(GetLastError()!=ERROR_NO_MORE_ITEMS)Require(false);break;}if(Ours(devices.h,d)){
        targets.push_back(d);HKEY key=SetupDiOpenDevRegKey(devices.h,&d,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
        if(key!=INVALID_HANDLE_VALUE){wchar_t inf[256]{};DWORD bytes=sizeof(inf),type=0;if(RegQueryValueExW(key,L"InfPath",nullptr,&type,reinterpret_cast<BYTE*>(inf),&bytes)==ERROR_SUCCESS&&type==REG_SZ&&bytes<sizeof(inf)){CharLowerBuffW(inf,static_cast<DWORD>(wcslen(inf)));packages.emplace(inf);}RegCloseKey(key);}
    }}
    // Find only this exact sensor package, including an orphan left by an earlier
    // package-in-use failure after its device node had already been removed.
    wchar_t windows[MAX_PATH]{};DWORD length=GetWindowsDirectoryW(windows,MAX_PATH);Require(length>0&&length<MAX_PATH);
    fs::path infDirectory=fs::path(windows)/L"INF";
    for(auto it=packages.begin();it!=packages.end();){if(!OwnPackage(infDirectory/(*it)))it=packages.erase(it);else ++it;}
    WIN32_FIND_DATAW entry{};HANDLE search=FindFirstFileW((infDirectory/L"oem*.inf").c_str(),&entry);
    if(search==INVALID_HANDLE_VALUE){if(GetLastError()!=ERROR_FILE_NOT_FOUND)Require(false);}
    else {
        struct Search{HANDLE h;~Search(){FindClose(h);}} cleanup{search};unsigned scanned=0;
        do {
            if(++scanned>16384){SetLastError(ERROR_BUFFER_OVERFLOW);Require(false);}
            auto path=infDirectory/entry.cFileName;
            if(OwnPackage(path)){std::wstring name=entry.cFileName;CharLowerBuffW(name.data(),static_cast<DWORD>(name.size()));packages.insert(name);}
        }while(FindNextFileW(search,&entry));
        if(GetLastError()!=ERROR_NO_MORE_FILES)Require(false);
    }
    for(auto& d:targets){BOOL reboot=FALSE;if(!DiUninstallDevice(nullptr,devices.h,&d,0,&reboot)){auto error=GetLastError();return {error,restart,L"Could not remove the HidAccess device. "+Message(error)};}restart=restart||reboot!=FALSE;}
    // Mark only this product's exact services for deletion. A running resident
    // clock stays resident until normal reboot; never force-unload either driver.
    RemoveOwnedService(L"TitanHidAccess",L"TitanHidAccess.sys");
    RemoveOwnedService(L"TitanHidAccessClock",L"TitanHidAccessClock.sys");
    for(auto& inf:packages){
        // Published names only; never remove an arbitrary path or force removal of an in-use package.
        bool valid=inf.size()>7&&_wcsnicmp(inf.c_str(),L"oem",3)==0&&_wcsicmp(inf.c_str()+inf.size()-4,L".inf")==0;
        for(size_t i=3;valid&&i<inf.size()-4;++i)valid=inf[i]>=L'0'&&inf[i]<=L'9';
        if(valid&&!SetupUninstallOEMInfW(inf.c_str(),0,nullptr)){DWORD error=GetLastError();if(restart)return {0,true,L"Owned class-filter registrations and the control device were removed. Windows is retaining an in-use package or resident demo clock until restart. Restart Windows, then run Uninstall once more to remove any retained package. No driver was force-unloaded. "+Message(error)};return {error,restart,L"The device was removed, but Windows retained the driver package. Restart if requested and run Uninstall again. "+Message(error)};}
    }
    return {0,restart,targets.empty()&&packages.empty()?L"The HID Access Control device and package are not installed.":L"HID Access Control devices and unused driver packages removed."};
    }catch(const std::system_error& error){return {static_cast<DWORD>(error.code().value()),restart,L"Uninstall did not finish. Any completed filter/service changes were retained. "+std::wstring(restart?L"Restart Windows, then run Uninstall again. ":L"Close SDK applications and run Uninstall again. ")+Message(static_cast<DWORD>(error.code().value()))};}
    catch(...){return {ERROR_GEN_FAILURE,restart,L"Uninstall did not finish. "+std::wstring(restart?L"Restart Windows, then run Uninstall again to finish retained package/service cleanup.":L"No forced restart or driver unload was attempted. Run Uninstall again after reviewing the system state.")};}
}
}
unsigned Setup::Installed(DWORD* error){if(error)*error=0;try{Devices devices;devices.h=SetupDiGetClassDevsW(&GUID_DEVCLASS_SYSTEM,nullptr,nullptr,DIGCF_PRESENT);if(devices.h==INVALID_HANDLE_VALUE){if(error)*error=GetLastError();return 0;}unsigned n=0;for(DWORD i=0;;++i){SP_DEVINFO_DATA d{};d.cbSize=sizeof(d);if(!SetupDiEnumDeviceInfo(devices.h,i,&d)){DWORD e=GetLastError();if(e!=ERROR_NO_MORE_ITEMS&&error)*error=e;break;}if(Ours(devices.h,d))++n;}return n;}catch(...){if(error)*error=ERROR_NOT_ENOUGH_MEMORY;return 0;}}
bool Setup::PackageMatches(const std::wstring& path) noexcept{try{return OwnPackage(fs::path(path));}catch(...){return false;}}
bool Setup::CatalogTrusted(const std::wstring& path) noexcept{try{return MicrosoftCatalog(fs::path(path));}catch(...){return false;}}
bool Setup::PayloadAvailable() noexcept{
    for(int id=101;id<=104;++id){HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);if(!resource||!SizeofResource(nullptr,resource))return false;}return true;
}
unsigned Setup::FilterAssociations(DWORD* error) noexcept{
    if(error)*error=0;try{unsigned count=0;for(auto id:ClassIds){RegistryKey key;auto path=std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Class\\")+id;LSTATUS result=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_QUERY_VALUE,&key.value);if(result==ERROR_FILE_NOT_FOUND)continue;Check(result);auto names=DecodeFilters(ReadFilters(key.value));if(std::any_of(names.begin(),names.end(),[](const auto& name){return _wcsicmp(name.c_str(),L"TitanHidAccess")==0;}))++count;}return count;}catch(const std::system_error& e){if(error)*error=static_cast<DWORD>(e.code().value());return 0;}catch(...){if(error)*error=ERROR_GEN_FAILURE;return 0;}
}
Setup::Result Setup::SelfTest() noexcept{
    try{
        FilterValue missing;auto added=DecodeFilters(ChangeFilters(missing,true));if(added!=std::vector<std::wstring>{L"TitanHidAccess"})throw std::runtime_error("Empty filter add failed");
        auto original=EncodeFilters({L"ForeignA",L"ForeignB"});auto with=ChangeFilters(original,true);if(DecodeFilters(with)!=std::vector<std::wstring>{L"ForeignA",L"ForeignB",L"TitanHidAccess"})throw std::runtime_error("Foreign filter preservation failed");
        if(!Same(ChangeFilters(with,true),with)||!Same(ChangeFilters(with,false),original))throw std::runtime_error("Idempotent filter update failed");
        auto duplicate=EncodeFilters({L"ForeignA",L"TITANHIDACCESS",L"ForeignB",L"TitanHidAccess"});if(DecodeFilters(ChangeFilters(duplicate,false))!=std::vector<std::wstring>{L"ForeignA",L"ForeignB"})throw std::runtime_error("Owned-name removal failed");
        if(ChangeFilters(EncodeFilters({L"TitanHidAccess"}),false).present)throw std::runtime_error("Empty property cleanup failed");
        auto malformed=original;malformed.type=REG_SZ;bool rejected=false;try{DecodeFilters(malformed);}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Registry type validation failed");
        malformed=original;malformed.bytes.pop_back();rejected=false;try{DecodeFilters(malformed);}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Registry size validation failed");
        malformed=original;malformed.bytes.back()=1;rejected=false;try{DecodeFilters(malformed);}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Registry termination validation failed");
        return {0,false,PayloadAvailable()?L"PASS: offline filter preservation, idempotency and malformed-registry checks. Four payload resources are embedded; live install still requires trusted Microsoft catalog and INF/SYS membership validation.":L"PASS: offline filter preservation, idempotency and malformed-registry checks. No signed payload is embedded; live installation is unavailable and will be refused. No system state was changed."};
    }catch(...){return {ERROR_INVALID_DATA,false,L"FAIL: offline installer validation. No system state was changed."};}
}
Setup::Result Setup::Execute(bool install) noexcept {
    HANDLE mutex=nullptr;
    try {
        mutex=CreateMutexW(nullptr,FALSE,L"Global\\TitanHidAccessSetup");if(!mutex)return {GetLastError(),false,L"Could not open the installer lock."};
        DWORD wait=WaitForSingleObject(mutex,0);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(mutex);return {ERROR_BUSY,false,L"Another HidAccess installation is already running."};}
        Result result=install?Install():Uninstall();ReleaseMutex(mutex);CloseHandle(mutex);return result;
    }catch(const std::system_error& e){if(mutex){ReleaseMutex(mutex);CloseHandle(mutex);}return {static_cast<DWORD>(e.code().value()),false,Message(static_cast<DWORD>(e.code().value()))};}
    catch(...){if(mutex){ReleaseMutex(mutex);CloseHandle(mutex);}return {ERROR_GEN_FAILURE,false,L"The operation failed before completion. No system restart was requested."};}
}
bool Setup::Restart() noexcept {
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return false;TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;bool ok=LookupPrivilegeValueW(nullptr,SE_SHUTDOWN_NAME,&p.Privileges[0].Luid)!=FALSE;p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    if(ok){SetLastError(0);ok=AdjustTokenPrivileges(token,FALSE,&p,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS;}CloseHandle(token);
    return ok&&ExitWindowsEx(EWX_REBOOT,SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_MINOR_INSTALLATION|SHTDN_REASON_FLAG_PLANNED);
}
