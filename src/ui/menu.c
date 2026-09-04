// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- scrolling console menu.

#include <stdio.h>
#include <string.h>

#include "nexus/menu.h"

// Matches the layout constants in ui.c.
#define MENU_ROW_WIDTH 75

#define C_RESET "\x1b[0m"
#define C_SEL   "\x1b[30;46m"   // black on cyan, for the selected row
#define C_DIM   "\x1b[37m"
#define C_HEAD  "\x1b[36m"
#define C_VALUE "\x1b[97m"

void menuReset(Menu *m, u32 visible_rows)
{
    memset(m, 0, sizeof(*m));
    m->visible = visible_rows > 0 ? visible_rows : 1;
}

static bool menu_push(Menu *m, int id, const char *label, const char *detail,
                      bool selectable)
{
    if (m->count >= MENU_MAX_ITEMS) return false;

    MenuItem *it = &m->items[m->count++];
    it->id         = id;
    it->selectable = selectable;
    snprintf(it->label,  sizeof(it->label),  "%s", label  != NULL ? label  : "");
    snprintf(it->detail, sizeof(it->detail), "%s", detail != NULL ? detail : "");

    // The first selectable row becomes the default selection.
    if (selectable && !m->items[m->selected].selectable) m->selected = m->count - 1;
    return true;
}

bool menuAdd(Menu *m, int id, const char *label, const char *detail)
{
    return menu_push(m, id, label, detail, true);
}

bool menuAddHeading(Menu *m, const char *label)
{
    return menu_push(m, -1, label, NULL, false);
}

bool menuAddSpacer(Menu *m)
{
    return menu_push(m, -1, "", NULL, false);
}

// Keeps the selection inside the visible window.
static void menu_scroll_to_selection(Menu *m)
{
    if (m->selected < m->scroll) {
        m->scroll = m->selected;
    } else if (m->selected >= m->scroll + m->visible) {
        m->scroll = m->selected - m->visible + 1;
    }

    // Never scroll past the end when the list shrinks.
    if (m->count > m->visible && m->scroll > m->count - m->visible) {
        m->scroll = m->count - m->visible;
    }
    if (m->count <= m->visible) m->scroll = 0;
}

void menuMove(Menu *m, int delta)
{
    if (m->count == 0 || delta == 0) return;

    const int step = (delta > 0) ? 1 : -1;
    int idx = (int)m->selected;

    // Walk in the requested direction until a selectable row turns up, giving
    // up after a full lap so a menu of only headings cannot spin forever.
    for (u32 tries = 0; tries < m->count; tries++) {
        idx += step;
        if (idx < 0)                 idx = (int)m->count - 1;
        if (idx >= (int)m->count)    idx = 0;

        if (m->items[idx].selectable) {
            m->selected = (u32)idx;
            menu_scroll_to_selection(m);
            return;
        }
    }
}

void menuPage(Menu *m, int direction)
{
    for (u32 i = 0; i < m->visible; i++) menuMove(m, direction);
}

int menuSelectedId(const Menu *m)
{
    if (m->count == 0) return -1;
    if (!m->items[m->selected].selectable) return -1;
    return m->items[m->selected].id;
}

void menuDraw(const Menu *m)
{
    const u32 end = (m->scroll + m->visible < m->count) ? m->scroll + m->visible : m->count;

    for (u32 i = m->scroll; i < end; i++) {
        const MenuItem *it = &m->items[i];

        if (!it->selectable) {
            if (it->label[0] == '\0') printf("\n");
            else printf("   " C_HEAD "%s" C_RESET "\n", it->label);
            continue;
        }

        const bool sel = (i == m->selected);

        // Pad the label so the detail column lines up and the selection
        // highlight covers the full row width.
        char row[MENU_ROW_WIDTH + 1];
        snprintf(row, sizeof(row), " %s %-*s %14s ",
                 sel ? ">" : " ",
                 MENU_ROW_WIDTH - 20, it->label,
                 it->detail);

        if (sel) printf(C_SEL "%s" C_RESET "\n", row);
        else     printf(C_VALUE "%s" C_RESET "\n", row);
    }

    // Show where we are when the list does not fit.
    if (m->count > m->visible) {
        printf("   " C_DIM "-- %u/%u --" C_RESET "\n", m->selected + 1, m->count);
    }
}
