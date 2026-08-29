#include "app_shell.h"

#include "ui_icons.h"

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
                             const lv_image_dsc_t &icon,
                             lv_color_t icon_colour,
                             bool dark_mode,
                             lv_event_cb_t callback,
                             void *user_data)
{
    lv_obj_t *button = lv_button_create(header);
    lv_obj_set_size(button, 42, 42);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, right_offset, 0);
    lv_obj_set_style_bg_color(button,
                              dark_mode ? lv_color_hex(0x243444) : lv_color_hex(0xFFFFFF),
                              0);
    lv_obj_set_style_bg_opa(button, dark_mode ? LV_OPA_50 : LV_OPA_70, 0);
    lv_obj_set_style_border_color(button,
                                  dark_mode ? lv_color_hex(0x60788C) : lv_color_hex(0xB5C7D3),
                                  0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 11, 0);
    lv_obj_set_style_pad_all(button, 6, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *image = lv_image_create(button);
    lv_image_set_src(image, &icon);
    lv_obj_set_style_image_recolor(image, icon_colour, 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_center(image);
    return button;
}

}  // namespace

lv_obj_t *create_page_frame(lv_obj_t *screen,
                            const char *title_text,
                            const char *initial_status,
                            bool dark_mode,
                            const PageActions &actions,
                            lv_obj_t **status_label)
{
    const lv_color_t screen_colour = dark_mode ? lv_color_hex(0x0B1118) : lv_color_hex(0xEAF1F5);
    const lv_color_t header_colour = dark_mode ? lv_color_hex(0x101B25) : lv_color_hex(0xE5EEF4);
    const lv_color_t text_colour = dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D);
    const lv_color_t secondary_colour = dark_mode ? lv_color_hex(0x80C9F2) : lv_color_hex(0x28709A);

    lv_obj_set_style_bg_color(screen, screen_colour, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_gap(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(header, header_colour, 0);
    lv_obj_set_style_border_color(header,
                                  dark_mode ? lv_color_hex(0x263A4A) : lv_color_hex(0xC0D0DA),
                                  0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(header, title_text, text_colour);
    lv_obj_set_pos(title, 16, 6);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *status = make_label(header, initial_status, secondary_colour);
    lv_obj_set_pos(status, 16, 34);
    lv_obj_set_width(status, actions.home != nullptr ? LV_PCT(55) : LV_PCT(67));
    lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);
    if (status_label != nullptr) {
        *status_label = status;
    }

    const lv_color_t icon_colour = dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D);
    int32_t right_offset = -8;
    if (actions.theme != nullptr) {
        make_header_button(header,
                           right_offset,
                           icon_night_mode,
                           icon_colour,
                           dark_mode,
                           actions.theme,
                           actions.user_data);
        right_offset -= 48;
    }
    if (actions.rotate != nullptr) {
        make_header_button(header,
                           right_offset,
                           icon_rotate,
                           icon_colour,
                           dark_mode,
                           actions.rotate,
                           actions.user_data);
        right_offset -= 48;
    }
    if (actions.home != nullptr) {
        make_header_button(header,
                           right_offset,
                           icon_home,
                           icon_colour,
                           dark_mode,
                           actions.home,
                           actions.user_data);
    }

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, LV_PCT(100), 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_color(content, screen_colour, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
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
                          const lv_image_dsc_t *icon,
                          const char *fallback_symbol,
                          bool recolour_icon,
                          uint32_t colour,
                          bool dark_mode,
                          lv_event_cb_t callback,
                          void *user_data)
{
    const lv_color_t label_colour = dark_mode ? lv_color_hex(0xF4F7F9) : lv_color_hex(0x17324D);
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    const int32_t plate_size = height - 42;
    lv_obj_t *plate = lv_obj_create(button);
    lv_obj_set_size(plate, plate_size, plate_size);
    lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(plate, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_grad_color(plate,
                                   dark_mode ? lv_color_hex(0x162532) : lv_color_hex(0xFFFFFF),
                                   0);
    lv_obj_set_style_bg_grad_dir(plate, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(plate,
                                  dark_mode ? lv_color_hex(0x71899B) : lv_color_hex(0xFFFFFF),
                                  0);
    lv_obj_set_style_border_width(plate, 1, 0);
    lv_obj_set_style_radius(plate, 22, 0);
    lv_obj_set_style_shadow_color(plate, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(plate, 12, 0);
    lv_obj_set_style_shadow_opa(plate, dark_mode ? LV_OPA_50 : LV_OPA_20, 0);
    lv_obj_set_style_pad_all(plate, 8, 0);
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(plate, LV_OBJ_FLAG_CLICKABLE);

    if (icon != nullptr) {
        lv_obj_t *image = lv_image_create(plate);
        lv_image_set_src(image, icon);
        if (recolour_icon) {
            lv_obj_set_style_image_recolor(image, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
        }
        lv_obj_center(image);
    } else {
        lv_obj_t *symbol = make_label(plate,
                                      fallback_symbol != nullptr ? fallback_symbol : LV_SYMBOL_LIST,
                                      lv_color_hex(0xFFFFFF));
        lv_obj_set_style_text_font(symbol, &lv_font_montserrat_22, 0);
        lv_obj_center(symbol);
    }

    lv_obj_t *title = make_label(button, title_text, label_colour);
    lv_obj_align(title, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    return button;
}

}  // namespace bluepaws::ui
