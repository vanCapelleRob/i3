/*
 * vim:ts=8:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 *
 * © 2009 Michael Stapelberg and contributors
 *
 * See file LICENSE for license information.
 *
 * i3-input/main.c: Utility which lets the user input commands and sends them
 * to i3.
 *
 */
#include <ev.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <stdint.h>
#include <getopt.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_keysyms.h>

#include <X11/keysym.h>

#include "keysym2ucs.h"

#include "i3-input.h"

static int sockfd;
static xcb_key_symbols_t *symbols;
static int modeswitchmask;
static bool modeswitch_active = false;
static xcb_window_t win;
static xcb_pixmap_t pixmap;
static xcb_gcontext_t pixmap_gc;
static char *glyphs_ucs[512];
static char *glyphs_utf8[512];
static int input_position;
static int font_height;


/*
 * Concats the glyphs (either UCS-2 or UTF-8) to a single string, suitable for
 * rendering it (UCS-2) or sending it to i3 (UTF-8).
 *
 */
static uint8_t *concat_strings(char **glyphs, int max) {
        uint8_t *output = calloc(max, 4);
        uint8_t *walk = output;
        for (int c = 0; c < max; c++) {
                printf("at %c\n", glyphs[c][0]);
                /* if the first byte is 0, this has to be UCS2 */
                if (glyphs[c][0] == '\0') {
                        memcpy(walk, glyphs[c], 2);
                        walk += 2;
                } else {
                        strcpy((char*)walk, glyphs[c]);
                        walk += strlen(glyphs[c]);
                }
        }
        printf("output = %s\n", output);
        return output;
}

/*
 * Handles expose events (redraws of the window) and rendering in general. Will
 * be called from the code with event == NULL or from X with event != NULL.
 *
 */
static int handle_expose(void *data, xcb_connection_t *conn, xcb_expose_event_t *event) {
        printf("expose!\n");

        /* re-draw the background */
        xcb_rectangle_t border = {0, 0, 500, font_height + 8}, inner = {2, 2, 496, font_height + 8 - 4};
        xcb_change_gc_single(conn, pixmap_gc, XCB_GC_FOREGROUND, get_colorpixel(conn, "#FF0000"));
        xcb_poly_fill_rectangle(conn, pixmap, pixmap_gc, 1, &border);
        xcb_change_gc_single(conn, pixmap_gc, XCB_GC_FOREGROUND, get_colorpixel(conn, "#000000"));
        xcb_poly_fill_rectangle(conn, pixmap, pixmap_gc, 1, &inner);

        /* restore font color */
        xcb_change_gc_single(conn, pixmap_gc, XCB_GC_FOREGROUND, get_colorpixel(conn, "#FFFFFF"));
        uint8_t *con = concat_strings(glyphs_ucs, input_position);
        xcb_image_text_16(conn, input_position, pixmap, pixmap_gc, 4 /* X */,
                          font_height + 2 /* Y = baseline of font */, (xcb_char2b_t*)con);

        /* Copy the contents of the pixmap to the real window */
        xcb_copy_area(conn, pixmap, win, pixmap_gc, 0, 0, 0, 0, /* */ 500, font_height + 8);
        xcb_flush(conn);
        free(con);

        return 1;
}

/*
 * Deactivates the Mode_switch bit upon release of the Mode_switch key.
 *
 */
static int handle_key_release(void *ignored, xcb_connection_t *conn, xcb_key_release_event_t *event) {
        printf("releasing %d, state raw = %d\n", event->detail, event->state);

        xcb_keysym_t sym = xcb_key_press_lookup_keysym(symbols, event, event->state);
        if (sym == XK_Mode_switch) {
                printf("Mode switch disabled\n");
                modeswitch_active = false;
        }

        return 1;
}

/*
 * Handles keypresses by converting the keycodes to keysymbols, then the
 * keysymbols to UCS-2. If the conversion succeeded, the glyph is saved in the
 * internal buffers and displayed in the input window.
 *
 * Also handles backspace (deleting one character) and return (sending the
 * command to i3).
 *
 */
static int handle_key_press(void *ignored, xcb_connection_t *conn, xcb_key_press_event_t *event) {
        printf("Keypress %d, state raw = %d\n", event->detail, event->state);

        /* fix state */
        if (modeswitch_active)
                event->state |= modeswitchmask;

        xcb_keysym_t sym = xcb_key_press_lookup_keysym(symbols, event, event->state);
        if (sym == XK_Mode_switch) {
                printf("Mode switch enabled\n");
                modeswitch_active = true;
                return 1;
        }

        if (sym == XK_Return) {
                uint8_t *command = concat_strings(glyphs_utf8, input_position);
                printf("command = %s\n", command);

                ipc_send_message(sockfd, strlen((char*)command), 0, command);

#if 0
                free(command);
                return 1;
#endif
                exit(0);
        }

        if (sym == XK_BackSpace) {
                if (input_position == 0)
                        return 1;

                input_position--;
                free(glyphs_ucs[input_position]);
                free(glyphs_utf8[input_position]);

                handle_expose(NULL, conn, NULL);
                return 1;
        }
        if (sym == XK_Escape) {
                exit(0);
        }

        /* TODO: handle all of these? */
        printf("is_keypad_key = %d\n", xcb_is_keypad_key(sym));
        printf("is_private_keypad_key = %d\n", xcb_is_private_keypad_key(sym));
        printf("xcb_is_cursor_key = %d\n", xcb_is_cursor_key(sym));
        printf("xcb_is_pf_key = %d\n", xcb_is_pf_key(sym));
        printf("xcb_is_function_key = %d\n", xcb_is_function_key(sym));
        printf("xcb_is_misc_function_key = %d\n", xcb_is_misc_function_key(sym));
        printf("xcb_is_modifier_key = %d\n", xcb_is_modifier_key(sym));

        if (xcb_is_modifier_key(sym) || xcb_is_cursor_key(sym))
                return 1;

        printf("sym = %c (%d)\n", sym, sym);

        /* convert the keysym to UCS */
        uint16_t ucs = keysym2ucs(sym);
        if ((int16_t)ucs == -1) {
                fprintf(stderr, "Keysym could not be converted to UCS, skipping\n");
                return 1;
        }

        /* store the UCS into a string */
        uint8_t inp[3] = {(ucs & 0xFF00) >> 8, (ucs & 0xFF), 0};

        printf("inp[0] = %02x, inp[1] = %02x, inp[2] = %02x\n", inp[0], inp[1], inp[2]);
        /* convert it to UTF-8 */
        char *out = convert_ucs_to_utf8((char*)inp);
        printf("converted to %s\n", out);

        glyphs_ucs[input_position] = malloc(3 * sizeof(uint8_t));
        if (glyphs_ucs[input_position] == NULL)
                err(EXIT_FAILURE, "malloc() failed\n");
        memcpy(glyphs_ucs[input_position], inp, 3);
        glyphs_utf8[input_position] = strdup(out);
        input_position++;

        handle_expose(NULL, conn, NULL);
        return 1;
}

int main(int argc, char *argv[]) {
        char *socket_path = "/tmp/i3-ipc.sock";
        char *pattern = "-misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1";
        int o, option_index = 0;

        static struct option long_options[] = {
                {"socket", required_argument, 0, 's'},
                {"version", no_argument, 0, 'v'},
                {"help", no_argument, 0, 'h'},
                {0, 0, 0, 0}
        };

        char *options_string = "s:t:vh";

        while ((o = getopt_long(argc, argv, options_string, long_options, &option_index)) != -1) {
                if (o == 's') {
                        socket_path = strdup(optarg);
                        break;
                } else if (o == 'v') {
                        printf("i3-input " I3_VERSION);
                        return 0;
                } else if (o == 'h') {
                        printf("i3-input " I3_VERSION);
                        printf("i3-input [-s <socket>] [-p <prefix>]\n");
                        return 0;
                }
        }

        sockfd = connect_ipc(socket_path);

        int screens;
        xcb_connection_t *conn = xcb_connect(NULL, &screens);
        if (xcb_connection_has_error(conn))
                die("Cannot open display\n");

        /* Set up event handlers for key press and key release */
        xcb_event_handlers_t evenths;
        memset(&evenths, 0, sizeof(xcb_event_handlers_t));
        xcb_event_handlers_init(conn, &evenths);
        xcb_event_set_key_press_handler(&evenths, handle_key_press, NULL);
        xcb_event_set_key_release_handler(&evenths, handle_key_release, NULL);
        xcb_event_set_expose_handler(&evenths, handle_expose, NULL);

        modeswitchmask = get_mode_switch_mask(conn);
	symbols = xcb_key_symbols_alloc(conn);

        uint32_t font_id = get_font_id(conn, pattern, &font_height);

        /* Open an input window */
        win = open_input_window(conn, 500, font_height + 8);

        /* Create pixmap */
        xcb_screen_t *root_screen = xcb_aux_get_screen(conn, screens);

        pixmap = xcb_generate_id(conn);
        pixmap_gc = xcb_generate_id(conn);
        xcb_create_pixmap(conn, root_screen->root_depth, pixmap, win, 500, font_height + 8);
        xcb_create_gc(conn, pixmap_gc, pixmap, 0, 0);

        /* Create graphics context */
        xcb_change_gc_single(conn, pixmap_gc, XCB_GC_FONT, font_id);

        /* Grab the keyboard to get all input */
        xcb_grab_keyboard(conn, false, win, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

        xcb_flush(conn);

        xcb_event_wait_for_event_loop(&evenths);

        return 0;
}
