#include "SDK/HidAccess.h"
#include "WireTypes.h"
#include <map>
struct ProductSession{

std::unique_ptr<HidAccess::Session> session;
HidAccess::Session& Get(){if(!session)session=std::make_unique<HidAccess::Session>();return *session;}
Json Status(const HidAccess::Status& s){return {{"installed",s.installed},{"running",s.running},{"active",s.active},{"demo_expired",s.demo_expired},{"remaining_100ns",std::to_string(s.remaining_100ns)},{"generation",std::to_string(s.generation)},{"allowed_opens",std::to_string(s.allowed_opens)},{"denied_opens",std::to_string(s.denied_opens)},{"dropped_events",std::to_string(s.dropped_events)},{"detail",Utf8(s.detail)}};}
std::vector<HidAccess::Rule> Rules(const Json& a){auto array=a.at("rules");Require(array.is_array() && array.size()<=256,"Too many rules");std::vector<HidAccess::Rule> rules;for(const auto& value:array){HidAccess::Rule r;r.device=value.at("device").get<std::string>();Require(!r.device.empty() && r.device.size()<=4096,"Invalid device token");r.mode=static_cast<HidAccess::Mode>(Number(value,"mode",0,2));auto apps=value.value("applications",Json::array());Require(apps.is_array() && apps.size()<=256,"Too many application paths");for(const auto& path:apps)r.applications.push_back(Wide(path.get<std::string>()));rules.push_back(std::move(r));}return rules;}

Json Invoke(const std::string& op,const Json& a){

if(op=="check"){auto rules=Rules(a);return {{"valid",true},{"rule_count",rules.size()},{"scope","JSON shape only; Apply performs native semantic validation"}};}
if(op=="probe")return Status(HidAccess::Probe());
if(op=="status")return Status(Get().GetStatus());
if(op=="devices"){Json devices=Json::array();for(const auto& d:Get().Devices())devices.push_back({{"token",d.token},{"instance_id",Utf8(d.instance_id)},{"name",Utf8(d.name)},{"vid",d.vid},{"pid",d.pid},{"supported",d.supported},{"protected_input",d.protected_input},{"flags",d.flags}});return devices;}
if(op=="apply"){Get().Apply(Rules(a));return Json::object();}
if(op=="stop"){if(session)session->Stop();return Json::object();}
if(op=="read_events"){Json events=Json::array();for(const auto& e:Get().ReadEvents())events.push_back({{"sequence",std::to_string(e.sequence)},{"timestamp_100ns",std::to_string(e.timestamp_100ns)},{"device_id",std::to_string(e.device_id)},{"process_id",e.process_id},{"denied",e.denied},{"application",Utf8(e.application)},{"reason",Utf8(e.reason)}});return events;}

throw SdkError(50,"Unknown operation");}
};
static Runtime<ProductSession> runtime;
extern "C" __declspec(dllexport) unsigned hid_access_control_json_open(std::uint64_t* id)noexcept{return runtime.Open(id);}
extern "C" __declspec(dllexport) unsigned hid_access_control_json_close(std::uint64_t id)noexcept{return runtime.Close(id);}
extern "C" __declspec(dllexport) char* hid_access_control_json_call(std::uint64_t id,const char* request,std::uint32_t length)noexcept{return runtime.Call(id,request,length);}
extern "C" __declspec(dllexport) void hid_access_control_json_free(char* response)noexcept{delete[] response;}
