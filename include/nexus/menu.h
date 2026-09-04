// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- scrolling console menu.
//
// A small list widget: items, a selection, and a window that scrolls to keep
// the selection visible. It knows nothing about what the items mean, so the
// screens in ui.c stay readable.
#pragma once

#include <switch.h>
#include <stdbool.h>

#define MENU_MAX_ITEMS   192
#define MENU_LABEL_LEN   64
#define MENU_DETAIL_LEN  48

typedef struct {
    int  id;                        // caller's identifier for the row
    char label[MENU_LABEL_LEN];
    char detail[MENU_DETAIL_LEN];   // right-aligned status text, may be empty
    bool selectable;                // false for headings and spacers
} MenuItem;

typedef struct {
    MenuItem items[MENU_MAX_ITEMS];
    u32      count;
    u32      selected;
    u32      scroll;      // index of the first visible row
    u32      visible;     // rows the window can show
} Menu;

/// Resets the menu and sets how many rows fit on screen.
void menuReset(Menu *m, u32 visible_rows);

/// Appends a selectable row. Returns false when the menu is full.
bool menuAdd(Menu *m, int id, const char *label, const char *detail);

/// Appends a non-selectable heading or blank spacer.
bool menuAddHeading(Menu *m, const char *label);
bool menuAddSpacer(Menu *m);

/// Moves the selection, skipping non-selectable rows and wrapping at the ends.
void menuMove(Menu *m, int delta);

/// Jumps a whole page.
void menuPage(Menu *m, int direction);

/// Id of the selected row, or -1 when nothing selectable exists.
int menuSelectedId(const Menu *m);

/// Draws the visible window, plus a scroll indicator when the list overflows.
void menuDraw(const Menu *m);
