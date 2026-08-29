#include "app_shell.h"

namespace bluepaws::ui {
namespace {

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

lv_obj_t *make_header_button(lv_obj_t *header,
                             int32_t right_offset,
                             const char *text,
                             lv_event_cb_t callback,
                             void *user_data)
{
    lv_obj_t *button = lv_button_create(header);
    lv_obj_set_size(button, 66, 38);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, right_offset, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2B5878), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x71B9E6), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = make_label(button, text, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    return button;
}

}  // namespace

lv_obj_t *create_page_frame(lv_obj_t *screen,
                            const char *title_text,
                            const char *initial_status,
                            const PageActions &actions,
                            lv_obj_t **status_label)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xEAF1F5), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_gap(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(header, title_text, lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(title, 18, 6);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *status = make_label(header, initial_status, lv_color_hex(0x9FD8FF));
    lv_obj_set_pos(status, 18, 34);
    lv_obj_set_width(status, actions.home != nullptr ? LV_PCT(58) : LV_PCT(72));
    lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);
    if (status_label != nullptr) {
        *status_label = status;
    }

    if (actions.rotate != nullptr) {
        make_header_button(header, -10, "ROT", actions.rotate, actions.user_data);
    }
    if (actions.home != nullptr) {
        make_header_button(header, -84, "APPS", actions.home, actions.user_data);
    }

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, LV_PCT(100), 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_gap(content, 8, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    return content;
}

lv_obj_t *create_app_tile(lv_obj_t *parent,
                          int32_t width,
                          int32_t height,
                          const char *title_text,
                          const char *detail,
                          uint32_t colour,
                          lv_event_cb_t callback,
                          void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_grad_color(button, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0x7392A6), 0);
    lv_obj_set_style_shadow_width(button, 8, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(button, 18, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *title = make_label(button, title_text, lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *description = make_label(button, detail, lv_color_hex(0xD8EEFB));
    lv_obj_set_pos(description, 0, 42);
    lv_obj_set_width(description, width - 36);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, 0);
    return button;
}

}  // namespace bluepaws::ui
