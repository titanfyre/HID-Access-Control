#include "HIDAccessControlJsonSDK.h"
#include <string.h>
int main(void){uint64_t session=0;if(hid_access_control_json_open(&session))return 1;char *reply=hid_access_control_json_call(session,"{",1);if(!reply){hid_access_control_json_close(session);return 2;}int failed=strstr(reply,"\"ok\":false")==0;hid_access_control_json_free(reply);if(hid_access_control_json_close(session))return 3;return failed;}
