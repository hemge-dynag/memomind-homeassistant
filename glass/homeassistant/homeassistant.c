#include "gm_plugin_lvgl_api.h"
#include "gm_plugin_libc.h"

/* Paired with PhoneSDK/examples/homeassistant. The phone connects to a Home
 * Assistant instance, fetches entities grouped into dashboard pages, and pushes
 * every page to this plugin. The wearer drives it hands-free: turning the head
 * left or right flips between pages, while raising or lowering the head
 * scrolls the rows of the current page when a page holds more sensors than fit
 * on screen. Each page shows a title and up to MAX_ROWS_PER_PAGE label/value
 * rows (sensor readings, light state, ...). The glasses never talk to Home
 * Assistant directly: all network work stays on the phone.
 */

#define PAGES_CHANNEL UINT16_C(0x4841)  /* 'HA': phone -> glasses */
#define EVENT_CHANNEL UINT16_C(0x4853)  /* 'HS': glasses -> phone */
#define PROTOCOL_VERSION 1U

#define COMMAND_SET_PAGES 1U
#define COMMAND_CLEAR 2U

#define EVENT_PAGE_CHANGED UINT8_C(1)

#define MAX_PAGES 24U
/* Rows buffered per page. The visible window is smaller; the rest is reachable
 * by scrolling with head up/down. */
#define MAX_ROWS_PER_PAGE 32U
/* Upper bound on rows drawn at once; the actual count (ha.visible_rows) is
 * derived from the panel height at start and never exceeds this. */
#define MAX_VISIBLE_ROWS 8U
/* Per row: label <= 32 bytes, value <= 24 bytes. Per page title <= 32 bytes. */
#define MAX_LABEL_BYTES 32U
#define MAX_VALUE_BYTES 24U
#define MAX_TITLE_BYTES 32U

/* version(1) + command(1) + page_count(2) */
#define MIN_SET_PAGES_PAYLOAD 4U

#define SCREEN_MARGIN 16
#define HEADER_HEIGHT 44
#define HINT_HEIGHT 28
#define ROW_MIN_HEIGHT 26

typedef struct {
    char label[MAX_LABEL_BYTES + 1U];
    char value[MAX_VALUE_BYTES + 1U];
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
    gm_plugin_lvgl_obj_t *row_label[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *row_value[MAX_VISIBLE_ROWS];
    gm_plugin_lvgl_obj_t *hint_label;
    uint16_t page_count;
    uint16_t page_index;
    uint16_t scroll_offset; /* first visible row of the current page */
    uint8_t visible_rows;   /* rows that fit on screen */
    int32_t row_height;
    int32_t label_width;    /* label column width (left-aligned) */
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
        for (slot = 0U; slot < MAX_VISIBLE_ROWS; slot++) {
            ha.ui->label_set_text(ha.row_label[slot], "");
            ha.ui->label_set_text(ha.row_value[slot], "");
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
            if (row < page->row_count) {
                ha.ui->label_set_text(ha.row_label[slot], page->rows[row].label);
                ha.ui->label_set_text(ha.row_value[slot], page->rows[row].value);
            } else {
                ha.ui->label_set_text(ha.row_label[slot], "");
                ha.ui->label_set_text(ha.row_value[slot], "");
            }
        }

        if (total_rows > ha.visible_rows) {
            ha.ui->label_set_text(ha.hint_label,
                L("Gauche/droite: page  Haut/bas: defiler",
                  "Left/right: page  Up/down: scroll",
                  "Izq./der.: página  Arriba/abajo: desplazar",
                  "Sinistra/destra: pagina  Su/giù: scorrere",
                  "Links/rechts: Seite  Hoch/runter: scrollen",
                  "左/右：翻页  上/下：滚动"));
        } else {
            ha.ui->label_set_text(ha.hint_label,
                L("Tete gauche/droite: changer de page",
                  "Head left/right: change page",
                  "Cabeza izq./der.: cambiar página",
                  "Testa sinistra/destra: cambia pagina",
                  "Kopf links/rechts: Seite wechseln",
                  "头左/右：切换页面"));
        }
    }
}

/* Scrolls the current page by delta rows, clamped to [0, rows-visible]. No-op
 * when everything already fits on screen. */
static void scroll_page(int32_t delta)
{
    const ha_page_t *page;
    int32_t max_offset;
    int32_t next;

    if (ha.page_count == 0U) return;
    page = &ha_pages[ha.page_index];
    if (page->row_count <= ha.visible_rows) return;

    max_offset = (int32_t)page->row_count - (int32_t)ha.visible_rows;
    next = (int32_t)ha.scroll_offset + delta;
    if (next < 0) next = 0;
    if (next > max_offset) next = max_offset;
    if ((uint16_t)next == ha.scroll_offset) return;
    ha.scroll_offset = (uint16_t)next;
    refresh_page();
}

/* Loops through pages in both directions so navigation never sticks at the
 * ends. Only fires when there is more than one page. */
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
     * beyond that stay in the page buffer and are reached by scrolling. */
    ha.visible_rows = (uint8_t)(rows_area_height / ROW_MIN_HEIGHT);
    if (ha.visible_rows > MAX_VISIBLE_ROWS) ha.visible_rows = MAX_VISIBLE_ROWS;
    if (ha.visible_rows == 0U) return GM_PLUGIN_ESTATE;
    ha.row_height = rows_area_height / (int32_t)ha.visible_rows;
    if (ha.row_height < ROW_MIN_HEIGHT)
        return GM_PLUGIN_ESTATE;

    /* Split the usable width into a label column and a value column with a
     * fixed gap between them. The value gets a fixed share so long numeric
     * readings stay whole; the label takes the rest and truncates. */
    {
        int32_t usable = (int32_t)display.width - SCREEN_MARGIN * 2;
        int32_t gap = usable / 12;
        ha.value_width = usable * 2 / 5;
        ha.label_width = usable - ha.value_width - gap;
        if (ha.label_width < 0) ha.label_width = 0;
    }

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
    ha.ui->obj_set_size(ha.title_label, display.width - SCREEN_MARGIN * 2U,
                         HEADER_HEIGHT);
    ha.ui->obj_align(ha.title_label, GM_PLUGIN_LVGL_ALIGN_TOP_MID, 0, 6);

    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) {
        int32_t offset_y = HEADER_HEIGHT
            + (int32_t)row * ha.row_height - (HINT_HEIGHT / 2);
        /* Two fixed columns: label hugs the left margin, value the right margin.
         * Sizing both from a single split point (instead of centring each with
         * its own offset) guarantees a long label can never push the value off
         * the screen; the label clips with "..." via LABEL_DOT. */
        ha.row_label[row] = make_label(ha.screen, "", 0xB0, true);
        if (ha.row_label[row] == 0) goto no_memory;
        ha.ui->obj_set_size(ha.row_label[row], ha.label_width,
                             ha.row_height - 2);
        ha.ui->obj_align(ha.row_label[row], GM_PLUGIN_LVGL_ALIGN_LEFT_MID,
                         SCREEN_MARGIN, offset_y);

        ha.row_value[row] = make_label(ha.screen, "", 0xF0, false);
        if (ha.row_value[row] == 0) goto no_memory;
        ha.ui->obj_set_size(ha.row_value[row], ha.value_width,
                             ha.row_height - 2);
        ha.ui->obj_align(ha.row_value[row], GM_PLUGIN_LVGL_ALIGN_RIGHT_MID,
                         -SCREEN_MARGIN, offset_y);
    }

    ha.hint_label = make_label(ha.screen, "", 0x70, false);
    if (ha.hint_label == 0) goto no_memory;
    ha.ui->obj_set_size(ha.hint_label, display.width - SCREEN_MARGIN * 2U,
                         HINT_HEIGHT);
    ha.ui->obj_align(ha.hint_label, GM_PLUGIN_LVGL_ALIGN_BOTTOM_MID, 0, -4);

    ha.page_count = 0U;
    ha.page_index = 0U;
    refresh_page();

    if (ha.host->imu_enable(GM_PLUGIN_IMU_ENABLE_GESTURES) != GM_PLUGIN_OK)
        goto no_memory;
    return GM_PLUGIN_OK;

no_memory:
    ha.ui->obj_clean(root);
    ha.screen = 0;
    ha.title_label = 0;
    for (row = 0U; row < MAX_VISIBLE_ROWS; row++) {
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
 *     { label_len, label, value_len, value } * row_count } * page_count ] */
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
        }
        page->row_count = (uint8_t)r;
    }
    ha.page_count = p;
    if (ha.page_index >= ha.page_count) ha.page_index = 0U;
    ha.scroll_offset = 0U;
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

    if (event->type == GM_PLUGIN_EVENT_IMU_GESTURE &&
        event->data.imu_gesture.active) {
        switch (event->data.imu_gesture.gesture) {
        case GM_PLUGIN_IMU_GESTURE_RIGHT:
            change_page(1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_LEFT:
            change_page(-1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_HEAD_RAISE:
            scroll_page(-1);
            return true;
        case GM_PLUGIN_IMU_GESTURE_HEAD_LOWER:
            scroll_page(1);
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
        ha.row_label[row] = 0;
        ha.row_value[row] = 0;
    }
    ha.hint_label = 0;
    ha.page_count = 0U;
    ha.page_index = 0U;
    ha.scroll_offset = 0U;
    ha.visible_rows = 0U;
}

gm_plugin_result_t gm_plugin_entry(const gm_plugin_host_api_t *host,
                                    gm_plugin_descriptor_t *plugin)
{
    const gm_plugin_capabilities_t required =
        GM_PLUGIN_CAP_BLUETOOTH | GM_PLUGIN_CAP_IMU_EVENTS;

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
    if (ha.ui->struct_size < GM_PLUGIN_LVGL_API_MIN_SIZE ||
        !GM_PLUGIN_VERSION_COMPATIBLE(ha.ui->api_version,
                                      GM_PLUGIN_LVGL_API_MIN_VERSION))
        return GM_PLUGIN_EVERSION;

    plugin->abi_version = GM_PLUGIN_ABI_MIN_VERSION;
    plugin->context = &ha;
    plugin->on_start = homeassistant_start;
    plugin->on_event = homeassistant_event;
    plugin->on_stop = homeassistant_stop;
    return GM_PLUGIN_OK;
}
