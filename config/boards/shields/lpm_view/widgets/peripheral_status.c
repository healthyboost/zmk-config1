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

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

static void rotate_canvas_180_in_place(lv_obj_t *canvas);

static void draw_status(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t foreground;
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&foreground, LVGL_FOREGROUND);
    init_rect_dsc(&background, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label_right;
    init_label_dsc(&label_right, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t label_center;
    init_label_dsc(&label_center, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_center_inv;
    init_label_dsc(&label_center_inv, LVGL_BACKGROUND, &lv_font_montserrat_16,
                   LV_TEXT_ALIGN_CENTER);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc;
    init_arc_dsc(&arc, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_filled;
    init_arc_dsc(&arc_filled, LVGL_FOREGROUND, 9);

    /* Fill background */
    canvas_draw_rect(canvas, 0, 0, PERIPHERAL_CANVAS_WIDTH, PERIPHERAL_CANVAS_HEIGHT, &background);

    /* Same battery visual language as the left-hand screen. */
    draw_battery(canvas, state);

    char battery_text[6];
    uint8_t level = state->battery > 100 ? 100 : state->battery;
    snprintf(battery_text, sizeof(battery_text), "%d%%", level);
    canvas_draw_text(canvas, 36, 0, 54, &label_right, battery_text);

    /* Right-hand screen plays the role of the left screen's output symbol. */
    char link_symbol[6] = {};
    strcat(link_symbol, state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    canvas_draw_text(canvas, 112, 0, 32, &label_right, link_symbol);

    /* Outlined meter block, echoing the left screen's WPM frame. */
    canvas_draw_rect(canvas, 2, 18, 140, 28, &foreground);
    canvas_draw_rect(canvas, 3, 19, 138, 26, &background);

    if (state->connected) {
        for (int i = 0; i < 4; i++) {
            int bar_height = 8 + (i * 4);
            canvas_draw_rect(canvas, 9 + (i * 9), 43 - bar_height, 6, bar_height, &foreground);
        }
        canvas_draw_text(canvas, 52, 24, 86, &label_right, "LINKED");
    } else {
        lv_point_t cross_a[2] = {{15, 24}, {39, 42}};
        lv_point_t cross_b[2] = {{39, 24}, {15, 42}};
        canvas_draw_line(canvas, cross_a, 2, &line);
        canvas_draw_line(canvas, cross_b, 2, &line);
        canvas_draw_text(canvas, 52, 24, 86, &label_right, "LOST");
    }

    /* Two nodes for the two halves, echoing the left screen's profile circles. */
    if (state->connected) {
        canvas_draw_arc(canvas, 36, 59, 5, 0, 360, &arc_solid);
        canvas_draw_arc(canvas, 108, 59, 5, 0, 360, &arc_solid);
        canvas_draw_text(canvas, 26, 52, 20, &label_center_inv, "L");
        canvas_draw_text(canvas, 98, 52, 20, &label_center_inv, "R");

        lv_point_t link[2] = {{47, 59}, {97, 59}};
        canvas_draw_line(canvas, link, 2, &line);
    } else {
        canvas_draw_arc(canvas, 36, 59, 9, 0, 360, &arc);
        canvas_draw_arc(canvas, 108, 59, 9, 0, 360, &arc);
        canvas_draw_text(canvas, 26, 52, 20, &label_center, "L");
        canvas_draw_text(canvas, 98, 52, 20, &label_center, "R");

        lv_point_t left_link[2] = {{47, 59}, {64, 59}};
        lv_point_t right_link[2] = {{80, 59}, {97, 59}};
        lv_point_t break_a[2] = {{68, 54}, {76, 64}};
        lv_point_t break_b[2] = {{76, 54}, {68, 64}};
        canvas_draw_line(canvas, left_link, 2, &line);
        canvas_draw_line(canvas, right_link, 2, &line);
        canvas_draw_line(canvas, break_a, 2, &line);
        canvas_draw_line(canvas, break_b, 2, &line);
    }

    rotate_canvas_180_in_place(canvas);
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

static void rotate_canvas_180_in_place(lv_obj_t *canvas) {
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(canvas);
    uint8_t *buf = draw_buf->data;
    uint32_t stride = lv_draw_buf_width_to_stride(PERIPHERAL_CANVAS_WIDTH, CANVAS_COLOR_FORMAT);

    for (uint16_t y = 0; y < PERIPHERAL_CANVAS_HEIGHT / 2; y++) {
        for (uint16_t x = 0; x < PERIPHERAL_CANVAS_WIDTH; x++) {
            uint16_t opposite_x = PERIPHERAL_CANVAS_WIDTH - 1 - x;
            uint16_t opposite_y = PERIPHERAL_CANVAS_HEIGHT - 1 - y;
            uint8_t tmp = buf[y * stride + x];
            buf[y * stride + x] = buf[opposite_y * stride + opposite_x];
            buf[opposite_y * stride + opposite_x] = tmp;
        }
    }
}

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, PERIPHERAL_CANVAS_WIDTH, PERIPHERAL_CANVAS_HEIGHT);

    lv_obj_t *canvas = lv_canvas_create(widget->obj);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(canvas, widget->cbuf, PERIPHERAL_CANVAS_WIDTH,
                         PERIPHERAL_CANVAS_HEIGHT, CANVAS_COLOR_FORMAT);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }