/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>

#include "peripheral_status.h"

#define PERIPHERAL_WIDTH 144
#define PERIPHERAL_HEIGHT 72
#define PERIPHERAL_PANEL_SIZE 72

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

static void draw_battery_panel(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t foreground;
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&foreground, LVGL_FOREGROUND);
    init_rect_dsc(&background, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);

    canvas_draw_rect(canvas, 0, 0, PERIPHERAL_PANEL_SIZE, PERIPHERAL_PANEL_SIZE, &background);

    /* Battery outline and terminal. */
    canvas_draw_rect(canvas, 8, 16, 52, 34, &foreground);
    canvas_draw_rect(canvas, 10, 18, 48, 30, &background);
    canvas_draw_rect(canvas, 60, 27, 4, 12, &foreground);

    uint8_t level = state->battery > 100 ? 100 : state->battery;
    int fill_width = level * 44 / 100;

    if (fill_width > 0) {
        canvas_draw_rect(canvas, 12, 20, fill_width, 26, &foreground);
    }

    char battery_text[6];
    snprintf(battery_text, sizeof(battery_text), "%d%%", level);
    canvas_draw_text(canvas, 0, 51, PERIPHERAL_PANEL_SIZE, &label, battery_text);

    if (state->charging) {
        canvas_draw_text(canvas, 0, 0, PERIPHERAL_PANEL_SIZE, &label, "CHG");
    }

    rotate_canvas(canvas);
}

static void draw_link_panel(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t foreground;
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&foreground, LVGL_FOREGROUND);
    init_rect_dsc(&background, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 3);

    canvas_draw_rect(canvas, 0, 0, PERIPHERAL_PANEL_SIZE, PERIPHERAL_PANEL_SIZE, &background);
    canvas_draw_text(canvas, 0, 2, PERIPHERAL_PANEL_SIZE, &label, "RIGHT");

    if (state->connected) {
        /* Signal bars: three short bars and one tall bar. */
        canvas_draw_rect(canvas, 16, 39, 7, 9, &foreground);
        canvas_draw_rect(canvas, 28, 34, 7, 14, &foreground);
        canvas_draw_rect(canvas, 40, 29, 7, 19, &foreground);
        canvas_draw_rect(canvas, 52, 24, 7, 24, &foreground);
        canvas_draw_text(canvas, 0, 51, PERIPHERAL_PANEL_SIZE, &label, "LINK");
    } else {
        lv_point_t cross_a[2] = {{23, 23}, {49, 49}};
        lv_point_t cross_b[2] = {{49, 23}, {23, 49}};
        canvas_draw_line(canvas, cross_a, 2, &line);
        canvas_draw_line(canvas, cross_b, 2, &line);
        canvas_draw_text(canvas, 0, 51, PERIPHERAL_PANEL_SIZE, &label, "LOST");
    }

    rotate_canvas(canvas);
}

static void draw_status(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *battery_canvas = lv_obj_get_child(widget, 0);
    lv_obj_t *link_canvas = lv_obj_get_child(widget, 1);

    draw_battery_panel(battery_canvas, state);
    draw_link_panel(link_canvas, state);
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
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
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

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, PERIPHERAL_WIDTH, PERIPHERAL_HEIGHT);

    lv_obj_t *battery_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(battery_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(battery_canvas, widget->cbuf, PERIPHERAL_PANEL_SIZE,
                         PERIPHERAL_PANEL_SIZE, CANVAS_COLOR_FORMAT);

    lv_obj_t *link_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(link_canvas, LV_ALIGN_TOP_LEFT, PERIPHERAL_PANEL_SIZE, 0);
    lv_canvas_set_buffer(link_canvas, widget->cbuf2, PERIPHERAL_PANEL_SIZE,
                         PERIPHERAL_PANEL_SIZE, CANVAS_COLOR_FORMAT);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }