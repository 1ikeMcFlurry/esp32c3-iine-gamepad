/* Simulated inputs for README artwork, rendered with the firmware's C UI. */
#include "gamepad_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    for (int frame = 0; frame < 64; ++frame) {
        gamepad_ui_t ui;
        gamepad_ui_init(&ui);
        ui.page = UI_LIVE; ui.link = GP_READY; ui.vendor = true; ui.decoded = true;
        ui.protocol_stage = IINE_RECEIVING; ui.reports = 358 + frame;
        ui.mapped_buttons = 0xfff; ui.mapped_axes = ui.input.axes_present = 63;
        strcpy(ui.active.name, "IINE GAMEPAD");
        for (int i = 0; i < 6; ++i) {
            ui.minimum[i] = 0; ui.maximum[i] = 255; ui.input.axes[i] = 128;
        }
        ui.input.axes[2] = ui.input.axes[5] = 0;
        if (frame < 32) {
            int axis = frame < 16 ? 0 : 3;
            double angle = (frame % 16) * 6.28318530718 / 16;
            ui.input.axes[axis] = 128 + (int)(110 * cos(angle));
            ui.input.axes[axis + 1] = 128 + (int)(110 * sin(angle));
        } else if (frame < 48) {
            if (frame % 4 < 3) ui.input.buttons = 1u << ((frame - 32) / 4);
        } else if (frame < 56) {
            static const unsigned directions[] = {1, 4, 2, 8};
            ui.input.dpad = directions[(frame - 48) / 2];
        } else if (frame < 62) {
            ui.input.buttons = (1u << 0) | (1u << 1) | (1u << 6);
            ui.input.axes[2] = 255;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/motion-%02d.ppm", argv[1], frame);
        FILE *file = fopen(path, "wb");
        if (!file) return 1;
        fprintf(file, "P6\n240 320\n255\n");
        uint16_t stripe[240 * 16];
        for (int y = 0; y < 320; y += 16) {
            gamepad_ui_render(&ui, 1500, stripe, y, 16);
            for (unsigned p = 0; p < 240 * 16; ++p) {
                unsigned v = stripe[p];
                unsigned char rgb[] = {((v >> 11) & 31) * 255 / 31,
                                       ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31};
                fwrite(rgb, 1, 3, file);
            }
        }
        fclose(file);
    }
    return 0;
}
