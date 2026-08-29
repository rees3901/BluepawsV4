#pragma once

#include "lvgl.h"

namespace bluepaws::ui {

struct PageActions {
    lv_event_cb_t home = nullptr;
    lv_event_cb_t rotate = nullptr;
    void *user_data = nullptr;
};

lv_obj_t *create_page_frame(lv_obj_t *screen,
                            const char *title,
                            const char *initial_status,
                            const PageActions &actions,
                            lv_obj_t **status_label);

lv_obj_t *create_app_tile(lv_obj_t *parent,
                          int32_t width,
                          int32_t height,
                          const char *title,
                          const char *detail,
                          uint32_t colour,
                          lv_event_cb_t callback,
                          void *user_data);

}  // namespace bluepaws::ui
