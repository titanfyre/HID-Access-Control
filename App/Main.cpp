#include "../SDK/HidAccess.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr COLORREF Ink=RGB(15,23,42),Blue=RGB(37,99,235),Paper=RGB(241,245,249),Muted=RGB(100,116,139);
constexpr UINT_PTR PollTimer=1;
enum Id { Devices=101,Mode=102,Apps=103,Add=104,Remove=105,Self=106,Start=107,Refresh=108,Setup=109,Copy=110,Clear=111,ShowProtected=112 };
struct Item {HidAccess::Device device;bool selected=false;HidAccess::Mode mode=HidAccess::Mode::AllowListed;std::vector<std::wstring> apps;};
HWND window{},deviceList{},appList{},modeBox{},logList{};
HFONT body{},smallFont{},title{},emphasis{};
HBRUSH paper{},white{};
bool preview=false,active=false,updating=false,selfTest=false,showProtected=false,working=false,closing=false;
std::wstring screenshot,note=L"Select a supported device and configure its access rule. Nothing is hidden yet.";
std::vector<Item> items;
std::vector<size_t> visible;
std::vector<Item> rollback;
std::thread worker;
std::deque<HidAccess::Event> events;
std::unique_ptr<HidAccess::Session> session;
HidAccess::Status status{};
int selected=-1;
struct PolicyResult {HidAccess::Status status;std::string error;bool active=false;};

std::wstring Wide(const std::string& text) {
    if(text.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)return L"The operation failed. Check the driver and configuration.";
    std::wstring result(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n);
    return result;
}
std::wstring ExePath() {
    std::vector<wchar_t> path(32768);
    DWORD count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!count||count>=path.size())throw std::runtime_error("Cannot resolve this application's path.");
    return std::wstring(path.data(),count);
}
std::wstring HexId(const HidAccess::Device& device) {
    wchar_t value[20]{};
    swprintf_s(value,L"%04X : %04X",device.vid,device.pid);
    return value;
}
const wchar_t* ModeName(HidAccess::Mode mode) {
    switch(mode){case HidAccess::Mode::AllowListed:return L"Allow listed apps only";case HidAccess::Mode::DenyListed:return L"Block listed apps only";default:return L"Block all user apps";}
}
std::wstring DeviceName(const HidAccess::Device& device) {return device.name.empty()?device.instance_id:device.name;}
bool Editable(int index) {return index>=0&&static_cast<size_t>(index)<items.size()&&items[static_cast<size_t>(index)].device.supported&&!items[static_cast<size_t>(index)].device.protected_input;}
void Error(const std::exception& error) {note=Wide(error.what());InvalidateRect(window,nullptr,FALSE);}
void SetCell(HWND list,int row,int column,const std::wstring& text) {
    LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.iSubItem=column;item.pszText=const_cast<wchar_t*>(text.c_str());
    if(column)ListView_SetItem(list,&item);else ListView_InsertItem(list,&item);
}
void Columns(HWND list,std::initializer_list<std::pair<const wchar_t*,int>> values) {
    int index=0;for(const auto& value:values){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<wchar_t*>(value.first);col.cx=value.second;ListView_InsertColumn(list,index++,&col);}
}
void SelectionControls() {
    updating=true;
    ListView_DeleteAllItems(appList);
    bool editable=!working&&Editable(selected);
    EnableWindow(modeBox,editable);
    for(int id:{Add,Remove,Self})EnableWindow(GetDlgItem(window,id),editable);
    if(selected>=0&&static_cast<size_t>(selected)<items.size()){
        auto& item=items[static_cast<size_t>(selected)];
        SendMessageW(modeBox,CB_SETCURSEL,static_cast<WPARAM>(item.mode),0);
        LVCOLUMNW column{};column.mask=LVCF_TEXT;column.pszText=const_cast<wchar_t*>(item.mode==HidAccess::Mode::DenyListed?L"Blocked executable paths":item.mode==HidAccess::Mode::BlockAll?L"No application exceptions":L"Allowed executable paths");ListView_SetColumn(appList,0,&column);
        for(size_t i=0;i<item.apps.size();++i)SetCell(appList,static_cast<int>(i),0,item.apps[i]);
        if(item.mode==HidAccess::Mode::BlockAll){EnableWindow(appList,FALSE);for(int id:{Add,Remove,Self})EnableWindow(GetDlgItem(window,id),FALSE);}else EnableWindow(appList,editable);
    }else{SendMessageW(modeBox,CB_SETCURSEL,0,0);EnableWindow(appList,FALSE);}
    updating=false;
    InvalidateRect(window,nullptr,FALSE);
}
void RebuildDevices() {
    updating=true;ListView_DeleteAllItems(deviceList);
    visible.clear();
    for(size_t i=0;i<items.size();++i){
        const auto& item=items[i];if(!showProtected&&(!item.device.supported||item.device.protected_input))continue;int row=static_cast<int>(visible.size());visible.push_back(i);
        SetCell(deviceList,row,0,DeviceName(item.device));SetCell(deviceList,row,1,HexId(item.device));
        SetCell(deviceList,row,2,item.device.protected_input?L"Protected / transport":!item.device.supported?L"Not supported":item.selected?ModeName(item.mode):L"Unrestricted");
        ListView_SetCheckState(deviceList,row,item.selected);
    }
    if(!visible.empty()){
        auto row=std::find(visible.begin(),visible.end(),static_cast<size_t>(selected));if(row==visible.end()){selected=static_cast<int>(visible[0]);row=visible.begin();}
        ListView_SetItemState(deviceList,static_cast<int>(row-visible.begin()),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    }else selected=-1;
    updating=false;SelectionControls();
}
void EditingEnabled(bool enabled) {
    for(int id:{Devices,Mode,Apps,Add,Remove,Self,Start,Refresh,Setup,ShowProtected})EnableWindow(GetDlgItem(window,id),enabled);
    if(enabled)SelectionControls();
}
void BeginPolicy(std::vector<HidAccess::Rule> rules,bool starting) {
    if(working)return;if(worker.joinable())worker.join();working=true;EditingEnabled(false);note=L"Applying access rules... The interface remains responsive.";InvalidateRect(window,nullptr,FALSE);
    try{worker=std::thread([rules=std::move(rules),starting]{
        std::unique_ptr<PolicyResult> result(new(std::nothrow)PolicyResult);
        if(!result){PostMessageW(window,WM_APP+2,0,0);return;}
        try{
            if(starting){auto client=std::make_unique<HidAccess::Session>();client->Apply(rules);result->status=client->GetStatus();session=std::move(client);result->active=true;}
            else if(rules.empty()){session->Stop();session.reset();result->status=HidAccess::Probe();result->active=false;}
            else{
                try{session->Apply(rules);}catch(const std::exception& error){result->error=error.what();result->active=true;}
                if(result->error.empty()){try{result->status=session->GetStatus();result->active=result->status.active;if(!result->active){session->Stop();session.reset();}}catch(...){if(session)session->Stop();session.reset();throw;}}
            }
        }catch(const std::exception& error){result->error=error.what();result->active=false;}
        auto pointer=result.release();if(!PostMessageW(window,WM_APP+1,0,reinterpret_cast<LPARAM>(pointer)))delete pointer;
    });}catch(...){working=false;EditingEnabled(true);throw;}
}
std::vector<HidAccess::Rule> Rules() {
    std::vector<HidAccess::Rule> result;
    for(const auto& item:items)if(item.selected){
        if(!item.device.supported||item.device.protected_input)throw std::runtime_error("A protected or unsupported device cannot be restricted.");
        if(item.mode==HidAccess::Mode::AllowListed&&item.apps.empty())throw std::runtime_error("Add at least one allowed executable, or explicitly choose Block all user apps.");
        if(item.mode==HidAccess::Mode::DenyListed&&item.apps.empty())throw std::runtime_error("Add at least one executable to the block list.");
        HidAccess::Rule rule;rule.device=item.device.token;rule.mode=item.mode;if(item.mode!=HidAccess::Mode::BlockAll)rule.applications=item.apps;result.push_back(std::move(rule));
    }
    return result;
}
void ApplyChanges() {
    if(!active)return;
    auto rules=Rules();
    if(rules.empty()){
        if(session)session->Stop();session.reset();active=false;
        SetWindowTextW(GetDlgItem(window,Start),L"Start session");note=L"No devices selected. Access is unrestricted.";return;
    }
    if(!preview){BeginPolicy(std::move(rules),false);return;}
    note=preview?L"PREVIEW: rule changes are simulated; no driver is opened.":L"Rules updated atomically. Reopen affected applications to test new opens.";
}
template<class F> void Edit(F&& change) {
    auto before=items;
    try{change();if(active&&!preview)rollback=before;ApplyChanges();RebuildDevices();if(working)EditingEnabled(false);}
    catch(const std::exception& error){items=std::move(before);RebuildDevices();Error(error);}
}
void Stop() {
    if(working)return;
    if(session)session->Stop();session.reset();active=false;
    SetWindowTextW(GetDlgItem(window,Start),L"Start session");
    note=preview?L"PREVIEW: no device access was changed.":L"Session stopped. Selected devices are unrestricted again.";
    status.active=false;InvalidateRect(window,nullptr,FALSE);
}
void StartSession() {
    if(active){Stop();return;}
    auto rules=Rules();
    if(rules.empty())throw std::runtime_error("Check at least one supported device first.");
    if(!preview){rollback.clear();BeginPolicy(std::move(rules),true);return;}
    active=true;SetWindowTextW(GetDlgItem(window,Start),L"Stop session");
    note=preview?L"PREVIEW SESSION: rules and events are synthetic; no driver is opened.":L"Session active. Only new device opens are evaluated; existing handles stay open.";
    InvalidateRect(window,nullptr,FALSE);
}
void SeedPreview() {
    items.clear();
    HidAccess::Device gamepad;gamepad.token="hidac:101";gamepad.instance_id=L"HID\\VID_FFFE&PID_0001\\PREVIEW_GAMEPAD";gamepad.name=L"Example HID gamepad (synthetic)";gamepad.vid=0xFFFE;gamepad.pid=1;gamepad.supported=true;gamepad.flags=5;
    Item pad;pad.device=std::move(gamepad);pad.selected=true;pad.apps={L"C:\\Tools\\ControllerMapper.exe"};items.push_back(std::move(pad));
    HidAccess::Device keyboard;keyboard.token="hidac:102";keyboard.instance_id=L"HID\\PREVIEW_KEYBOARD";keyboard.name=L"Example keyboard - protected (synthetic)";keyboard.protected_input=true;keyboard.flags=6;
    Item key;key.device=std::move(keyboard);items.push_back(std::move(key));
    HidAccess::Device second;second.token="hidac:103";second.instance_id=L"USB\\VID_FFFE&PID_0002\\PREVIEW_XUSB";second.name=L"Example USB controller (synthetic)";second.vid=0xFFFE;second.pid=2;second.supported=true;second.flags=9;
    Item controller;controller.device=std::move(second);items.push_back(std::move(controller));
    selected=0;status={};note=L"OFFLINE PREVIEW: all listed devices and decisions are synthetic. No driver is opened.";
}
void RefreshInventory() {
    if(preview){if(items.empty())SeedPreview();RebuildDevices();return;}
    std::unique_ptr<HidAccess::Session> temporary;
    try{
        status=HidAccess::Probe();
        if(!status.running){items.clear();selected=-1;note=status.detail.empty()?L"Install the matching driver, restart if requested, and refresh.":status.detail;RebuildDevices();return;}
        auto* client=session.get();if(!client){temporary=std::make_unique<HidAccess::Session>();client=temporary.get();}
        auto devices=client->Devices();status=client->GetStatus();
        std::vector<Item> next;next.reserve(devices.size());
        for(auto& device:devices){auto found=std::find_if(items.begin(),items.end(),[&](const Item& item){return item.device.token==device.token;});Item item;if(found!=items.end())item=*found;item.device=std::move(device);if(!item.device.supported||item.device.protected_input)item.selected=false;next.push_back(std::move(item));}
        items=std::move(next);if(active)ApplyChanges();
        note=items.empty()?L"No filter instances are attached. Restart Windows after installation, then reconnect a controller.":L"Inventory refreshed. Supported controller collections are shown; protected transport nodes are available in the debug view.";
        RebuildDevices();
    }catch(const std::exception& error){Error(error);}
}
void AddApplication(bool own) {
    if(!Editable(selected))return;
    std::wstring path;
    if(own)path=ExePath();
    else{
        std::vector<wchar_t> buffer(32768);
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFilter=L"Windows executable (*.exe)\0*.exe\0\0";dialog.lpstrFile=buffer.data();dialog.nMaxFile=static_cast<DWORD>(buffer.size());dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&dialog))return;path=buffer.data();
    }
    Edit([&]{auto& apps=items[static_cast<size_t>(selected)].apps;if(std::none_of(apps.begin(),apps.end(),[&](const auto& value){return _wcsicmp(value.c_str(),path.c_str())==0;}))apps.push_back(path);});
}
void AddEvent(const HidAccess::Event& event) {
    events.push_back(event);if(events.size()>300){events.pop_front();ListView_DeleteItem(logList,0);}
    int row=ListView_GetItemCount(logList);SetCell(logList,row,0,std::to_wstring(event.sequence));SetCell(logList,row,1,event.denied?L"DENIED":L"Allowed");SetCell(logList,row,2,std::to_wstring(event.process_id));SetCell(logList,row,3,event.reason);SetCell(logList,row,4,event.application);
}
void Poll() {
    if(working||preview||!session||!active)return;
    try{
        auto next=session->GetStatus();
        if(next.demo_expired||!next.active){Stop();status=next;note=next.demo_expired?L"The one-hour demo has expired. Input access is unrestricted. Restart Windows for another demo period.":L"The driver released this policy. Access is unrestricted.";return;}
        status=next;auto batch=session->ReadEvents();
        if(!batch.empty()){SendMessageW(logList,WM_SETREDRAW,FALSE,0);for(const auto& event:batch)AddEvent(event);SendMessageW(logList,WM_SETREDRAW,TRUE,0);InvalidateRect(logList,nullptr,FALSE);}
        RECT cards{20,88,1200,170};InvalidateRect(window,&cards,FALSE);
    }catch(const std::exception& error){Stop();Error(error);}
}
std::wstring Report() {
    std::wostringstream out;out<<L"HID Access Control\r\nMode: "<<(preview?L"OFFLINE SYNTHETIC PREVIEW":L"Live")<<L"\r\nLocal session: "<<(active?L"active":L"inactive")<<L"\r\n";
    out<<L"Driver installed/running: "<<status.installed<<L" / "<<status.running<<L"\r\nSupported collections / raw attached nodes: "<<std::count_if(items.begin(),items.end(),[](const Item& item){return item.device.supported&&!item.device.protected_input;})<<L" / "<<items.size()<<L"\r\nAllowed / denied opens: "<<status.allowed_opens<<L" / "<<status.denied_opens<<L"\r\nDropped events: "<<status.dropped_events<<L"\r\n";
    for(const auto& item:items){out<<L"\r\nDevice: "<<DeviceName(item.device)<<L" ["<<HexId(item.device)<<L"]\r\nInstance: "<<item.device.instance_id<<L"\r\nRule: "<<(item.selected?ModeName(item.mode):L"Unrestricted")<<L"\r\n";for(const auto& app:item.apps)out<<L"  "<<app<<L"\r\n";}
    out<<L"\r\nLatest decisions:\r\n";for(const auto& event:events)out<<event.sequence<<L" | "<<(event.denied?L"DENIED":L"Allowed")<<L" | PID "<<event.process_id<<L" | "<<event.reason<<L" | "<<event.application<<L"\r\n";
    out<<L"\r\nScope: new user-mode device opens only. Existing handles are not revoked. Windows brokers can open a device on behalf of another application; their executable is the observed caller. Kernel/system paths are preserved. This does not remove PnP/Device Manager entries or hide a process. Closing the owner session clears all restrictions.\r\n\r\nStatus: "<<note<<L"\r\n";
    return out.str();
}
void CopyReport() {
    auto text=Report();SIZE_T bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!memory)throw std::bad_alloc();void* data=GlobalLock(memory);if(!data){GlobalFree(memory);throw std::runtime_error("Cannot allocate clipboard memory.");}memcpy(data,text.c_str(),bytes);GlobalUnlock(memory);
    if(!OpenClipboard(window)){GlobalFree(memory);throw std::runtime_error("Clipboard is busy. Try again.");}
    if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory)){CloseClipboard();GlobalFree(memory);throw std::runtime_error("Cannot copy the report.");}CloseClipboard();note=L"Diagnostic report copied. It contains device identities and executable paths.";InvalidateRect(window,nullptr,FALSE);
}
void OpenSetup() {
    Stop();auto path=ExePath();path.resize(path.find_last_of(L"\\/")+1);
    std::wstring setup=path+L"HidAccessSetup.exe";
    if(GetFileAttributesW(setup.c_str())==INVALID_FILE_ATTRIBUTES)setup=path+L"Dependencies\\HidAccessSetup.exe";
    if(GetFileAttributesW(setup.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Extract the complete release ZIP first. HidAccessSetup.exe is missing.");
    if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",setup.c_str(),nullptr,path.c_str(),SW_SHOWNORMAL))<=32)throw std::runtime_error("Windows could not open the driver installer.");
}
void Text(HDC dc,RECT rectangle,const std::wstring& value,COLORREF color=Ink,HFONT font=nullptr,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS) {
    auto previous=SelectObject(dc,font?font:body);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&rectangle,flags);SelectObject(dc,previous);
}
void Card(HDC dc,RECT rectangle) {
    auto previous=SelectObject(dc,white);auto pen=SelectObject(dc,GetStockObject(NULL_PEN));RoundRect(dc,rectangle.left,rectangle.top,rectangle.right,rectangle.bottom,16,16);SelectObject(dc,pen);SelectObject(dc,previous);
}
int PanelBottom() {RECT r{};GetClientRect(window,&r);return std::clamp(static_cast<int>(r.bottom)-300,435,543);}
void Paint(HDC dc) {
    RECT client{};GetClientRect(window,&client);FillRect(dc,&client,paper);int width=client.right,half=width/2;
    int panelBottom=PanelBottom();
    Text(dc,{24,19,width-300,53},L"HID Access Control",Ink,title);Text(dc,{25,53,width-300,76},L"Per-device access rules. Your input reports stay untouched.",Muted,smallFont);
    int cardWidth=(width-72)/3;for(int i=0;i<3;++i)Card(dc,{24+i*(cardWidth+12),91,24+i*(cardWidth+12)+cardWidth,165});
    Text(dc,{40,99,24+cardWidth,121},L"DRIVER",Muted,smallFont);Text(dc,{40,125,24+cardWidth,154},preview?L"Offline preview":status.running?L"Running":status.installed?L"Installed / not running":L"Not installed",Ink,emphasis);
    int second=36+cardWidth;Text(dc,{second+16,99,second+cardWidth,121},L"LOCAL POLICY",Muted,smallFont);Text(dc,{second+16,125,second+cardWidth,154},active?(preview?L"Synthetic session":L"Active session"):L"Unrestricted",active?Blue:Ink,emphasis);
    int third=48+2*cardWidth;Text(dc,{third+16,99,third+cardWidth,121},L"DEMO ALLOWANCE",Muted,smallFont);
    std::wstring remaining=preview?L"Preview only":status.demo_expired?L"Expired":!status.remaining_100ns?L"Starts with activation":std::to_wstring((status.remaining_100ns+599999999)/600000000)+L" min remaining";
    Text(dc,{third+16,125,third+cardWidth,154},remaining,Ink,emphasis);
    Text(dc,{26,183,220,211},L"1. Select devices",Ink,emphasis);Text(dc,{half+12,183,width-25,211},L"2. Set the selected device's rule",Ink,emphasis);
    Text(dc,{half+14,215,width-26,241},selected>=0&&static_cast<size_t>(selected)<items.size()?DeviceName(items[static_cast<size_t>(selected)].device):L"Select a row on the left.",Muted,smallFont);
    Text(dc,{500,panelBottom+10,width-26,panelBottom+46},L"3. Start a session. Stop or close to restore access.",Muted,smallFont);
    Text(dc,{26,panelBottom+54,width-26,panelBottom+88},note,Muted,smallFont,DT_LEFT|DT_WORDBREAK);
    Text(dc,{26,panelBottom+92,width-26,panelBottom+115},L"Access decisions - new opens only",Ink,emphasis);
    Text(dc,{26,client.bottom-30,width-26,client.bottom-7},preview?L"SYNTHETIC PREVIEW  |  No driver, input or access policy is opened.":L"One control application at a time. Existing handles and Windows broker behavior still apply.",Muted,smallFont);
}
void Layout() {
    RECT r{};GetClientRect(window,&r);int width=r.right,half=width/2;
    int panelBottom=PanelBottom();
    MoveWindow(GetDlgItem(window,Setup),width-272,24,130,36,TRUE);MoveWindow(GetDlgItem(window,Refresh),width-130,24,106,36,TRUE);
    MoveWindow(GetDlgItem(window,ShowProtected),235,187,std::max(180,half-255),24,TRUE);
    MoveWindow(deviceList,24,221,half-38,panelBottom-221,TRUE);MoveWindow(modeBox,half+12,249,width-half-36,200,TRUE);
    MoveWindow(appList,half+12,289,width-half-36,panelBottom-340,TRUE);
    MoveWindow(GetDlgItem(window,Add),half+12,panelBottom-38,112,36,TRUE);MoveWindow(GetDlgItem(window,Remove),half+134,panelBottom-38,106,36,TRUE);MoveWindow(GetDlgItem(window,Self),half+250,panelBottom-38,std::max(130,width-half-274),36,TRUE);
    MoveWindow(GetDlgItem(window,Start),24,panelBottom+10,174,38,TRUE);MoveWindow(GetDlgItem(window,Copy),210,panelBottom+10,148,38,TRUE);MoveWindow(GetDlgItem(window,Clear),370,panelBottom+10,110,38,TRUE);
    MoveWindow(logList,24,panelBottom+120,width-48,std::max(92,static_cast<int>(r.bottom)-panelBottom-152),TRUE);
    ListView_SetColumnWidth(deviceList,0,std::max(140,half-330));ListView_SetColumnWidth(deviceList,1,105);ListView_SetColumnWidth(deviceList,2,165);
    ListView_SetColumnWidth(appList,0,std::max(150,width-half-60));ListView_SetColumnWidth(logList,4,std::max(200,width-502));
}
HWND Control(const wchar_t* type,const wchar_t* caption,int id,DWORD style=0,DWORD extended=0) {
    auto handle=CreateWindowExW(extended,type,caption,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,10,10,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    if(!handle)throw std::runtime_error("Cannot create a Windows UI control.");SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(body),TRUE);return handle;
}
void DrawButton(const DRAWITEMSTRUCT& item) {
    if(item.CtlID==Mode){int mode=item.itemID==static_cast<UINT>(-1)?static_cast<int>(SendMessageW(modeBox,CB_GETCURSEL,0,0)):static_cast<int>(item.itemID);FillRect(item.hDC,&item.rcItem,white);if(mode>=0&&mode<=2){RECT r=item.rcItem;r.left+=6;Text(item.hDC,r,ModeName(static_cast<HidAccess::Mode>(mode)));}return;}
    RECT r=item.rcItem;bool primary=item.CtlID==Start;COLORREF color=primary?Blue:RGB(255,255,255);
    if(item.itemState&ODS_DISABLED)color=Paper;else if(item.itemState&ODS_SELECTED)color=primary?RGB(29,78,216):RGB(226,232,240);
    HBRUSH brush=CreateSolidBrush(color);HPEN border=CreatePen(PS_SOLID,1,primary?color:RGB(203,213,225));auto previous=SelectObject(item.hDC,brush);auto pen=SelectObject(item.hDC,border);RoundRect(item.hDC,r.left,r.top,r.right,r.bottom,10,10);SelectObject(item.hDC,pen);SelectObject(item.hDC,previous);DeleteObject(border);DeleteObject(brush);
    wchar_t caption[128]{};GetWindowTextW(item.hwndItem,caption,128);Text(item.hDC,r,caption,(item.itemState&ODS_DISABLED)?Muted:primary?RGB(255,255,255):Ink,nullptr,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(item.itemState&ODS_FOCUS){InflateRect(&r,-4,-4);DrawFocusRect(item.hDC,&r);}
}
bool SaveScreenshot(const std::wstring& path) {
    RECT client{};GetClientRect(window,&client);HDC screen=GetDC(window),memory=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);auto previous=SelectObject(memory,bitmap);
    Paint(memory);
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT rectangle{};GetWindowRect(child,&rectangle);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rectangle),2);
        int saved=SaveDC(memory);SetViewportOrgEx(memory,rectangle.left,rectangle.top,nullptr);
        IntersectClipRect(memory,0,0,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top);
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
        if(child==modeBox){
            // Some comctl builds omit owner-drawn combo text from WM_PRINT.
            // Use the actual selected native item, not a separately invented label.
            LRESULT chosen=SendMessageW(child,CB_GETCURSEL,0,0);
            if(chosen!=CB_ERR){LRESULT count=SendMessageW(child,CB_GETLBTEXTLEN,static_cast<WPARAM>(chosen),0);if(count>=0&&count<256){std::array<wchar_t,256> caption{};SendMessageW(child,CB_GETLBTEXT,static_cast<WPARAM>(chosen),reinterpret_cast<LPARAM>(caption.data()));RECT textArea{5,2,std::max(6L,rectangle.right-rectangle.left-26),rectangle.bottom-rectangle.top-2};FillRect(memory,&textArea,white);Text(memory,textArea,caption.data());}}
        }
        RestoreDC(memory,saved);
    }
    SelectObject(memory,previous);DeleteDC(memory);ReleaseDC(window,screen);
    IWICImagingFactory* factory=nullptr;IWICBitmap* source=nullptr;IWICStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;IPropertyBag2* options=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateBitmapFromHBITMAP(bitmap,nullptr,WICBitmapIgnoreAlpha,&source);
    if(SUCCEEDED(hr))hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);
    if(SUCCEEDED(hr))hr=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(hr))hr=encoder->Initialize(stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,&options);
    if(SUCCEEDED(hr))hr=frame->Initialize(options);
    if(SUCCEEDED(hr))hr=frame->SetSize(static_cast<UINT>(client.right),static_cast<UINT>(client.bottom));
    WICPixelFormatGUID pixelFormat=GUID_WICPixelFormat32bppBGR;
    if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&pixelFormat);
    if(SUCCEEDED(hr))hr=frame->WriteSource(source,nullptr);
    if(SUCCEEDED(hr))hr=frame->Commit();
    if(SUCCEEDED(hr))hr=encoder->Commit();
    if(options)options->Release();if(frame)frame->Release();if(encoder)encoder->Release();if(stream)stream->Release();if(source)source->Release();if(factory)factory->Release();DeleteObject(bitmap);
    return SUCCEEDED(hr);
}
bool CompactLayout() {
    RECT original{};GetWindowRect(window,&original);
    auto savedApps=items[0].apps;items[0].apps={L"C:\\Preview\\One.exe",L"C:\\Preview\\Two.exe",L"C:\\Preview\\Three.exe"};RebuildDevices();
    HidAccess::Event third;third.sequence=3;third.process_id=4202;third.reason=L"Layout fixture (synthetic)";third.application=L"C:\\Preview\\Three.exe";AddEvent(third);
    SetWindowPos(window,nullptr,0,0,1080,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Layout();
    RECT appClient{},appThird{},logClient{},logThird{};GetClientRect(appList,&appClient);GetClientRect(logList,&logClient);
    bool ok=ListView_GetItemRect(appList,2,&appThird,LVIR_BOUNDS)&&ListView_GetItemRect(logList,2,&logThird,LVIR_BOUNDS)&&appThird.bottom<=appClient.bottom&&logThird.bottom<=logClient.bottom&&SendMessageW(modeBox,CB_GETCURSEL,0,0)==0;
    items[0].apps=std::move(savedApps);events.pop_back();ListView_DeleteItem(logList,ListView_GetItemCount(logList)-1);RebuildDevices();
    SetWindowPos(window,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Layout();
    return ok;
}
LRESULT CALLBACK Proc(HWND h,UINT message,WPARAM w,LPARAM l) {
    try{switch(message){
    case WM_CREATE:{
        window=h;body=CreateFontW(17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");smallFont=CreateFontW(15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");title=CreateFontW(28,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");emphasis=CreateFontW(19,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");paper=CreateSolidBrush(Paper);white=CreateSolidBrush(RGB(255,255,255));
        for(auto button:std::initializer_list<std::pair<const wchar_t*,int>>{{L"Driver setup",Setup},{L"Refresh",Refresh},{L"Add EXE",Add},{L"Remove",Remove},{L"Allow this application",Self},{L"Start session",Start},{L"Copy report",Copy},{L"Clear log",Clear}})Control(L"BUTTON",button.first,button.second,BS_OWNERDRAW);
        deviceList=Control(WC_LISTVIEWW,L"Devices",Devices,LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(deviceList,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);Columns(deviceList,{{L"Device",240},{L"VID : PID",105},{L"Access",142}});
        Control(L"BUTTON",L"Show protected nodes",ShowProtected,BS_AUTOCHECKBOX);
        modeBox=Control(WC_COMBOBOXW,L"Access mode",Mode,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS);for(auto mode:{HidAccess::Mode::AllowListed,HidAccess::Mode::DenyListed,HidAccess::Mode::BlockAll})SendMessageW(modeBox,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(ModeName(mode)));
        appList=Control(WC_LISTVIEWW,L"Applications for the selected device",Apps,LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(appList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);Columns(appList,{{L"Executable path",480}});
        logList=Control(WC_LISTVIEWW,L"Access decisions",200,LVS_REPORT|LVS_SHOWSELALWAYS,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(logList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);Columns(logList,{{L"#",60},{L"Decision",85},{L"PID",70},{L"Reason",220},{L"Executable",500}});
        if(preview){SeedPreview();HidAccess::Event sample;sample.sequence=1;sample.process_id=4200;sample.denied=true;sample.reason=L"Not in allow list (synthetic)";sample.application=L"C:\\Tools\\Game.exe";AddEvent(sample);sample.sequence=2;sample.process_id=4201;sample.denied=false;sample.reason=L"Allowed executable (synthetic)";sample.application=L"C:\\Tools\\ControllerMapper.exe";AddEvent(sample);}
        for(auto control:{deviceList,appList,logList})SetWindowTheme(control,L"Explorer",nullptr);
        RefreshInventory();Layout();if(!selfTest)SetTimer(h,PollTimer,250,nullptr);return 0;
    }
    case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={1080,720};return 0;}
    case WM_SIZE:if(deviceList){Layout();InvalidateRect(h,nullptr,FALSE);}return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,client.right),std::max(1L,client.bottom));auto old=SelectObject(memory,bitmap);Paint(memory);BitBlt(dc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);EndPaint(h,&ps);return 0;}
    case WM_PRINTCLIENT:Paint(reinterpret_cast<HDC>(w));return 0;
    case WM_DRAWITEM:DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
    case WM_MEASUREITEM:if(reinterpret_cast<MEASUREITEMSTRUCT*>(l)->CtlID==Mode){reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight=24;return TRUE;}break;
    case WM_TIMER:if(w==PollTimer)Poll();return 0;
    case WM_COMMAND:
        if(working&&LOWORD(w)!=Copy&&LOWORD(w)!=Clear)return 0;
        if(LOWORD(w)==Mode&&HIWORD(w)==CBN_SELCHANGE&&!updating&&!working&&Editable(selected)){auto value=SendMessageW(modeBox,CB_GETCURSEL,0,0);if(value>=0&&value<=2)Edit([&]{items[static_cast<size_t>(selected)].mode=static_cast<HidAccess::Mode>(value);});return 0;}
        if(HIWORD(w)==BN_CLICKED){switch(LOWORD(w)){
        case Start:StartSession();break;case Refresh:RefreshInventory();break;case Setup:OpenSetup();break;case Copy:CopyReport();break;case ShowProtected:showProtected=SendMessageW(GetDlgItem(h,ShowProtected),BM_GETCHECK,0,0)==BST_CHECKED;RebuildDevices();break;
        case Clear:events.clear();ListView_DeleteAllItems(logList);break;case Add:AddApplication(false);break;case Self:AddApplication(true);break;
        case Remove:if(Editable(selected)){int row=ListView_GetNextItem(appList,-1,LVNI_SELECTED);if(row>=0)Edit([&]{auto& paths=items[static_cast<size_t>(selected)].apps;if(static_cast<size_t>(row)<paths.size())paths.erase(paths.begin()+row);});}break;
        }InvalidateRect(h,nullptr,FALSE);}return 0;
    case WM_NOTIFY:{auto header=reinterpret_cast<NMHDR*>(l);if(header->hwndFrom==deviceList){
        if(header->code==LVN_ITEMCHANGING&&!updating){auto change=reinterpret_cast<NMLISTVIEW*>(l);if(change->iItem>=0&&static_cast<size_t>(change->iItem)<visible.size()&&!Editable(static_cast<int>(visible[static_cast<size_t>(change->iItem)]))&&((change->uNewState^change->uOldState)&LVIS_STATEIMAGEMASK))return TRUE;}
        if(header->code==LVN_ITEMCHANGED&&!updating&&!working){auto change=reinterpret_cast<NMLISTVIEW*>(l);if(change->iItem>=0&&static_cast<size_t>(change->iItem)<visible.size()){
            auto index=visible[static_cast<size_t>(change->iItem)];
            if((change->uNewState&LVIS_SELECTED)&&!(change->uOldState&LVIS_SELECTED)){selected=static_cast<int>(index);SelectionControls();}
            if((change->uNewState^change->uOldState)&LVIS_STATEIMAGEMASK){int row=change->iItem;bool checked=ListView_GetCheckState(deviceList,row)!=FALSE;Edit([&]{items[index].selected=checked;});}
        }}
        if(header->code==NM_CUSTOMDRAW){auto draw=reinterpret_cast<NMLVCUSTOMDRAW*>(l);if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT){auto index=static_cast<size_t>(draw->nmcd.dwItemSpec);if(index<visible.size()&&!Editable(static_cast<int>(visible[index])))draw->clrText=Muted;return CDRF_DODEFAULT;}}
    }return 0;}
    case WM_SETCURSOR:{HWND child=reinterpret_cast<HWND>(w);if(child!=h&&IsWindowEnabled(child)){wchar_t type[32]{};GetClassNameW(child,type,32);if(_wcsicmp(type,L"BUTTON")==0||_wcsicmp(type,L"ComboBox")==0){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}}break;}
    case WM_APP+1:{std::unique_ptr<PolicyResult> result(reinterpret_cast<PolicyResult*>(l));if(worker.joinable())worker.join();working=false;if(!result->error.empty()){if(!rollback.empty())items=std::move(rollback);note=Wide(result->error);}else{status=result->status;note=result->active?L"Session active. Rules were replaced atomically; reopen affected applications to test new opens.":L"Access is unrestricted.";}rollback.clear();active=result->active;if(!active&&session){session->Stop();session.reset();}SetWindowTextW(GetDlgItem(window,Start),active?L"Stop session":L"Start session");EditingEnabled(true);RebuildDevices();if(closing)DestroyWindow(h);else InvalidateRect(h,nullptr,FALSE);return 0;}
    case WM_APP+2:if(worker.joinable())worker.join();working=false;if(!rollback.empty())items=std::move(rollback);rollback.clear();Stop();note=L"Not enough memory to apply the configuration. The session was stopped safely.";EditingEnabled(true);RebuildDevices();if(closing)DestroyWindow(h);else InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_CLOSE:if(working){closing=true;CancelSynchronousIo(worker.native_handle());note=L"Cancelling the current operation before closing...";InvalidateRect(h,nullptr,FALSE);return 0;}DestroyWindow(h);return 0;
    case WM_DESTROY:KillTimer(h,PollTimer);if(worker.joinable()){CancelSynchronousIo(worker.native_handle());worker.join();}if(session)session->Stop();session.reset();for(auto font:{body,smallFont,title,emphasis})if(font)DeleteObject(font);if(paper)DeleteObject(paper);if(white)DeleteObject(white);PostQuitMessage(0);return 0;
    }}catch(const std::exception& error){Error(error);if(message==WM_CREATE)return -1;return 0;}
    return DefWindowProcW(h,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    int count=0;wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 1;
    for(int i=1;i<count;++i){std::wstring arg=args[i];if(arg==L"--preview")preview=true;else if(arg==L"--ui-self-test"){selfTest=true;preview=true;}else if(arg==L"--screenshot"&&i+1<count){screenshot=args[++i];selfTest=true;preview=true;}else{LocalFree(args);return 2;}}LocalFree(args);
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))return 1;
    SetProcessDPIAware();INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=Proc;cls.lpszClassName=L"TitanHidAccessWindow";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);if(!RegisterClassExW(&cls)){CoUninitialize();return 1;}
    HWND h=CreateWindowExW(0,cls.lpszClassName,L"HID Access Control",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,100,60,1180,960,nullptr,nullptr,instance,nullptr);if(!h){CoUninitialize();return 1;}
    if(selfTest){bool ok=preview&&items.size()==3&&ListView_GetItemCount(deviceList)==2&&ListView_GetItemCount(logList)==2&&Editable(0)&&!Editable(1);try{showProtected=true;RebuildDevices();ok=ok&&ListView_GetItemCount(deviceList)==3;showProtected=false;RebuildDevices();StartSession();ok=ok&&active;Stop();ok=ok&&!active;ok=ok&&CompactLayout();}catch(...){ok=false;}if(ok&&!screenshot.empty())ok=SaveScreenshot(screenshot);DestroyWindow(h);CoUninitialize();return ok?0:1;}
    ShowWindow(h,show);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(h,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    CoUninitialize();return 0;
}
