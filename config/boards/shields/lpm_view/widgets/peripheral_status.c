/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#include "peripheral_status.h"

LV_IMG_DECLARE(bolt);

#define PAGE_SWITCH_SECONDS 6

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static atomic_t key_count = ATOMIC_INIT(0);
static atomic_t link_since_ms = ATOMIC_INIT(0);
static atomic_t ui_page = ATOMIC_INIT(0);

struct peripheral_status_state {
    bool connected;
};

struct activity_status_state {
    uint32_t key_count;
};

static void format_uptime(char *buf, size_t len, uint32_t total_seconds) {
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds / 60) % 60;
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
}

static void format_duration(char *buf, size_t len, uint32_t total_seconds) {
    uint32_t minutes = total_seconds / 60;
    uint32_t seconds = total_seconds % 60;
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
}

static uint32_t current_link_seconds(bool connected) {
    if (!connected) {
        return 0;
    }

    uint32_t since = (uint32_t)atomic_get(&link_since_ms);
    if (since == 0) {
        return 0;
    }

    return (k_uptime_get_32() - since) / 1000;
}

static void draw_battery_icon(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t rect_foreground;
    lv_draw_rect_dsc_t rect_background;
    init_rect_dsc(&rect_foreground, LVGL_FOREGROUND);
    init_rect_dsc(&rect_background, LVGL_BACKGROUND);

    uint8_t level = state->battery > 100 ? 100 : state->battery;
    int fill_width = (level * 34) / 100;

    canvas_draw_rect(canvas, 18, 6, 36, 16, &rect_foreground);
    canvas_draw_rect(canvas, 19, 7, 34, 14, &rect_background);

    if (fill_width > 0) {
        canvas_draw_rect(canvas, 20, 8, fill_width, 12, &rect_foreground);
    }

    canvas_draw_rect(canvas, 54, 10, 3, 8, &rect_foreground);

    if (state->charging) {
        lv_draw_image_dsc_t image;
        lv_draw_image_dsc_init(&image);
        canvas_draw_img(canvas, 28, -2, &bolt, &image);
    }
}

static void draw_page_dots(lv_obj_t *canvas, uint8_t page) {
    lv_draw_arc_dsc_t filled;
    lv_draw_arc_dsc_t ring;
    init_arc_dsc(&filled, LVGL_FOREGROUND, 4);
    init_arc_dsc(&ring, LVGL_FOREGROUND, 1);

    for (uint8_t i = 0; i < 2; i++) {
        if (i == page) {
            canvas_draw_arc(canvas, 32 + (i * 8), 65, 2, 0, 360, &filled);
        } else {
            canvas_draw_arc(canvas, 32 + (i * 8), 65, 2, 0, 360, &ring);
        }
    }
}

/* Page 1, top tile: big battery percentage. */
static void draw_status_top(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_big;
    lv_draw_label_dsc_t label_small;
    init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&label_small, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    draw_battery_icon(canvas, state);

    uint8_t level = state->battery > 100 ? 100 : state->battery;
    char battery_text[8];
    snprintf(battery_text, sizeof(battery_text), "%u%%", (unsigned int)level);
    canvas_draw_text(canvas, 0, 23, CANVAS_SIZE, &label_big, battery_text);

    const char *power_text = "BAT";
    if (state->charging) {
        power_text = "USB POWER";
    } else if (level <= 20) {
        power_text = "LOW BAT";
    }
    canvas_draw_text(canvas, 0, 59, CANVAS_SIZE, &label_small, power_text);

    rotate_canvas(canvas);
}

/* Page 1, bottom tile: link state, key count, uptime. */
static void draw_status_bottom(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_arc_dsc_t arc_solid;
    lv_draw_arc_dsc_t arc_ring;
    init_arc_dsc(&arc_solid, LVGL_FOREGROUND, 16);
    init_arc_dsc(&arc_ring, LVGL_FOREGROUND, 2);

    lv_draw_label_dsc_t label_center;
    lv_draw_label_dsc_t label_center_inv;
    lv_draw_label_dsc_t label_small_left;
    lv_draw_label_dsc_t label_small_right;
    init_label_dsc(&label_center, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&label_center_inv, LVGL_BACKGROUND, &lv_font_montserrat_16,
                   LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&label_small_left, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_LEFT);
    init_label_dsc(&label_small_right, LVGL_FOREGROUND, &lv_font_montserrat_12,
                   LV_TEXT_ALIGN_RIGHT);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 3);

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    if (state->connected) {
        canvas_draw_arc(canvas, 27, 19, 8, 0, 360, &arc_solid);
        canvas_draw_arc(canvas, 45, 19, 8, 0, 360, &arc_solid);
        canvas_draw_text(canvas, 19, 11, 16, &label_center_inv, "L");
        canvas_draw_text(canvas, 37, 11, 16, &label_center_inv, "R");

        lv_point_t link[2] = {{35, 19}, {37, 19}};
        canvas_draw_line(canvas, link, 2, &line);
        canvas_draw_text(canvas, 0, 39, CANVAS_SIZE, &label_center, "LINKED");
    } else {
        canvas_draw_arc(canvas, 27, 19, 12, 0, 360, &arc_ring);
        canvas_draw_arc(canvas, 45, 19, 12, 0, 360, &arc_ring);
        canvas_draw_text(canvas, 19, 11, 16, &label_center, "L");
        canvas_draw_text(canvas, 37, 11, 16, &label_center, "R");

        lv_point_t break_a[2] = {{22, 14}, {30, 24}};
        lv_point_t break_b[2] = {{30, 14}, {22, 24}};
        canvas_draw_line(canvas, break_a, 2, &line);
        canvas_draw_line(canvas, break_b, 2, &line);
        canvas_draw_text(canvas, 0, 39, CANVAS_SIZE, &label_center, "LOST");
    }

    char keys_text[16];
    char uptime_text[12];
    snprintf(keys_text, sizeof(keys_text), "K%lu", (unsigned long)atomic_get(&key_count));
    format_uptime(uptime_text, sizeof(uptime_text), k_uptime_get_32() / 1000);
    canvas_draw_text(canvas, 2, 57, 30, &label_small_left, keys_text);
    canvas_draw_text(canvas, 40, 57, 30, &label_small_right, uptime_text);

    draw_page_dots(canvas, 0);
    rotate_canvas(canvas);
}

/* Page 2, top tile: system info and uptime. */
static void draw_info_top(lv_obj_t *canvas) {
    lv_draw_label_dsc_t label_title;
    lv_draw_label_dsc_t label_big;
    lv_draw_label_dsc_t label_small;
    init_label_dsc(&label_title, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&label_big, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&label_small, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 1);

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    canvas_draw_text(canvas, 0, 8, CANVAS_SIZE, &label_title, "INFO");

    lv_point_t divider[2] = {{6, 27}, {66, 27}};
    canvas_draw_line(canvas, divider, 2, &line);

    char uptime_text[12];
    format_uptime(uptime_text, sizeof(uptime_text), k_uptime_get_32() / 1000);
    canvas_draw_text(canvas, 0, 33, CANVAS_SIZE, &label_big, uptime_text);
    canvas_draw_text(canvas, 0, 59, CANVAS_SIZE, &label_small, "UPTIME");

    rotate_canvas(canvas);
}

/* Page 2, bottom tile: key count and link duration. */
static void draw_info_bottom(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 1);

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    char keys_text[16];
    char link_text[16];
    snprintf(keys_text, sizeof(keys_text), "KEY %lu", (unsigned long)atomic_get(&key_count));

    if (state->connected) {
        format_duration(link_text, sizeof(link_text), current_link_seconds(true));
    } else {
        snprintf(link_text, sizeof(link_text), "LINK --:--");
    }

    canvas_draw_text(canvas, 0, 12, CANVAS_SIZE, &label, keys_text);

    lv_point_t divider[2] = {{6, 34}, {66, 34}};
    canvas_draw_line(canvas, divider, 2, &line);

    char link_label[24];
    if (state->connected) {
        snprintf(link_label, sizeof(link_label), "LINK %s", link_text);
    } else {
        snprintf(link_label, sizeof(link_label), "%s", link_text);
    }
    canvas_draw_text(canvas, 0, 42, CANVAS_SIZE, &label, link_label);

    draw_page_dots(canvas, 1);
    rotate_canvas(canvas);
}

static void draw_status(lv_obj_t *widget, const struct status_state *state) {
    if (atomic_get(&ui_page) != 0) {
        draw_info_top(lv_obj_get_child(widget, 0));
        draw_info_bottom(lv_obj_get_child(widget, 1), state);
    } else {
        draw_status_top(lv_obj_get_child(widget, 0), state);
        draw_status_bottom(lv_obj_get_child(widget, 1), state);
    }
}

static void redraw_work_cb(struct k_work *work) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_status(widget->obj, &widget->state); }
}

K_WORK_DEFINE(redraw_work, redraw_work_cb);

static struct k_work_delayable page_work;

static void page_work_cb(struct k_work *work) {
    atomic_set(&ui_page, atomic_get(&ui_page) == 0 ? 1 : 0);

    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }

    k_work_reschedule(&page_work, K_SECONDS(PAGE_SWITCH_SECONDS));
}



static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_status(widget->obj, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state) {
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static struct peripheral_status_state get_state(const zmk_event_t *_eh) {
    bool connected = zmk_split_bt_peripheral_is_connected();

    if (connected) {
        if (atomic_get(&link_since_ms) == 0) {
            atomic_set(&link_since_ms, k_uptime_get_32());
        }
    } else {
        atomic_set(&link_since_ms, 0);
    }

    return (struct peripheral_status_state){.connected = connected};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;

    draw_status(widget->obj, &widget->state);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

static struct activity_status_state activity_status_get_state(const zmk_event_t *eh) {
    if (eh != NULL) {
        const struct zmk_position_state_changed *event =
            as_zmk_position_state_changed(eh);
        if (event != NULL && event->state) {
            atomic_inc(&key_count);
        }
    }

    return (struct activity_status_state){.key_count = (uint32_t)atomic_get(&key_count)};
}

static void activity_status_update_cb(struct activity_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_status(widget->obj, &widget->state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_activity_status, struct activity_status_state,
                            activity_status_update_cb, activity_status_get_state)
ZMK_SUBSCRIPTION(widget_activity_status, zmk_position_state_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, PERIPHERAL_CANVAS_WIDTH, PERIPHERAL_CANVAS_HEIGHT);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, CANVAS_SIZE, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();
    widget_activity_status_init();
    k_work_init_delayable(&page_work, page_work_cb);
    k_work_schedule(&page_work, K_SECONDS(PAGE_SWITCH_SECONDS));

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }