#include "application_c_access.h"

#include "application.h"

extern "C" EventGroupHandle_t application_main_event_group(void) {
    return Application::GetInstance().GetMainEventGroupHandle();
}
