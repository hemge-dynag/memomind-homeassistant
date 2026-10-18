#include "gm_plugin_lvgl_api.h"
#include "gm_plugin_libc.h"
#include "ha_icons.h"

/* Paired with PhoneSDK/examples/homeassistant. The phone connects to a Home
 * Assistant instance, fetches entities grouped into dashboard pages, and pushes
 * every page to this plugin. The wearer drives it hands-free: turning the head
 * left or right flips between pages, while raising or lowering the head moves a
 * selection cursor up or down the rows (the view scrolls to follow it when a
 * page holds more rows than fit on screen). Pressing the primary button runs the
 * action of the selected row: the glasses send the row's entity id back to the
 * phone, which calls the matching Home Assistant service (toggle a light, a
 * switch, run a scene...). Each page shows a title and up to MAX_ROWS_PER_PAGE
 * label/value rows (sensor readings, light state, ...). The glasses never talk
 * to Home Assistant directly: all network work stays on the phone.
 */

#define PAGES_CHANNEL UINT16_C(0x4841)  /* 'HA': phone -> glasses */
#define EVENT_CHANNEL UINT16_C(0x4853)  /* 'HS': glasses -> phone */
#define PROTOCOL_VERSION 2U

#define COMMAND_SET_PAGES 1U
#define COMMAND_CLEAR 2U

#define EVENT_PAGE_CHANGED UINT8_C(1)
#define EVENT_ACTION UINT8_C(2)

#define MAX_PAGES 24U
/* Rows buffered per page. The visible window is smaller; the rest is reachable
 * by scrolling with head up/down. */
#define MAX_ROWS_PER_PAGE 32U
/* Upper bound on rows drawn at once; the actual count (ha.visible_rows) is
 * derived from the panel height at start and never exceeds this. */
#define MAX_VISIBLE_ROWS 8U
/* Per row: label <= 32 bytes, value <= 24 bytes, action <= 48 bytes (the entity
 * id echoed back to the phone when the row is activated). Per page title <= 32
 * bytes. */
#define MAX_LABEL_BYTES 32U
#define MAX_VALUE_BYTES 24U
#define MAX_TITLE_BYTES 32U
#define MAX_ACTION_BYTES 48U
/* No icon: the row is drawn without a glyph. Any other value is an index into
 * ha_icons[] (the phone sends the same order, see PhoneSDK icons.js). */
#define NO_ICON 0xFFU
/* Generic glyph used when a row has no usable icon id (text-box). */
#define HA_ICON_FALLBACK (HA_ICON_COUNT - 1U)

/* version(1) + command(1) + page_count(2) */
#define MIN_SET_PAGES_PAYLOAD 4U

#define SCREEN_MARGIN 16
/* The value column is right-aligned; keep it clear of the right edge with a
 * slightly wider margin than the left so the reading never hugs the border. */
#define RIGHT_MARGIN 30
#define HEADER_HEIGHT 44
#define HINT_HEIGHT 28
#define ROW_MIN_HEIGHT 26
/* Smallest legible box for a 16x16 glyph. The icon column is only carved out
 * when a row is tall enough for it; otherwise rows fall back to text-only. */
#define ICON_CELL 16
/* Horizontal gap between the icon and the label text. */
#define ICON_GAP 8

typedef struct {
    char label[MAX_LABEL_BYTES + 1U];
    char value[MAX_VALUE_BYTES + 1U];
    char action[MAX_ACTION_BYTES + 1U];
    uint8_t icon; /* index into ha_icons[], or NO_ICON */
} ha_row_t;

typedef struct {
    char title[MAX_TITLE_BYTES + 1U];
    ha_row_t rows[MAX_ROWS_PER_PAGE];
    uint8_t row_count;
} ha_page_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;
    gm_plugin_lvgl_obj_t *screen;
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *row_bg[MAX_VISIBLE_ROWS]; /* selection highlight */
    gm_plugin_lvgl_obj_t *row_icon[MAX_VISIBLE_ROWS]; /* 16x16 glyph, or 0 */
    gm_plugin_lvgl_obj_t *row_label[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *row_value[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *hint_label;
    uint16_t page_count;
    uint16_t page_index;
    uint16_t scroll_offset; /* first visible row of the current page */
    uint16_t selected;      /* selected row index within the page */
    uint8_t visible_rows;   /* rows that fit on screen */
    int32_t row_height;
    int32_t label_width;    /* label column width (left-aligned) */
    int32_t label_x;        /* left edge of the label text */
    uint8_t icon_column;    /* 1 when rows reserve a 16x16 icon slot */
    int32_t value_width;    /* value column width (right-aligned) */
    uint8_t language; /* 0=fr 1=en 2=es 3=it 4=de 5=zh-CN */
} ha_t;

/* The page payloads live in their own array (not inside ha_t): keeping the
 * hot scalar state in a small struct lets the compiler address it with compact
 * offsets, and keeps every cross-segment address materialization inside the
 * plugin's XIP image. */
static ha_page_t ha_pages[MAX_PAGES];

/* Picks the string for the detected UI locale. */
#define L(fr_str, en_str, es_str, it_str, de_str, zh_str) \
    (ha.language == 1U ? (en_str) : ha.language == 2U ? (es_str) : \
     ha.language == 3U ? (it_str) : ha.language == 4U ? (de_str) : \
     ha.language == 5U ? (zh_str) : (fr_str))

static ha_t ha;

static uint8_t ha_detect_language(const char *locale)
{
    if (locale[0] == 'e' && locale[1] == 'n') return 1U;
    if (locale[0] == 'e' && locale[1] == 's') return 2U;
    if (locale[0] == 'i' && locale[1] == 't') return 3U;
    if (locale[0] == 'd' && locale[1] == 'e') return 4U;
    if (locale[0] == 'z' && locale[1] == 'h') return 5U;
    return 0U;
}

#define number gm_plugin_lvgl_style_number
#define color gm_plugin_lvgl_style_color

static void set_style(gm_plugin_lvgl_obj_t *object,
                       gm_plugin_lvgl_style_prop_t property,
                       gm_plugin_lvgl_style_value_t value)
{
    ha.ui->style_set(object, property, value, GM_PLUGIN_LVGL_SELECTOR_MAIN);
}

static void style_panel(gm_plugin_lvgl_obj_t *object, uint8_t fill,
                         uint8_t border, uint8_t radius)
{
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(fill));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BG_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_COLOR, color(0xA0));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(255));
    set_style(object, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(border));
    set_style(object, GM_PLUGIN_LVGL_STYLE_RADIUS, number(radius));
    ha.ui->obj_clear_flag(object, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
}

static gm_plugin_lvgl_obj_t *make_label(gm_plugin_lvgl_obj_t *parent,
                                         const char *text, uint8_t shade,
                                         bool align_left)
{
    gm_plugin_lvgl_obj_t *label = ha.ui->label_create(parent);
    if (label == 0) return 0;
    ha.ui->label_set_text(label, text);
    ha.ui->label_set_long_mode(label, GM_PLUGIN_LVGL_LABEL_DOT);
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_COLOR, color(shade));
    set_style(label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(align_left ? GM_PLUGIN_LVGL_TEXT_ALIGN_LEFT
                                : GM_PLUGIN_LVGL_TEXT_ALIGN_RIGHT));
    return label;
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static void hide_row_icon(uint32_t slot);

/* Maps a wire icon id to its descriptor, or 0 when the row carries no icon or
 * an out-of-range id (e.g. a newer phone sent an icon this build lacks). */
static const gm_plugin_lvgl_image_dsc_t *row_icon_dsc(uint8_t icon)
{
    /* An unknown id (including NO_ICON) falls back to the generic glyph instead
     * of leaving the slot empty: a row always shows an icon, so an older phone
     * that still sends 0xFF for unrecognised icons stays readable. */
    if (icon >= HA_ICON_COUNT) icon = HA_ICON_FALLBACK;
    return &ha_icons_dsc[icon];
}

/* Points a row's image object at the glyph for `icon`, hiding it when the row
 * has none. The descriptor data is the table's static bitmap, borrowed by the
 * host for as long as the image shows it. */
static void show_row_icon(uint32_t slot, uint8_t icon)
{
    const gm_plugin_lvgl_image_dsc_t *dsc;
    gm_plugin_result_t result;
    if (slot >= ha.visible_rows || ha.row_icon[slot] == 0) return;
    dsc = row_icon_dsc(icon);
    if (dsc == 0) {
        hide_row_icon(slot);
        return;
    }
    result = ha.ui->image_set_source(ha.row_icon[slot], dsc);
    if (result != GM_PLUGIN_OK) {
        ha.host->log("homeassistant: icon set_source slot=%u id=%u rc=%d",
                      (unsigned int)slot, (unsigned int)icon, (int)result);
        return;
    }
    ha.ui->obj_clear_flag(ha.row_icon[slot], GM_PLUGIN_LVGL_FLAG_HIDDEN);
    /* Geometry is taken from the descriptor's bitmap, but some Host build
     * leaves an image created without an explicit size at zero extent, which
     * draws nothing. Sizing it here matches the glyph and is harmless when the
     * Host already derived it. */
    ha.ui->obj_set_size(ha.row_icon[slot], dsc->width, dsc->height);
    ha.ui->obj_invalidate(ha.row_icon[slot]);
}

/* Empties the icon slot so a text-only row leaves no stale glyph behind. */
static void hide_row_icon(uint32_t slot)
{
    if (slot >= ha.visible_rows || ha.row_icon[slot] == 0) return;
    ha.ui->obj_add_flag(ha.row_icon[slot], GM_PLUGIN_LVGL_FLAG_HIDDEN);
}

/* Renders the current page: title with an "i/n" position marker, the visible
 * slice of label/value rows starting at scroll_offset, and a navigation hint.
 * Rows past the page's row_count are blanked so a shorter page never shows
 * stale entries. */
static void refresh_page(void)
{
    char title[64];
    uint32_t slot;

    if (ha.page_count == 0U) {
        ha.ui->label_set_text(ha.title_label,
            L("Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant"));
        for (slot = 0U; slot < ha.visible_rows; slot++) {
            ha.ui->label_set_text(ha.row_label[slot], "");
            ha.ui->label_set_text(ha.row_value[slot], "");
            if (ha.row_icon[slot] != 0) hide_row_icon(slot);
            set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
        }
        ha.ui->label_set_text(ha.hint_label,
            L("En attente du telephone...", "Waiting for the phone...", "Esperando al teléfono...", "In attesa del telefono...", "Warte auf das Telefon...", "正在等待手机..."));
        return;
    }

    {
        const ha_page_t *page = &ha_pages[ha.page_index];
        uint16_t total_rows = page->row_count;
        uint16_t max_offset;
        uint32_t pos = (uint32_t)(ha.page_index + 1U);
        uint32_t total = (uint32_t)ha.page_count;
        int written = 0;
        const char *src = page->title;

        /* Keep the window inside the page after a refresh that shortened it. */
        max_offset = (total_rows > ha.visible_rows)
            ? (uint16_t)(total_rows - ha.visible_rows) : 0U;
        if (ha.scroll_offset > max_offset) ha.scroll_offset = max_offset;

        /* Title then "i/n" appended manually to avoid snprintf dependency. */
        while (*src != '\0' && written < 48) title[written++] = *src++;
        title[written++] = ' ';
        title[written++] = ' ';
        if (pos >= 10U) title[written++] = (char)('0' + (pos / 10U) % 10U);
        title[written++] = (char)('0' + pos % 10U);
        title[written++] = '/';
        if (total >= 10U) title[written++] = (char)('0' + (total / 10U) % 10U);
        title[written++] = (char)('0' + total % 10U);
        /* Scroll position marker when the page overflows the screen. */
        if (total_rows > ha.visible_rows) {
            uint16_t last = (uint16_t)(ha.scroll_offset + ha.visible_rows);
            title[written++] = ' ';
            if (last >= 10U) title[written++] = (char)('0' + (last / 10U) % 10U);
            title[written++] = (char)('0' + last % 10U);
            title[written++] = '/';
            if (total_rows >= 10U) title[written++] = (char)('0' + (total_rows / 10U) % 10U);
            title[written++] = (char)('0' + total_rows % 10U);
        }
        title[written] = '\0';
        ha.ui->label_set_text(ha.title_label, title);

        for (slot = 0U; slot < ha.visible_rows; slot++) {
            uint32_t row = (uint32_t)ha.scroll_offset + slot;
            bool is_selected = (row < page->row_count) && ((uint16_t)row == ha.selected);
            if (row < page->row_count) {
                ha.ui->label_set_text(ha.row_label[slot], page->rows[row].label);
                ha.ui->label_set_text(ha.row_value[slot], page->rows[row].value);
                show_row_icon(slot, page->rows[row].icon);
            } else {
                ha.ui->label_set_text(ha.row_label[slot], "");
                ha.ui->label_set_text(ha.row_value[slot], "");
                if (ha.row_icon[slot] != 0) hide_row_icon(slot);
            }
            /* Highlight the selected row; the others keep a transparent
             * background so the highlight follows the cursor. */
            set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA,
                      number(is_selected ? 255 : 0));
        }

        /* Navigation hints were removed at the wearer's request: the head
         * gestures are discoverable on their own and the text crowded the
         * rows. The label is kept (empty) so the layout stays unchanged. */
        ha.ui->label_set_text(ha.hint_label, "");
    }
}

/* Keeps the selected row inside the visible window, scrolling the page as
 * needed so the cursor can never leave the screen. */
static void ensure_selection_visible(void)
{
    if (ha.selected < ha.scroll_offset) {
        ha.scroll_offset = ha.selected;
    } else if ((uint32_t)ha.selected >=
               (uint32_t)ha.scroll_offset + ha.visible_rows) {
        ha.scroll_offset = (uint16_t)(ha.selected - ha.visible_rows + 1U);
    }
}

/* Moves the selection cursor by delta rows, clamped to the current page, and
 * scrolls so the cursor stays visible. No-op when the gesture would not move. */
static void move_selection(int32_t delta)
{
    const ha_page_t *page;
    int32_t next;

    if (ha.page_count == 0U) return;
    page = &ha_pages[ha.page_index];
    if (page->row_count == 0U) return;

    next = (int32_t)ha.selected + delta;
    if (next < 0) next = 0;
    if (next >= (int32_t)page->row_count) next = (int32_t)page->row_count - 1;
    if ((uint16_t)next == ha.selected) return;
    ha.selected = (uint16_t)next;
    ensure_selection_visible();
    refresh_page();
}

/* Sends the selected row's action (entity id) to the phone so it can run the
 * matching Home Assistant service. Ignored when the row carries no action. */
static void run_action(void)
{
    const ha_page_t *page;
    const char *action;
    uint8_t payload[3U + MAX_ACTION_BYTES];
    uint8_t len = 0U;

    if (ha.page_count == 0U) return;
    page = &ha_pages[ha.page_index];
    if (ha.selected >= page->row_count) return;

    action = page->rows[ha.selected].action;
    while (action[len] != '\0' && len < MAX_ACTION_BYTES) {
        payload[3U + len] = (uint8_t)action[len];
        len++;
    }
    if (len == 0U) return;

    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_ACTION;
    payload[2] = len;
    ha.host->log("homeassistant: action row=%u len=%u",
                  (unsigned int)ha.selected, (unsigned int)len);
    (void)ha.host->bt_send(EVENT_CHANNEL, payload, 3U + len);
}

/* Loops through pages in both directions so navigation never sticks at the
 * ends. Only fires when there is more than one page. Resets the selection to
 * the first row of the newly shown page. */
static void change_page(int32_t delta)
{
    uint8_t payload[3];
    if (ha.page_count == 0U) return;
    {
        int32_t wrapped = ((int32_t)ha.page_index + delta) % (int32_t)ha.page_count;
        if (wrapped < 0) wrapped += (int32_t)ha.page_count;
        ha.page_index = (uint16_t)wrapped;
    }
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    refresh_page();
    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_PAGE_CHANGED;
    payload[2] = (uint8_t)(ha.page_index & 0xFFU);
    (void)ha.host->bt_send(EVENT_CHANNEL, payload, sizeof(payload));
}

static gm_plugin_result_t homeassistant_start(void *context)
{
    gm_plugin_display_info_t display;
    gm_plugin_lvgl_obj_t *root;
    char locale[GM_PLUGIN_LOCALE_TAG_MAX];
    int32_t rows_area_height;
    uint32_t row;

    (void)context;

    ha.language = 0U;
    if (ha.host->locale_get != 0 &&
        ha.host->locale_get(locale) == GM_PLUGIN_OK)
        ha.language = ha_detect_language(locale);

    if (ha.host->display_get_info(&display) != GM_PLUGIN_OK ||
        display.width <= SCREEN_MARGIN * 2U)
        return GM_PLUGIN_ESTATE;

    rows_area_height = (int32_t)display.height - HEADER_HEIGHT - HINT_HEIGHT
                       - SCREEN_MARGIN;
    /* Fit as many rows as the panel allows, capped by MAX_VISIBLE_ROWS. Rows
     * beyond that stay in the page buffer and are reached by scrolling. Each
     * drawn row is sized from this area so the window always fits between the
     * header and the hint instead of overrunning the hint. */
    ha.visible_rows = (uint8_t)(rows_area_height / ROW_MIN_HEIGHT);
    if (ha.visible_rows > MAX_VISIBLE_ROWS) ha.visible_rows = MAX_VISIBLE_ROWS;
    if (ha.visible_rows == 0U) return GM_PLUGIN_ESTATE;
    ha.row_height = rows_area_height / (int32_t)ha.visible_rows;
    if (ha.row_height < ROW_MIN_HEIGHT)
        return GM_PLUGIN_ESTATE;
    /* Rows fill rows_area_height exactly; anchor them just under the header. */
    rows_area_height = (int32_t)ha.visible_rows * ha.row_height;

    ha.host->log("homeassistant: display %ux%u visible_rows=%u row_height=%d area=%d",
                  (unsigned int)display.width, (unsigned int)display.height,
                  (unsigned int)ha.visible_rows, (int)ha.row_height,
                  (int)rows_area_height);

    /* Split the usable width into a label column and a value column with a
     * fixed gap between them. The value gets a fixed share so long numeric
     * readings stay whole; the label takes the rest and truncates. The value
     * column stops short of the right edge (RIGHT_MARGIN) so it never hugs the
     * border. */
    {
        int32_t usable = (int32_t)display.width - SCREEN_MARGIN - RIGHT_MARGIN;
        int32_t gap = usable / 12;
        ha.value_width = usable * 2 / 5;
        ha.label_width = usable - ha.value_width - gap;
        if (ha.label_width < 0) ha.label_width = 0;

        /* ICON_CELL takes the first 16 px of the label column when a row is
         * tall enough to show a 16x16 glyph; the label then starts after the
         * glyph plus a small gap. Shorter rows keep the full width and draw
         * text only (no image objects are created for them). */
        ha.label_x = SCREEN_MARGIN;
        ha.icon_column = 0;
        if (ha.row_height - 2 >= ICON_CELL && ha.label_width > ICON_CELL + ICON_GAP + 16) {
            ha.icon_column = 1;
            ha.label_x = SCREEN_MARGIN + ICON_CELL + ICON_GAP;
            ha.label_width -= ICON_CELL + ICON_GAP;
        }
        ha.host->log("homeassistant: icons column=%u label_x=%d label_w=%d value_w=%d",
                      (unsigned int)ha.icon_column, (int)ha.label_x,
                      (int)ha.label_width, (int)ha.value_width);    }

    root = ha.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;
    ha.ui->obj_clean(root);

    ha.screen = ha.ui->obj_create(root);
    if (ha.screen == 0) return GM_PLUGIN_ENOMEM;
    ha.ui->obj_set_size(ha.screen, display.width, display.height);
    ha.ui->obj_align(ha.screen, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    style_panel(ha.screen, 0x00, 0, 0);

    ha.title_label = make_label(ha.screen, "", 0xF0, false);
    if (ha.title_label == 0) goto no_memory;
    {
        gm_plugin_lvgl_style_value_t font_value = {0};
        font_value.ptr = ha.ui->font_large;
        set_style(ha.title_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, font_value);
    }
    set_style(ha.title_label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    ha.ui->obj_set_size(ha.title_label, display.width - SCREEN_MARGIN * 2U,
                         HEADER_HEIGHT);
    ha.ui->obj_align(ha.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 6);

    for (row = 0U; row < ha.visible_rows; row++) {
        /* Absolute placement via obj_set_pos (top-left in logical pixels) keeps
         * the rows inside the area below the header without depending on the
         * alignment point's origin. Two fixed columns: label hugs the left
         * margin, value stops before the right margin. Sizing both from a single
         * split point guarantees a long label can never push the value off the
         * screen; the label clips with "..." via LABEL_DOT. A background
         * rectangle sits behind each row and is made opaque only for the
         * selected row, drawing the selection cursor. */
        int32_t top_y = HEADER_HEIGHT + SCREEN_MARGIN / 2
            + (int32_t)row * ha.row_height;

        /* The highlight rectangle is created first, then the glyph: LVGL draws
         * children in creation order, so a later child sits above an earlier
         * one and the glyph stays visible over the selected-row highlight. */
        ha.row_bg[row] = ha.ui->obj_create(ha.screen);
        if (ha.row_bg[row] == 0) goto no_memory;
        ha.ui->obj_set_size(ha.row_bg[row],
                             (int32_t)display.width - SCREEN_MARGIN - RIGHT_MARGIN,
                             ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_bg[row], SCREEN_MARGIN, top_y);
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(0x38));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_RADIUS, number(6));
        /* No outline around the highlight: only a flat filled bar. */
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(0));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(0));
        ha.ui->obj_clear_flag(ha.row_bg[row], GM_PLUGIN_LVGL_FLAG_SCROLLABLE);

        if (ha.icon_column) {
            ha.row_icon[row] = ha.ui->image_create(ha.screen, &ha_icons_dsc[0]);
            if (ha.row_icon[row] == 0) goto no_memory;
            ha.ui->obj_set_size(ha.row_icon[row], ICON_CELL, ICON_CELL);
            ha.ui->obj_set_pos(ha.row_icon[row], SCREEN_MARGIN,
                               top_y + ((ha.row_height - 2) - ICON_CELL) / 2);
            ha.ui->obj_add_flag(ha.row_icon[row], GM_PLUGIN_LVGL_FLAG_HIDDEN);
            ha.ui->obj_clear_flag(ha.row_icon[row], GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
        }

        ha.row_label[row] = make_label(ha.screen, "", 0xD0, true);
        if (ha.row_label[row] == 0) goto no_memory;
        ha.ui->obj_set_size(ha.row_label[row], ha.label_width,
                             ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_label[row], ha.label_x, top_y);

        ha.row_value[row] = make_label(ha.screen, "", 0xF0, false);
        if (ha.row_value[row] == 0) goto no_memory;
        ha.ui->obj_set_size(ha.row_value[row], ha.value_width,
                             ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_value[row],
                           (int32_t)display.width - RIGHT_MARGIN - ha.value_width,
                           top_y);
    }

    ha.hint_label = make_label(ha.screen, "", 0x70, false);
    if (ha.hint_label == 0) goto no_memory;
    ha.ui->obj_set_size(ha.hint_label, display.width - SCREEN_MARGIN * 2U,
                         HINT_HEIGHT);
    ha.ui->obj_align(ha.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, 0);

    ha.page_count = 0U;
    ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) ha.row_icon[row] = 0;
    refresh_page();

    if (ha.host->imu_enable(GM_PLUGIN_IMU_ENABLE_GESTURES) != GM_PLUGIN_OK)
        goto no_memory;
    return GM_PLUGIN_OK;

no_memory:
    ha.ui->obj_clean(root);
    ha.screen = 0;
    ha.title_label = 0;
    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) {
        ha.row_bg[row] = 0;
        ha.row_icon[row] = 0;
        ha.row_label[row] = 0;
        ha.row_value[row] = 0;
    }
    ha.hint_label = 0;
    return GM_PLUGIN_ENOMEM;
}

/* Parses a full page set pushed by the phone:
 * [version, cmd, page_count(2),
 *   { title_len, title,
 *     row_count(2),
 *     { icon_id, label_len, label, value_len, value, action_len, action }
 *       * row_count } * page_count ]
 * icon_id indexes ha_icons[], or NO_ICON. */
static void handle_set_pages(const uint8_t *data, uint32_t length)
{
    uint32_t offset;
    uint16_t requested_pages;
    uint16_t p;

    if (length < MIN_SET_PAGES_PAYLOAD || data[0] != PROTOCOL_VERSION) return;

    if (data[1] == COMMAND_CLEAR) {
        ha.page_count = 0U;
        ha.page_index = 0U;
        ha.scroll_offset = 0U;
        ha.selected = 0U;
        refresh_page();
        return;
    }
    if (data[1] != COMMAND_SET_PAGES) return;

    requested_pages = read_u16(&data[2]);
    if (requested_pages > MAX_PAGES) requested_pages = MAX_PAGES;

    offset = 4U;
    for (p = 0U; p < requested_pages; p++) {
        ha_page_t *page = &ha_pages[p];
        uint8_t title_len;
        uint16_t requested_rows;
        uint16_t r;

        if (offset + 1U > length) break;
        title_len = data[offset++];
        if (title_len > MAX_TITLE_BYTES || offset + title_len > length) break;
        ha.libc->memcpy(page->title, &data[offset], title_len);
        page->title[title_len] = '\0';
        offset += title_len;

        if (offset + 2U > length) break;
        requested_rows = read_u16(&data[offset]);
        offset += 2U;
        if (requested_rows > MAX_ROWS_PER_PAGE) requested_rows = MAX_ROWS_PER_PAGE;

        for (r = 0U; r < requested_rows; r++) {
            uint8_t label_len;
            uint8_t value_len;
            uint8_t action_len;
            if (offset + 1U > length) break;
            page->rows[r].icon = data[offset++];
            if (offset + 1U > length) break;
            label_len = data[offset++];
            if (label_len > MAX_LABEL_BYTES || offset + label_len > length) break;
            ha.libc->memcpy(page->rows[r].label, &data[offset], label_len);
            page->rows[r].label[label_len] = '\0';
            offset += label_len;

            if (offset + 1U > length) break;
            value_len = data[offset++];
            if (value_len > MAX_VALUE_BYTES || offset + value_len > length) break;
            ha.libc->memcpy(page->rows[r].value, &data[offset], value_len);
            page->rows[r].value[value_len] = '\0';
            offset += value_len;

            if (offset + 1U > length) break;
            action_len = data[offset++];
            if (action_len > MAX_ACTION_BYTES || offset + action_len > length) break;
            ha.libc->memcpy(page->rows[r].action, &data[offset], action_len);
            page->rows[r].action[action_len] = '\0';
            offset += action_len;
        }
        page->row_count = (uint8_t)r;
    }
    ha.page_count = p;
    if (ha.page_index >= ha.page_count) ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    ha.host->log("homeassistant: set_pages count=%u page0_rows=%u visible=%u",
                  (unsigned int)ha.page_count,
                  (unsigned int)(ha.page_count ? ha_pages[0].row_count : 0U),
                  (unsigned int)ha.visible_rows);
    /* First icons of page 0: distinguishes "phone sent no icon" (ff) from a
     * rendering problem with a real id. */
    for (p = 0U; p < 4U && p < ha_pages[0].row_count; p++)
        ha.host->log("homeassistant: row%u icon=%u image=%p",
                      (unsigned int)p, (unsigned int)ha_pages[0].rows[p].icon,
                      (void *)ha.row_icon[p]);
    refresh_page();
}

static bool homeassistant_event(void *context, const gm_plugin_event_t *event)
{
    (void)context;
    if (event == 0) return false;

    if (event->type == GM_PLUGIN_EVENT_CONNECTION) {
        ha.host->log("homeassistant: phone connected=%u",
                      (unsigned int)event->data.connection.connected);
        return true;
    }

    if (event->type == GM_PLUGIN_EVENT_BT_MESSAGE) {
        if (event->data.bt.channel != PAGES_CHANNEL || event->data.bt.data == 0)
            return false;
        handle_set_pages(event->data.bt.data, event->data.bt.length);
        return true;
    }

    if (ha.page_count == 0U) return false;

    if (event->type == GM_PLUGIN_EVENT_BUTTON &&
        event->data.button.button == GM_PLUGIN_BUTTON_PRIMARY &&
        event->data.button.action == GM_PLUGIN_BUTTON_ACTION_SINGLE) {
        run_action();
        return true;
    }

    if (event->type == GM_PLUGIN_EVENT_IMU_GESTURE &&
        event->data.imu_gesture.active) {
        ha.host->log("homeassistant: gesture=%u active=%u page=%u rows=%u visible=%u scroll=%u sel=%u",
                      (unsigned int)event->data.imu_gesture.gesture,
                      (unsigned int)event->data.imu_gesture.active,
                      (unsigned int)ha.page_index,
                      (unsigned int)(ha.page_count ? ha_pages[ha.page_index].row_count : 0U),
                      (unsigned int)ha.visible_rows,
                      (unsigned int)ha.scroll_offset,
                      (unsigned int)ha.selected);
        switch (event->data.imu_gesture.gesture) {
        case GM_PLUGIN_IMU_GESTURE_RIGHT:
            change_page(1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_LEFT:
            change_page(-1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_HEAD_RAISE:
            move_selection(-1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_HEAD_LOWER:
            move_selection(1);
            return true;
        default:
            break;
        }
    }
    return false;
}

static void homeassistant_stop(void *context)
{
    gm_plugin_lvgl_obj_t *root;
    uint32_t row;
    (void)context;
    root = ha.ui->root_get();
    (void)ha.host->imu_enable(GM_PLUGIN_IMU_ENABLE_NONE);
    if (root != 0) ha.ui->obj_clean(root);
    ha.screen = 0;
    ha.title_label = 0;
    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) {
        ha.row_bg[row] = 0;
        ha.row_icon[row] = 0;
        ha.row_label[row] = 0;
        ha.row_value[row] = 0;
    }
    ha.hint_label = 0;
    ha.icon_column = 0U;
    ha.page_count = 0U;
    ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    ha.visible_rows = 0U;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                    gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required =
        GM_PLUGIN_CAP_BLUETOOTH | GM_PLUGIN_CAP_BUTTON | GM_PLUGIN_CAP_IMU_EVENTS;

    if (host == 0 || plugin == 0 || host->log == 0 || host->bt_send == 0 ||
        host->display_get_info == 0 || host->graphics.lvgl == 0 ||
        host->imu_enable == 0 ||
        !GM_PLUGIN_VERSION_COMPATIBLE(host->abi_version,
                                      GM_PLUGIN_ABI_MIN_VERSION) ||
        host->struct_size < GM_PLUGIN_HOST_API_MIN_SIZE ||
        plugin->struct_size < GM_PLUGIN_DESCRIPTOR_MIN_SIZE ||
        (host->capabilities & required) != required)
        return GM_PLUGIN_ENOTSUP;

    ha.host = host;
    ha.ui = host->graphics.lvgl;
    if (gm_plugin_libc_get(host, &ha.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;
    /* The row glyphs are static indexed images, so require the 1.1 image
     * entry points to be present instead of only the older table size. */
    if (ha.ui->struct_size < GM_PLUGIN_LVGL_API_1_1_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(ha.ui->api_version,
                                      GM_PLUGIN_VERSION(1U, 1U)) ||
        ha.ui->image_create == 0 || ha.ui->image_set_source == 0)
        return GM_PLUGIN_EVERSION;

    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &ha;
    plugin->on_start = homeassistant_start;
    plugin->on_event = homeassistant_event;
    plugin->on_stop = homeassistant_stop;
    return GM_PLUGIN_OK;
}
