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
#define EVENT_REQUEST_CARD UINT8_C(3)

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
/* Number of glyphs in the icon table (must match the phone's ICON_NAMES,
 * see PhoneSDK/examples/homeassistant/icons.js). */
#define HA_ICON_COUNT 40U
/* Glyph used when a row has no usable icon id (text-box). */
#define HA_ICON_FALLBACK (HA_ICON_COUNT - 1U)
/* Width of the icon column, including the gap that separates the 16 px bitmap
 * from the entity name. */
#define ICON_CELL 34

/* version(1) + command(1) + page_count(2) */
#define MIN_SET_PAGES_PAYLOAD 4U

#define SCREEN_MARGIN 16
/* The value column is right-aligned; keep it clear of the right edge with a
 * slightly wider margin than the left so the reading never hugs the border. */
#define RIGHT_MARGIN 30
#define HEADER_HEIGHT 44
#define HINT_HEIGHT 28
#define ROW_MIN_HEIGHT 26

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
    /* A card the phone cannot express as rows (gauge, markdown, thermostat...):
     * the page owns the screen and shows either its picture or a hint, and a
     * head nod asks the phone to (re)send that picture. */
    bool image;
} ha_page_t;

typedef struct {
    const gm_plugin_host_api_t *host;
    const gm_plugin_lvgl_api_t *ui;
    const gm_plugin_libc_extension_api_t *libc;
    gm_plugin_lvgl_obj_t *screen;
    gm_plugin_lvgl_obj_t *title_label;
    gm_plugin_lvgl_obj_t *row_bg[MAX_VISIBLE_ROWS]; /* selection highlight */
    gm_plugin_lvgl_obj_t *row_icon[MAX_VISIBLE_ROWS]; /* bitmap icon left of the text */
    gm_plugin_lvgl_obj_t *row_label[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *row_value[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *hint_label;
    uint16_t page_count;
    uint16_t page_index;
    uint16_t scroll_offset; /* first visible row of the current page */
    uint16_t selected;      /* selected row index within the page */
    uint8_t visible_rows;   /* rows that fit on screen */
    int32_t row_height;
    int32_t icon_x;         /* left edge of the bitmap icon column */
    int32_t label_width;    /* label column width (left-aligned) */
    bool images_ok;         /* host can create/update bitmap images */
    int32_t label_x;        /* left edge of the label text */
    int32_t value_width;    /* value column width (right-aligned) */
    uint8_t language; /* 0=fr 1=en 2=es 3=it 4=de 5=zh-CN */
    uint32_t display_width;
    uint32_t display_height;
    /* Rectangles standing in for the cleared list while a card picture is on
     * screen: the picture is written straight to the framebuffer, so any LVGL
     * widget on top of it would hide it. */
    gm_plugin_lvgl_obj_t *remote_rect[MAX_VISIBLE_ROWS + 2U];
    uint8_t remote_rect_count;
    bool connected; /* phone link state, mirrored from connection events */
    uint16_t image_page; /* page whose card picture is on screen, or NO_PAGE */
    uint16_t image_width; /* geometry of the picture last shown */
    uint16_t image_height;
    int32_t image_x;
    int32_t image_y;
} ha_t;

#define NO_PAGE 0xFFFFU
#define NO_OFFSET 0xFFFFFFFFU

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

/* Keeps the bitmap icon of one row in sync with its wire icon id. The image
 * sits in the dedicated icon column, left of the text. It is only drawn when
 * the host can create/update bitmap images. */
static void show_row_icon(uint32_t slot, uint8_t icon)
{
    const gm_plugin_lvgl_image_dsc_t *dsc;

    if (slot >= MAX_VISIBLE_ROWS || ha.row_icon[slot] == 0) return;
    if (icon == NO_ICON) icon = HA_ICON_FALLBACK;
    if (icon >= HA_ICON_COUNT) icon = HA_ICON_FALLBACK;
    dsc = &ha_icons_dsc[icon];
    (void)ha.ui->image_set_source(ha.row_icon[slot], dsc);
    if (!ha.images_ok) {
        set_style(ha.row_icon[slot], GM_PLUGIN_LVGL_STYLE_IMG_OPA, number(0));
        return;
    }
    set_style(ha.row_icon[slot], GM_PLUGIN_LVGL_STYLE_IMG_OPA, number(255));
    ha.ui->obj_set_size(ha.row_icon[slot], (int32_t)dsc->width,
                         (int32_t)dsc->height);
}

static void hide_row_icon(uint32_t slot)
{
    if (slot >= MAX_VISIBLE_ROWS || ha.row_icon[slot] == 0) return;
    set_style(ha.row_icon[slot], GM_PLUGIN_LVGL_STYLE_IMG_OPA, number(0));
}

static void refresh_page(void);
static void run_action(void);
static void change_page(int32_t delta);
static void request_card_image(void);
static void move_selection(int32_t delta);
static void blank_rows(void);
static void add_remote_rect(int32_t x, int32_t y, int32_t width, int32_t height);
static void drop_remote_rects(void);
static void repaint_above_image(void);
static void layout_band_label(void);
static void on_image_presented(void);

/* Replaces a visible row with a single centered sentence. Used by image pages
 * for their waiting state. */
static void show_message(uint32_t slot, const char *text)
{
    if (slot >= ha.visible_rows) return;
    hide_row_icon(slot);
    ha.ui->label_set_text(ha.row_label[slot], text);
    ha.ui->label_set_text(ha.row_value[slot], "");
    set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
}

static void blank_rows(void)
{
    uint32_t slot;
    for (slot = 0U; slot < ha.visible_rows; slot++) {
        ha.ui->label_set_text(ha.row_label[slot], "");
        ha.ui->label_set_text(ha.row_value[slot], "");
        hide_row_icon(slot);
        set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
    }
}

/* Asks the phone for the picture of the image page currently shown. */
static void request_card_image(void)
{
    uint8_t payload[3];
    if (ha.page_count == 0U || !ha_pages[ha.page_index].image) return;
    if (!ha.connected) return;
    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_REQUEST_CARD;
    payload[2] = (uint8_t)(ha.page_index & 0xFFU);
    ha.host->log("homeassistant: request_card page=%u", (unsigned int)ha.page_index);
    (void)ha.host->bt_send(EVENT_CHANNEL, payload, sizeof(payload));
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

/* Builds the label shown for one row: just the entity name. The bitmap icon
 * lives in its own column to the left, so the text starts after a gap and never
 * crowds it. */
static void fill_row_text(char *out, size_t capacity, const char *label)
{
    size_t used = 0U;
    while (label[0] != '\0' && used + 1U < capacity) out[used++] = *label++;
    out[used] = '\0';
}

/* Reads the panel size once and derives the row geometry from it. Called at
 * start and whenever the list widgets are rebuilt. */
static gm_plugin_result_t layout(void)
{
    gm_plugin_display_info_t display;
    int32_t rows_area_height;

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
    if (ha.row_height < ROW_MIN_HEIGHT) return GM_PLUGIN_ESTATE;
    /* Rows fill rows_area_height exactly; anchor them just under the header. */
    rows_area_height = (int32_t)ha.visible_rows * ha.row_height;
    ha.display_width = display.width;
    ha.display_height = display.height;

    ha.host->log("homeassistant: display %ux%u visible_rows=%u row_height=%d area=%d",
                  (unsigned int)display.width, (unsigned int)display.height,
                  (unsigned int)ha.visible_rows, (int)ha.row_height,
                  (int)rows_area_height);
    return GM_PLUGIN_OK;
}

/* Creates the screen and the navigation hint. Shared by the list view and by
 * the overlay that sits on top of a card picture. */
static gm_plugin_result_t build_frame(void)
{
    gm_plugin_lvgl_obj_t *root;
    int32_t width;

    if (layout() != GM_PLUGIN_OK) return GM_PLUGIN_ESTATE;
    width = (int32_t)ha.display_width;
    root = ha.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;

    ha.screen = ha.ui->obj_create(root);
    if (ha.screen == 0) return GM_PLUGIN_ENOMEM;
    ha.ui->obj_set_size(ha.screen, width, (int32_t)ha.display_height);
    ha.ui->obj_align(ha.screen, GM_PLUGIN_LVGL_ALIGN_CENTER, 0, 0);
    style_panel(ha.screen, 0x00, 0, 0);

    ha.title_label = make_label(ha.screen, "", 0xF0, false);
    if (ha.title_label == 0) return GM_PLUGIN_ENOMEM;
    {
        gm_plugin_lvgl_style_value_t font_value = {0};
        font_value.ptr = ha.ui->font_large;
        set_style(ha.title_label, GM_PLUGIN_LVGL_STYLE_TEXT_FONT, font_value);
    }
    set_style(ha.title_label, GM_PLUGIN_LVGL_STYLE_TEXT_ALIGN,
              number(GM_PLUGIN_LVGL_TEXT_ALIGN_CENTER));
    ha.ui->obj_set_size(ha.title_label, width - SCREEN_MARGIN * 2U,
                         HEADER_HEIGHT);
    ha.ui->obj_align(ha.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 6);

    /* Navigation hints were removed at the wearer's request: the head gestures
     * are discoverable on their own and the text crowded the rows. The label is
     * kept (empty) so the layout stays unchanged. */
    ha.hint_label = make_label(ha.screen, "", 0x70, false);
    if (ha.hint_label == 0) return GM_PLUGIN_ENOMEM;
    ha.ui->obj_set_size(ha.hint_label, width - SCREEN_MARGIN * 2U, HINT_HEIGHT);
    ha.ui->obj_align(ha.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, 0);
    return GM_PLUGIN_OK;
}

/* Creates every row widget for the list view. */
static gm_plugin_result_t build_rows(void)
{
    uint32_t row;
    int32_t width = (int32_t)ha.display_width;

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

        /* The highlight rectangle is created first so the label text draws
         * above the selected-row bar. */
        ha.row_bg[row] = ha.ui->obj_create(ha.screen);
        if (ha.row_bg[row] == 0) return GM_PLUGIN_ENOMEM;
        ha.ui->obj_set_size(ha.row_bg[row], width - SCREEN_MARGIN - RIGHT_MARGIN,
                             ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_bg[row], SCREEN_MARGIN, top_y);
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(0x38));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_RADIUS, number(6));
        /* No outline around the highlight: only a flat filled bar. */
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(0));
        set_style(ha.row_bg[row], GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(0));
        ha.ui->obj_clear_flag(ha.row_bg[row], GM_PLUGIN_LVGL_FLAG_SCROLLABLE);

        /* Bitmap icon in its own column, behind the text so the selected-row
         * bar still shows through around it. */
        ha.row_icon[row] = ha.ui->image_create(ha.screen, &ha_icons_dsc[HA_ICON_FALLBACK]);
        if (ha.row_icon[row] == 0) return GM_PLUGIN_ENOMEM;
        ha.ui->obj_set_pos(ha.row_icon[row], ha.icon_x,
                           top_y + (ha.row_height - 2 - (int32_t)HA_ICON_SIZE) / 2);
        set_style(ha.row_icon[row], GM_PLUGIN_LVGL_STYLE_IMG_OPA, number(0));

        /* The label starts right of the icon column. */
        ha.row_label[row] = make_label(ha.screen, "", 0xD0, true);
        if (ha.row_label[row] == 0) return GM_PLUGIN_ENOMEM;
        ha.ui->obj_set_size(ha.row_label[row], ha.label_width, ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_label[row], ha.label_x, top_y);

        ha.row_value[row] = make_label(ha.screen, "", 0xF0, false);
        if (ha.row_value[row] == 0) return GM_PLUGIN_ENOMEM;
        ha.ui->obj_set_size(ha.row_value[row], ha.value_width, ha.row_height - 2);
        ha.ui->obj_set_pos(ha.row_value[row],
                           width - RIGHT_MARGIN - ha.value_width, top_y);
    }
    return GM_PLUGIN_OK;
}

static void reset_widgets(void)
{
    uint32_t slot;
    for (slot = 0U; slot < MAX_VISIBLE_ROWS; slot++) {
        ha.row_bg[slot] = 0;
        ha.row_icon[slot] = 0;
        ha.row_label[slot] = 0;
        ha.row_value[slot] = 0;
    }
    ha.screen = 0;
    ha.title_label = 0;
    ha.hint_label = 0;
}

/* Creates the full list view: panel, title, hint and every row. */
static gm_plugin_result_t build_ui(void)
{
    gm_plugin_result_t result;
    int32_t usable, gap;

    reset_widgets();
    result = build_frame();
    if (result != GM_PLUGIN_OK) return result;

    /* Split the usable width into a label column and a value column with a
     * fixed gap between them. The value gets a fixed share so long numeric
     * readings stay whole; the label takes the rest and truncates. The value
     * column stops short of the right edge (RIGHT_MARGIN) so it never hugs the
     * border. */
    usable = (int32_t)ha.display_width - SCREEN_MARGIN - RIGHT_MARGIN;
    gap = usable / 12;
    ha.value_width = usable * 2 / 5;
    ha.label_width = usable - ha.value_width - gap;
    if (ha.label_width < 0) ha.label_width = 0;

    /* A dedicated icon column holds the bitmap icon; the label starts after
     * ICON_CELL so there is a clear gap between icon and name. */
    ha.icon_x = SCREEN_MARGIN;
    ha.label_x = SCREEN_MARGIN + ICON_CELL;
    ha.label_width -= ICON_CELL;
    if (ha.label_width < 0) ha.label_width = 0;
    ha.host->log("homeassistant: icon_x=%d label_x=%d label_w=%d value_w=%d",
                  (int)ha.icon_x, (int)ha.label_x, (int)ha.label_width,
                  (int)ha.value_width);

    return build_rows();
}

/* Card pictures are written by the phone straight to the framebuffer, so on an
 * image page every widget must be fully transparent or absent. This overlay
 * keeps only the title band (with the page index) above the picture. */
static gm_plugin_result_t build_overlay(void)
{
    uint32_t slot;
    gm_plugin_lvgl_obj_t *root;
    int32_t width;

    reset_widgets();
    drop_remote_rects();
    if (layout() != GM_PLUGIN_OK) return GM_PLUGIN_ESTATE;
    width = (int32_t)ha.display_width;
    root = ha.ui->root_get();
    if (root == 0) return GM_PLUGIN_ESTATE;
    if (build_frame() != GM_PLUGIN_OK) return GM_PLUGIN_ENOMEM;

    /* The picture covers the title band; repaint it opaquely above. */
    ha.ui->obj_clean(ha.title_label);
    ha.title_label = make_label(root, "", 0xF0, false);
    if (ha.title_label == 0) return GM_PLUGIN_ENOMEM;
    layout_band_label();
    if (ha.remote_rect_count == 0U)
        add_remote_rect(0, 0, width, HEADER_HEIGHT);

    /* Invisible widgets: they keep the list code at home but must show nothing
     * over the picture. This waiting line lives in the bottom hint. */
    for (slot = 0U; slot < ha.visible_rows; slot++) {
        ha.row_bg[slot] = ha.ui->obj_create(root);
        ha.row_icon[slot] = ha.ui->image_create(root, &ha_icons_dsc[HA_ICON_FALLBACK]);
        ha.row_label[slot] = make_label(root, "", 0xD0, true);
        ha.row_value[slot] = make_label(root, "", 0xF0, false);
        if (ha.row_bg[slot] == 0 || ha.row_icon[slot] == 0 ||
            ha.row_label[slot] == 0 || ha.row_value[slot] == 0)
            return GM_PLUGIN_ENOMEM;
        set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
        set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BORDER_OPA, number(0));
        set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(0));
        set_style(ha.row_icon[slot], GM_PLUGIN_LVGL_STYLE_IMG_OPA, number(0));
        set_style(ha.row_label[slot], GM_PLUGIN_LVGL_STYLE_TEXT_OPA, number(0));
        set_style(ha.row_value[slot], GM_PLUGIN_LVGL_STYLE_TEXT_OPA, number(0));
    }
    if (ha.hint_label == 0) return GM_PLUGIN_ENOMEM;
    set_style(ha.hint_label, GM_PLUGIN_LVGL_STYLE_TEXT_OPA, number(0));
    ha.ui->label_set_text(ha.hint_label, "");
    return GM_PLUGIN_OK;
}

static void add_remote_rect(int32_t x, int32_t y, int32_t width, int32_t height)
{
    gm_plugin_lvgl_obj_t *root = ha.ui->root_get();
    gm_plugin_lvgl_obj_t *rect;

    if (root == 0) return;
    if (ha.remote_rect_count >= MAX_VISIBLE_ROWS) return;
    rect = ha.ui->obj_create(root);
    if (rect == 0) return;
    ha.remote_rect[ha.remote_rect_count++] = rect;
    ha.ui->obj_set_pos(rect, x, y);
    ha.ui->obj_set_size(rect, width, height);
    set_style(rect, GM_PLUGIN_LVGL_STYLE_BG_COLOR, color(0x00));
    set_style(rect, GM_PLUGIN_LVGL_STYLE_BG_OPA, number(255));
    set_style(rect, GM_PLUGIN_LVGL_STYLE_BORDER_WIDTH, number(0));
    set_style(rect, GM_PLUGIN_LVGL_STYLE_RADIUS, number(0));
    ha.ui->obj_clear_flag(rect, GM_PLUGIN_LVGL_FLAG_SCROLLABLE);
}

static void drop_remote_rects(void)
{
    uint32_t slot;
    for (slot = 0U; slot < ha.remote_rect_count; slot++) {
        if (ha.remote_rect[slot] != 0) ha.ui->obj_clean(ha.remote_rect[slot]);
        ha.remote_rect[slot] = 0;
    }
    ha.remote_rect_count = 0U;
}

/* Card pictures land directly in the framebuffer and the widgets a picture
 * covered stay erased, so every repaint of the band must restore them: the
 * opaque rectangle that hides the pixels underneath, then the title label on
 * top of it. The rectangle is created last so it also sits above the title:
 * LVGL then never draws the text into its own layer, the band area picks up no
 * static cursor, and the label's position is set explicitly right after the
 * file-scope callback. A text redraw may still paint the label under the
 * rectangle (one frame of latency, never a flicker). */
static void repaint_above_image(void)
{
    gm_plugin_lvgl_obj_t *root = ha.ui->root_get();
    gm_plugin_lvgl_obj_t *rect;
    int32_t width = (int32_t)ha.display_width;

    if (root == 0 || ha.title_label == 0) return;
    if (ha.remote_rect_count == 0U) {
        add_remote_rect(0, 0, width, HEADER_HEIGHT);
        if (ha.remote_rect_count == 0U) return;
    }
    rect = ha.remote_rect[0];
    if (rect == 0) return;
    ha.ui->obj_set_pos(rect, 0, 0);
    ha.ui->obj_set_size(rect, width, HEADER_HEIGHT);
}

static void layout_band_label(void)
{
    int32_t width = (int32_t)ha.display_width;

    if (ha.title_label == 0) return;
    ha.ui->obj_set_size(ha.title_label, width - SCREEN_MARGIN * 2U,
                        HEADER_HEIGHT);
    ha.ui->obj_align(ha.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 6);
    set_style(ha.title_label, GM_PLUGIN_LVGL_STYLE_TEXT_OPA, number(255));
}

/* A card picture was just written: only that page's title band is redrawn, and
 * only while the plugin view still shows the picture's page. */
static void on_image_presented(void)
{
    if (ha.screen != 0 || ha.page_count == 0U) return;
    if (ha.image_page != ha.page_index) return;
    ha.host->log("homeassistant: image presented page=%u",
                  (unsigned int)ha.page_index);
    repaint_above_image();
}

static void ensure_selection_visible(void)
{
    /* Image pages have no rows: there is nothing to scroll. */
    if (ha.screen == 0) return;
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
    if (page->image || ha.selected >= page->row_count) return;

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
    /* Landing on a picture page asks for its picture straight away: the
     * wearer only nods again to refresh it. */
    if (ha_pages[ha.page_index].image) request_card_image();
    payload[0] = PROTOCOL_VERSION;
    payload[1] = EVENT_PAGE_CHANGED;
    payload[2] = (uint8_t)(ha.page_index & 0xFFU);
    (void)ha.host->bt_send(EVENT_CHANNEL, payload, sizeof(payload));
}


static gm_plugin_result_t homeassistant_start(void *context)
{
    char locale[GM_PLUGIN_LOCALE_TAG_MAX];

    (void)context;

    ha.remote_rect_count = 0U;
    ha.connected = false;
    ha.image_page = NO_PAGE;
    ha.image_width = 0U;
    ha.image_height = 0U;
    ha.image_x = 0;
    ha.image_y = 0;
    ha.language = 0U;
    if (ha.host->locale_get != 0 &&
        ha.host->locale_get(locale) == GM_PLUGIN_OK)
        ha.language = ha_detect_language(locale);

    if (build_ui() != GM_PLUGIN_OK) return GM_PLUGIN_ENOMEM;

    ha.page_count = 0U;
    ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    refresh_page();

    if (ha.host->imu_enable(GM_PLUGIN_IMU_ENABLE_GESTURES) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOMEM;
    return GM_PLUGIN_OK;
}

/* Renders the current page: title with an "i/n" position marker, the visible
 * slice of label/value rows starting at scroll_offset, and a navigation hint.
 * Rows past the page's row_count are blanked so a shorter page never shows
 * stale entries. An image page is drawn by the phone instead: this function
 * only keeps the title and, until the picture arrives, one hint line. */
static void refresh_page(void)
{
    char title[64];
    char row_text[MAX_LABEL_BYTES + 4U];
    uint32_t slot;

    if (ha.page_count == 0U) {
        if (ha.screen == 0 && build_ui() != GM_PLUGIN_OK) return;
        ha.ui->label_set_text(ha.title_label,
            L("Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant", "Home Assistant"));
        for (slot = 0U; slot < ha.visible_rows; slot++) {
            ha.ui->label_set_text(ha.row_label[slot], "");
            ha.ui->label_set_text(ha.row_value[slot], "");
            hide_row_icon(slot);
            set_style(ha.row_bg[slot], GM_PLUGIN_LVGL_STYLE_BG_OPA, number(0));
        }
        ha.ui->label_set_text(ha.hint_label,
            L("En attente du telephone...", "Waiting for the phone...", "Esperando al teléfono...", "In attesa del telefono...", "Warte auf das Telefon...", "正在等待手机..."));
        ha.image_page = NO_PAGE;
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

        /* The list may have been cleared by a previous picture: rebuild it
         * before touching the row widgets. */
        if (page->image) {
            if (ha.image_page == ha.page_index && ha.screen == 0) return;
            if (ha.image_page != ha.page_index && build_ui() != GM_PLUGIN_OK) return;
        } else if (ha.screen == 0) {
            ha.image_page = NO_PAGE;
            ha.image_width = 0U;
            ha.image_height = 0U;
            if (build_ui() != GM_PLUGIN_OK) return;
        }

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

        if (page->image) {
            /* The picture is written by the phone straight to the framebuffer;
             * our rows would cover it. Once it landed, take the list away and
             * keep only the title band. Until then, one hint line explains how
             * to load it. */
            blank_rows();
            if (ha.image_page == ha.page_index) {
                if (build_overlay() != GM_PLUGIN_OK) return;
                ha.ui->label_set_text(ha.title_label, title);
            } else {
                ha.ui->label_set_text(ha.hint_label, "");
                show_message(0U,
                    L("Inclinez la tete vers le bas pour charger l'image.",
                      "Nod down to load the picture.",
                      "Inclina la cabeza hacia abajo para cargar la imagen.",
                      "Inclina la testa verso il basso per caricare l'immagine.",
                      "Kopf nach unten neigen, um das Bild zu laden.",
                      "向下点头以加载图像。"));
            }
            return;
        }

        /* A row page took the screen back from a picture. */
        if (ha.image_page != NO_PAGE) {
            ha.image_page = NO_PAGE;
            ha.image_width = 0U;
            ha.image_height = 0U;
        }

        for (slot = 0U; slot < ha.visible_rows; slot++) {
            uint32_t row = (uint32_t)ha.scroll_offset + slot;
            bool is_selected = (row < page->row_count) && ((uint16_t)row == ha.selected);
            if (row < page->row_count) {
                fill_row_text(row_text, sizeof(row_text),
                              page->rows[row].label);
                ha.ui->label_set_text(ha.row_label[slot], row_text);
                ha.ui->label_set_text(ha.row_value[slot], page->rows[row].value);
                show_row_icon(slot, page->rows[row].icon);
            } else {
                ha.ui->label_set_text(ha.row_label[slot], "");
                ha.ui->label_set_text(ha.row_value[slot], "");
                hide_row_icon(slot);
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

static void handle_set_pages(const uint8_t *data, uint32_t length)
{
    uint32_t offset;
    uint32_t flags_offset;
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

    flags_offset = NO_OFFSET;
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
        page->image = false;
    }
    ha.page_count = p;
    /* Trailer: exactly one flag byte per page, right at the end. */
    if (p == requested_pages && length >= offset &&
        length - offset == (uint32_t)requested_pages)
        flags_offset = offset;
    for (p = 0U; p < ha.page_count; p++) {
        if (flags_offset != NO_OFFSET && data[flags_offset + p] == 1U)
            ha_pages[p].image = true;
    }
    if (ha.page_index >= ha.page_count) ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.selected = 0U;
    ha.host->log("homeassistant: set_pages count=%u page0_rows=%u visible=%u",
                  (unsigned int)ha.page_count,
                  (unsigned int)(ha.page_count ? ha_pages[0].row_count : 0U),
                  (unsigned int)ha.visible_rows);
    /* First icon ids of page 0, to tell a phone that sends no icon (ff) from
     * an unexpected id. */
    for (p = 0U; p < 4U && p < ha_pages[0].row_count; p++)
        ha.host->log("homeassistant: row%u icon=%u",
                      (unsigned int)p, (unsigned int)ha_pages[0].rows[p].icon);
    refresh_page();
}

static bool homeassistant_event(void *context, const gm_plugin_event_t *event)
{
    (void)context;
    if (event == 0) return false;

    if (event->type == GM_PLUGIN_EVENT_CONNECTION) {
        ha.host->log("homeassistant: phone connected=%u",
                      (unsigned int)event->data.connection.connected);
        ha.connected = event->data.connection.connected;
        if (ha.connected && ha.page_count != 0U) refresh_page();
        return true;
    }

    if (event->type == GM_PLUGIN_EVENT_BT_MESSAGE) {
        if (event->data.bt.data == 0) return false;
        /* Pictures travel on the display channels, which the Host applies
         * itself (and drops when a plugin page is not visible). The plugin is
         * told once the pixels are in the framebuffer so it can keep the title
         * band above them; its own message channel keeps carrying the pages. */
        if (event->data.bt.channel != PAGES_CHANNEL) {
            on_image_presented();
            return true;
        }
        handle_set_pages(event->data.bt.data, event->data.bt.length);
        return true;
    }

    if (ha.page_count == 0U) return false;

    if (event->type == GM_PLUGIN_EVENT_BUTTON &&
        event->data.button.button == GM_PLUGIN_BUTTON_PRIMARY &&
        event->data.button.action == GM_PLUGIN_BUTTON_ACTION_SINGLE) {
        if (ha_pages[ha.page_index].image) {
            ha.image_page = NO_PAGE;
            request_card_image();
            refresh_page();
            return true;
        }
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
        case GM_PLUGIN_IMU_GESTURE_NOD:
            if (ha_pages[ha.page_index].image) {
                /* A nod is reported by its own gesture id; treat it as the
                 * documented "nod down" reload for picture pages. */
                ha.image_page = NO_PAGE;
                request_card_image();
                refresh_page();
                return true;
            }
            break;
        case GM_PLUGIN_IMU_GESTURE_HEAD_LOWER:
            if (ha_pages[ha.page_index].image) {
                /* Image pages have no rows; a nod fetches (or refreshes) the
                 * card picture instead of moving a cursor. */
                ha.image_page = NO_PAGE;
                request_card_image();
                refresh_page();
                return true;
            }
            move_selection(1);
            return true;
        default:
            break;
        }
        /* Any other gesture (or a picture page one) refreshes the picture, so
         * nudging the head never leaves the wearer stuck on a blank page. */
        if (ha_pages[ha.page_index].image) {
            ha.image_page = NO_PAGE;
            request_card_image();
            refresh_page();
            return true;
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
    ha.image_page = NO_PAGE;
    ha.image_width = 0U;
    ha.image_height = 0U;
    ha.screen = 0;
    ha.title_label = 0;
    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) {
        ha.row_bg[row] = 0;
        ha.row_icon[row] = 0;
        ha.row_label[row] = 0;
        ha.row_value[row] = 0;
    }
    ha.hint_label = 0;
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
    ha.images_ok = ha.ui->struct_size >= GM_PLUGIN_LVGL_API_1_1_SIZE &&
        GM_PLUGIN_VERSION_COMPATIBLE(ha.ui->api_version,
                                     GM_PLUGIN_VERSION(1U, 1U)) &&
        ha.ui->image_create != 0 && ha.ui->image_set_source != 0;
    if (gm_plugin_libc_get(host, &ha.libc) != GM_PLUGIN_OK)
        return GM_PLUGIN_ENOTSUP;
    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &ha;
    plugin->on_start = homeassistant_start;
    plugin->on_event = homeassistant_event;
    plugin->on_stop = homeassistant_stop;
    return GM_PLUGIN_OK;
}
