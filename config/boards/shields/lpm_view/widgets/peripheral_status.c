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

/* Top tile: battery on top, link state in the outlined block below.
 * Mirrors the left-hand screen's battery + WPM tile layout. */
static void draw_battery_tile(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t rect_foreground;
    lv_draw_rect_dsc_t rect_background;
    init_rect_dsc(&rect_foreground, LVGL_FOREGROUND);
    init_rect_dsc(&rect_background, LVGL_BACKGROUND);

    lv_draw_label_dsc_t label_right;
    init_label_dsc(&label_right, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 2);

    // Fill background
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    // Draw battery
    draw_battery(canvas, state);

    char battery_text[6];
    uint8_t level = state->battery > 100 ? 100 : state->battery;
    snprintf(battery_text, sizeof(battery_text), "%d%%", level);
    canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_right, battery_text);

    // Outlined block, same geometry as the left screen's WPM frame
    canvas_draw_rect(canvas, 0, 21, 70, 32, &rect_foreground);
    canvas_draw_rect(canvas, 1, 22, 66, 30, &rect_background);

    if (state->connected) {
        for (int i = 0; i < 3; i++) {
            int bar_height = 8 + (i * 4);
            canvas_draw_rect(canvas, 6 + (i * 8), 44 - bar_height, 5, bar_height, &rect_foreground);
        }
        canvas_draw_text(canvas, 28, 30, 38, &label_right, "LINK");
    } else {
        lv_point_t cross_a[2] = {{10, 29}, {28, 47}};
        lv_point_t cross_b[2] = {{28, 29}, {10, 47}};
        canvas_draw_line(canvas, cross_a, 2, &line);
        canvas_draw_line(canvas, cross_b, 2, &line);
        canvas_draw_text(canvas, 28, 30, 38, &label_right, "LOST");
    }

    // Rotate canvas
    rotate_canvas(canvas);
}

/* Bottom tile: one node per half, echoing the left screen's profile circles. */
static void draw_link_tile(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_arc_dsc_t arc_ring;
    init_arc_dsc(&arc_ring, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_solid;
    init_arc_dsc(&arc_solid, LVGL_FOREGROUND, 12);

    lv_draw_label_dsc_t label_center;
    init_label_dsc(&label_center, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_center_inv;
    init_label_dsc(&label_center_inv, LVGL_BACKGROUND, &lv_font_montserrat_16,
                   LV_TEXT_ALIGN_CENTER);

    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 2);

    // Fill background
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    if (state->connected) {
        canvas_draw_arc(canvas, 18, 36, 6, 0, 360, &arc_solid);
        canvas_draw_arc(canvas, 54, 36, 6, 0, 360, &arc_solid);
        canvas_draw_text(canvas, 8, 28, 20, &label_center_inv, "L");
        canvas_draw_text(canvas, 44, 28, 20, &label_center_inv, "R");

        lv_point_t link[2] = {{31, 36}, {41, 36}};
        canvas_draw_line(canvas, link, 2, &line);
    } else {
        canvas_draw_arc(canvas, 18, 36, 12, 0, 360, &arc_ring);
        canvas_draw_arc(canvas, 54, 36, 12, 0, 360, &arc_ring);
        canvas_draw_text(canvas, 8, 28, 20, &label_center, "L");
        canvas_draw_text(canvas, 44, 28, 20, &label_center, "R");

        lv_point_t left_link[2] = {{31, 36}, {34, 36}};
        lv_point_t right_link[2] = {{38, 36}, {41, 36}};
        lv_point_t break_a[2] = {{34, 32}, {38, 40}};
        lv_point_t break_b[2] = {{38, 32}, {34, 40}};
        canvas_draw_line(canvas, left_link, 2, &line);
        canvas_draw_line(canvas, right_link, 2, &line);
        canvas_draw_line(canvas, break_a, 2, &line);
        canvas_draw_line(canvas, break_b, 2, &line);
    }

    // Rotate canvas
    rotate_canvas(canvas);
}

static void draw_status(lv_obj_t *widget, const struct status_state *state) {
    draw_battery_tile(lv_obj_get_child(widget, 0), state);
    draw_link_tile(lv_obj_get_child(widget, 1), state);
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
    lv_obj_set_size(widget->obj, PERIPHERAL_CANVAS_WIDTH, PERIPHERAL_CANVAS_HEIGHT);

    // top tile: battery + link state
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    // bottom tile: left/right link nodes
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, CANVAS_SIZE, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }