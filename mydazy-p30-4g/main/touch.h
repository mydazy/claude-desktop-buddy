#pragma once

// Bring up I²C1 (shared with codec on this board) + AXS5106L touch IC,
// register it as an LVGL pointer indev. Must be called AFTER display_init()
// because attach_lvgl needs the lv_display_t in place.
//
// On success, every lv_obj with LV_OBJ_FLAG_CLICKABLE will receive
// LV_EVENT_CLICKED on tap. The Approve/Deny prompt buttons rely on this.
void touch_init();
