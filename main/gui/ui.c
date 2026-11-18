/*
 * LVGL 9 user interface for the SD-card Bitcoin wallet (320x480 portrait).
 *
 * Flow:  boot -> [no SD] -> [welcome: create / restore] -> [unlock] -> main
 *        main = tabview { Home | Receive | Send | History | Settings }
 *
 * Threading: everything here runs in the LVGL task except job_* functions,
 * which run on the worker task and wrap UI updates in ui_lock()/ui_unlock().
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "lvgl.h"
#include "lvgl_port.h"
#include "memzero.h"

#include "board/board.h"
#include "net/wifi_mgr.h"
#include "sys/sd_update.h"
#include "sys/worker.h"
#include "wallet/account.h"
#include "wallet/btc_tx.h"
#include "wallet/wallet.h"

static const char *TAG = "ui";

#define COLOR_ACCENT    lv_color_hex(0xF7931A)
#define COLOR_OK        lv_color_hex(0x3FB950)
#define COLOR_ERR       lv_color_hex(0xF85149)
#define COLOR_MUTED     lv_color_hex(0x9AA4AF)

/* ------------------------------------------------------------ state */

static lv_obj_t *s_main;            /* main tabview screen (kept while unlocked) */
static lv_obj_t *s_busy;            /* busy overlay on the top layer */
static lv_obj_t *s_busy_label;

/* main screen widgets */
static lv_obj_t *s_tv;
static lv_obj_t *s_status_wifi, *s_status_bat, *s_status_net;
static lv_obj_t *s_bal_btc, *s_bal_sub, *s_sync_lbl, *s_fp_lbl;
static lv_obj_t *s_rx_qr, *s_rx_addr, *s_rx_path;
static lv_obj_t *s_send_addr, *s_send_amt, *s_send_all, *s_send_fee, *s_send_fee_lbl, *s_kb;
static lv_obj_t *s_hist_list;
static lv_obj_t *s_wifi_item;
static lv_obj_t *s_wifi_dd, *s_wifi_pw, *s_wifi_state;

/* onboarding scratch (wiped as soon as possible) */
static char s_mnemonic[256];
static int  s_verify_idx[3];
static int  s_verify_step;

/* password screen purpose */
typedef void (*pw_cb_t)(const char *pw);

/* send flow */
static btc_tx_plan_t *s_plan;

static void route(void);
static void show_main(void);
static void show_unlock(void);
static void show_welcome(void);
static void show_words(bool onboarding);
static void main_refresh(void);
static void do_lock(void);
static void on_back_welcome(lv_event_t *e) { show_welcome(); }
static void on_back_main(lv_event_t *e) { show_main(); }

/* ------------------------------------------------------------ helpers */

static lv_obj_t *scr_new(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s, 12, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s, 8, 0);
    return s;
}

/* Load scr; delete the previous screen unless it is the persistent main screen. */
static void go(lv_obj_t *scr)
{
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(scr);
    if (old && old != scr && old != s_main) {
        lv_obj_delete_async(old);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *txt, const lv_font_t *font)
{
    lv_obj_t *l = label(parent, txt, font, lv_color_white());
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

static void ta_wipe(lv_obj_t *ta)
{
    if (!ta) {
        return;
    }
    char *t = (char *)lv_textarea_get_text(ta);
    if (t) {
        memzero(t, strlen(t));
    }
    lv_textarea_set_text(ta, "");
}

static void alert(const char *title, const char *text)
{
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, title);
    lv_obj_t *t = lv_msgbox_add_text(mb, text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_msgbox_add_close_button(mb);
    lv_obj_set_width(mb, 300);
}

static void busy(const char *msg)
{
    if (!s_busy) {
        s_busy = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_busy);
        lv_obj_set_size(s_busy, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_busy, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_busy, LV_OPA_80, 0);
        lv_obj_add_flag(s_busy, LV_OBJ_FLAG_CLICKABLE);   /* swallow touches */
        lv_obj_t *sp = lv_spinner_create(s_busy);
        lv_obj_set_size(sp, 70, 70);
        lv_obj_align(sp, LV_ALIGN_CENTER, 0, -30);
        s_busy_label = label(s_busy, "", &lv_font_montserrat_16, lv_color_white());
        lv_label_set_long_mode(s_busy_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_busy_label, 280);
        lv_obj_set_style_text_align(s_busy_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_busy_label, LV_ALIGN_CENTER, 0, 40);
    }
    lv_label_set_text(s_busy_label, msg);
}

static void idle(void)
{
    if (s_busy) {
        lv_obj_delete(s_busy);
        s_busy = NULL;
        s_busy_label = NULL;
    }
}

/* Called from the worker task. */
static void progress_cb(const char *msg)
{
    if (ui_lock(1000)) {
        if (s_busy) {
            lv_label_set_text(s_busy_label, msg);
        }
        ui_unlock();
    }
}

static bool start_job(worker_fn_t fn, void *arg, const char *msg)
{
    if (!worker_run(fn, arg)) {
        alert("Busy", "Another operation is still running.");
        return false;
    }
    busy(msg);
    return true;
}

/* Keyboard shared by a screen's text areas. */
static void kb_ta_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    }
}

static void kb_hide_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    }
}

static lv_obj_t *keyboard(lv_obj_t *scr, bool floating)
{
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, lv_pct(100), 200);
    if (floating) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 12);
    }
    return kb;
}

static lv_obj_t *textarea(lv_obj_t *parent, const char *placeholder, bool password, lv_obj_t *kb)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_width(ta, lv_pct(100));
    if (kb) {
        lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_FOCUSED, kb);
        lv_obj_add_event_cb(ta, kb_ta_event, LV_EVENT_CLICKED, kb);
    }
    return ta;
}

/*
 * Text-entry screen layout: a scrollable form body on top and a keyboard docked
 * below it. The keyboard is a sibling created after the body, so it is always
 * drawn on top and never hidden behind form widgets.
 */
#define FORM_KB_H   210

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *body;
    lv_obj_t *kb;
} form_t;

static void form_kb_event(lv_event_t *e);

static form_t form_new(void)
{
    form_t f;
    f.scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(f.scr, 0, 0);
    lv_obj_remove_flag(f.scr, LV_OBJ_FLAG_SCROLLABLE);

    f.body = lv_obj_create(f.scr);
    lv_obj_remove_style_all(f.body);
    lv_obj_set_size(f.body, lv_pct(100), BOARD_LCD_V_RES - FORM_KB_H);
    lv_obj_align(f.body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(f.body, 10, 0);
    lv_obj_set_style_pad_row(f.body, 8, 0);
    lv_obj_set_flex_flow(f.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(f.body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(f.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(f.body, LV_DIR_VER);

    f.kb = lv_keyboard_create(f.scr);
    lv_obj_set_size(f.kb, lv_pct(100), FORM_KB_H);
    lv_obj_align(f.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(f.kb, form_kb_event, LV_EVENT_CANCEL, NULL);
    return f;
}

/* Point the keyboard at ta, show its cursor and scroll it into view. */
static void form_focus(lv_obj_t *kb, lv_obj_t *ta)
{
    lv_obj_t *old = lv_keyboard_get_textarea(kb);
    if (old && old != ta) {
        lv_obj_remove_state(old, LV_STATE_FOCUSED);
    }
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

/* The keyboard's "close" key just clears the field focus; the keyboard stays docked. */
static void form_kb_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    }
}

static void form_show(form_t *f)
{
    go(f->scr);
    lv_obj_invalidate(f->scr);
}

static void fmt_btc(int64_t sats, char *out, size_t n)
{
    btc_format_amount(sats, out, n);
}

/* ======================================================== no SD card */

static void job_retry_sd(void *arg)
{
    board_sd_unmount();
    board_sd_mount();
    if (ui_lock(-1)) {
        idle();
        route();
        ui_unlock();
    }
}

static void on_retry_sd(lv_event_t *e)
{
    start_job(job_retry_sd, NULL, "Mounting SD card...");
}

static void show_no_sd(void)
{
    lv_obj_t *s = scr_new();
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(s, LV_SYMBOL_SD_CARD, &lv_font_montserrat_28, COLOR_ACCENT);
    label(s, "No SD card", &lv_font_montserrat_20, lv_color_white());
    wrap_label(s, "Insert a FAT32 formatted microSD card. Your encrypted wallet is stored on it "
                  "in /btcwallet/wallet.dat.", &lv_font_montserrat_14);
    button(s, LV_SYMBOL_REFRESH "  Retry", on_retry_sd, NULL);
    go(s);
}

/* ======================================================== onboarding */

static void on_create(lv_event_t *e)
{
    int words = (int)(intptr_t)lv_event_get_user_data(e);
    if (wallet_generate_mnemonic(words, s_mnemonic, sizeof(s_mnemonic)) != ESP_OK) {
        alert("Error", "Could not generate a recovery phrase.");
        return;
    }
    show_words(true);
}

static void show_restore(void);
static void on_restore(lv_event_t *e)
{
    show_restore();
}

static void show_welcome(void)
{
    memzero(s_mnemonic, sizeof(s_mnemonic));
    lv_obj_t *s = scr_new();
    label(s, LV_SYMBOL_SD_CARD "  SD Bitcoin Wallet", &lv_font_montserrat_20, COLOR_ACCENT);
    char net[64];
    snprintf(net, sizeof(net), "Network: %s", wallet_network_name());
    label(s, net, &lv_font_montserrat_14, COLOR_MUTED);
    wrap_label(s, "No wallet found on this SD card.\n\n"
                  "Create a new wallet and write the recovery phrase on paper, or restore an "
                  "existing BIP39 phrase (BIP84 native segwit).", &lv_font_montserrat_14);
    button(s, LV_SYMBOL_PLUS "  New wallet (24 words)", on_create, (void *)(intptr_t)24);
    button(s, LV_SYMBOL_PLUS "  New wallet (12 words)", on_create, (void *)(intptr_t)12);
    button(s, LV_SYMBOL_DOWNLOAD "  Restore from phrase", on_restore, NULL);
    go(s);
}

/* ---- show words ---- */

static void show_verify(void);

static void on_words_done(lv_event_t *e)
{
    bool onboarding = (bool)(intptr_t)lv_event_get_user_data(e);
    if (!onboarding) {
        memzero(s_mnemonic, sizeof(s_mnemonic));
        show_main();
        return;
    }
    /* pick 3 distinct random positions to verify */
    int n = 1;
    for (const char *p = s_mnemonic; *p; p++) {
        n += (*p == ' ');
    }
    for (int i = 0; i < 3; i++) {
        int v;
        bool dup;
        do {
            v = (int)(esp_random() % (uint32_t)n);
            dup = false;
            for (int k = 0; k < i; k++) {
                dup |= (s_verify_idx[k] == v);
            }
        } while (dup);
        s_verify_idx[i] = v;
    }
    s_verify_step = 0;
    show_verify();
}

static void show_words(bool onboarding)
{
    lv_obj_t *s = scr_new();
    label(s, "Recovery phrase", &lv_font_montserrat_20, COLOR_ACCENT);
    wrap_label(s, "Write these words on paper, in order. Anyone with them can spend your "
                  "bitcoin. Never photograph or type them anywhere.", &lv_font_montserrat_12);

    lv_obj_t *grid = lv_obj_create(s);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 2, 0);

    char copy[256];
    strlcpy(copy, s_mnemonic, sizeof(copy));
    int i = 0;
    char *save = NULL;
    for (char *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        char item[32];
        snprintf(item, sizeof(item), "%2d. %s", ++i, w);
        lv_obj_t *l = label(grid, item, &lv_font_montserrat_16, lv_color_white());
        lv_obj_set_width(l, lv_pct(48));
    }
    memzero(copy, sizeof(copy));
    button(s, onboarding ? LV_SYMBOL_OK "  I wrote them down" : LV_SYMBOL_LEFT "  Done",
           on_words_done, (void *)(intptr_t)onboarding);
    go(s);
}

/* ---- verify ---- */

static void show_password_create(void);

static void on_verify_ready(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_user_data(e);
    char want[16] = {0};
    char copy[256];
    strlcpy(copy, s_mnemonic, sizeof(copy));
    int i = 0;
    char *save = NULL;
    for (char *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save), i++) {
        if (i == s_verify_idx[s_verify_step]) {
            strlcpy(want, w, sizeof(want));
        }
    }
    memzero(copy, sizeof(copy));
    char got[32];
    wallet_mnemonic_normalise(lv_textarea_get_text(ta), got, sizeof(got));
    bool ok = strcmp(want, got) == 0;
    memzero(want, sizeof(want));
    memzero(got, sizeof(got));
    if (!ok) {
        show_words(true);
        alert("Wrong word", "That doesn't match. Check your written copy and try again.");
        return;
    }
    if (++s_verify_step >= 3) {
        show_password_create();
    } else {
        show_verify();
    }
}

static void show_verify(void)
{
    form_t f = form_new();
    char t[48];
    snprintf(t, sizeof(t), "Verify (%d/3)", s_verify_step + 1);
    label(f.body, t, &lv_font_montserrat_20, COLOR_ACCENT);
    snprintf(t, sizeof(t), "Enter word #%d of your phrase", s_verify_idx[s_verify_step] + 1);
    label(f.body, t, &lv_font_montserrat_16, lv_color_white());
    lv_obj_t *ta = textarea(f.body, "word", false, f.kb);
    lv_obj_t *ok = button(f.body, LV_SYMBOL_OK "  Check", on_verify_ready, ta);
    lv_obj_set_style_bg_color(ok, COLOR_ACCENT, 0);
    lv_obj_add_event_cb(f.kb, on_verify_ready, LV_EVENT_READY, ta);
    form_focus(f.kb, ta);
    form_show(&f);
}

/* ---- restore ---- */

static lv_obj_t *s_restore_ta, *s_restore_status;

static void restore_update(lv_event_t *e)
{
    char norm[300];
    wallet_mnemonic_normalise(lv_textarea_get_text(s_restore_ta), norm, sizeof(norm));
    int n = norm[0] ? 1 : 0;
    for (char *p = norm; *p; p++) {
        n += (*p == ' ');
    }
    bool valid = (n == 12 || n == 15 || n == 18 || n == 21 || n == 24) && wallet_mnemonic_valid(norm);
    memzero(norm, sizeof(norm));
    char st[64];
    snprintf(st, sizeof(st), "%d words  %s", n, valid ? LV_SYMBOL_OK " valid" : "");
    lv_label_set_text(s_restore_status, st);
    lv_obj_set_style_text_color(s_restore_status, valid ? COLOR_OK : COLOR_MUTED, 0);
}

static void on_restore_next(lv_event_t *e)
{
    char norm[300];
    wallet_mnemonic_normalise(lv_textarea_get_text(s_restore_ta), norm, sizeof(norm));
    if (!wallet_mnemonic_valid(norm) || strlen(norm) >= sizeof(s_mnemonic)) {
        memzero(norm, sizeof(norm));
        alert("Invalid phrase", "The words or checksum are not a valid BIP39 English phrase.");
        return;
    }
    strlcpy(s_mnemonic, norm, sizeof(s_mnemonic));
    memzero(norm, sizeof(norm));
    ta_wipe(s_restore_ta);
    show_password_create();
}

static void show_restore(void)
{
    form_t f = form_new();
    lv_obj_t *s = f.body;
    lv_obj_t *kb = f.kb;
    label(s, "Restore wallet", &lv_font_montserrat_20, COLOR_ACCENT);
    s_restore_ta = lv_textarea_create(s);
    lv_textarea_set_placeholder_text(s_restore_ta, "word1 word2 word3 ...");
    lv_obj_set_size(s_restore_ta, lv_pct(100), 100);
    lv_obj_add_event_cb(s_restore_ta, restore_update, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_restore_ta, kb_ta_event, LV_EVENT_CLICKED, kb);
    s_restore_status = label(s, "0 words", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_t *row = lv_obj_create(s);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *b1 = button(row, LV_SYMBOL_LEFT " Back", on_back_welcome, NULL);
    lv_obj_set_flex_grow(b1, 1);
    lv_obj_t *b2 = button(row, "Next " LV_SYMBOL_RIGHT, on_restore_next, NULL);
    lv_obj_set_flex_grow(b2, 1);
    lv_obj_add_event_cb(kb, on_restore_next, LV_EVENT_READY, NULL);
    form_focus(kb, s_restore_ta);
    form_show(&f);
}

/* ---- password (create) ---- */

typedef struct {
    char mnemonic[256];
    char pass[129];
    char bip39[257];
} create_req_t;

static lv_obj_t *s_pw1, *s_pw2, *s_pw3;

static void job_create(void *arg)
{
    create_req_t *r = arg;
    esp_err_t err = wallet_create(r->mnemonic, r->bip39, r->pass);
    memzero(r, sizeof(*r));
    free(r);
    if (err == ESP_OK) {
        account_reset();
    }
    if (ui_lock(-1)) {
        idle();
        if (err == ESP_OK) {
            show_main();
            alert("Wallet created", "Your encrypted wallet was saved to the SD card. "
                                    "Connect Wi-Fi in Settings, then Sync.");
        } else {
            char m[96];
            snprintf(m, sizeof(m), "Saving the wallet failed: %s", esp_err_to_name(err));
            alert("Error", m);
        }
        ui_unlock();
    }
}

static void on_password_save(lv_event_t *e)
{
    const char *a = lv_textarea_get_text(s_pw1);
    const char *b = lv_textarea_get_text(s_pw2);
    if (strlen(a) < 6) {
        alert("Password", "Use at least 6 characters (longer is much safer).");
        return;
    }
    if (strcmp(a, b) != 0) {
        alert("Password", "The passwords don't match.");
        return;
    }
    create_req_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return;
    }
    strlcpy(r->mnemonic, s_mnemonic, sizeof(r->mnemonic));
    strlcpy(r->pass, a, sizeof(r->pass));
    strlcpy(r->bip39, lv_textarea_get_text(s_pw3), sizeof(r->bip39));
    memzero(s_mnemonic, sizeof(s_mnemonic));
    ta_wipe(s_pw1);
    ta_wipe(s_pw2);
    ta_wipe(s_pw3);
    if (!start_job(job_create, r, "Encrypting wallet and saving to SD card...\nThis takes a few seconds.")) {
        memzero(r, sizeof(*r));
        free(r);
    }
}

/* Keyboard OK: move to the next field, save after the last one. */
static void on_password_kb_ready(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta == s_pw1) {
        form_focus(kb, s_pw2);
    } else if (ta == s_pw2) {
        form_focus(kb, s_pw3);
    } else {
        on_password_save(e);
    }
}

static void show_password_create(void)
{
    form_t f = form_new();
    lv_obj_t *s = f.body;
    label(s, "Protect your wallet", &lv_font_montserrat_20, COLOR_ACCENT);
    wrap_label(s, "This password encrypts the wallet file on the SD card.", &lv_font_montserrat_12);
    s_pw1 = textarea(s, "Password (min 6)", true, f.kb);
    s_pw2 = textarea(s, "Repeat password", true, f.kb);
    lv_obj_t *save = button(s, LV_SYMBOL_SAVE "  Save wallet to SD", on_password_save, NULL);
    lv_obj_set_style_bg_color(save, COLOR_ACCENT, 0);
    wrap_label(s, "Optional BIP39 passphrase (\"25th word\"). Leave empty unless you know you need "
                  "it - losing it means losing the funds.", &lv_font_montserrat_12);
    s_pw3 = textarea(s, "BIP39 passphrase (optional)", true, f.kb);
    lv_obj_add_event_cb(f.kb, on_password_kb_ready, LV_EVENT_READY, NULL);
    form_focus(f.kb, s_pw1);
    form_show(&f);
}

/* ======================================================== unlock */

static lv_obj_t *s_unlock_ta, *s_unlock_msg;

static void job_unlock(void *arg)
{
    char *pw = arg;
    esp_err_t err = wallet_unlock(pw);
    memzero(pw, strlen(pw));
    free(pw);
    if (err == ESP_OK) {
        account_reset();
    }
    if (ui_lock(-1)) {
        idle();
        if (err == ESP_OK) {
            show_main();
        } else if (s_unlock_msg) {
            char m[128];
            if (err == ESP_ERR_INVALID_CRC) {
                snprintf(m, sizeof(m), "Wrong password");
            } else if (err == ESP_ERR_INVALID_STATE) {
                int w = wallet_unlock_wait_seconds();
                snprintf(m, sizeof(m), w ? "Too many attempts - wait %d s" :
                                           "Device secret missing (wallet bound to another board?)", w);
            } else {
                snprintf(m, sizeof(m), "Cannot read wallet: %s", esp_err_to_name(err));
            }
            lv_label_set_text(s_unlock_msg, m);
        }
        ui_unlock();
    }
}

static void on_unlock(lv_event_t *e)
{
    int w = wallet_unlock_wait_seconds();
    if (w > 0) {
        char m[64];
        snprintf(m, sizeof(m), "Too many attempts - wait %d s", w);
        lv_label_set_text(s_unlock_msg, m);
        return;
    }
    char *pw = strdup(lv_textarea_get_text(s_unlock_ta));
    ta_wipe(s_unlock_ta);
    if (!pw) {
        return;
    }
    if (!start_job(job_unlock, pw, "Unlocking...\n(deriving key, ~5 s)")) {
        memzero(pw, strlen(pw));
        free(pw);
    }
}

static void show_unlock(void)
{
    form_t f = form_new();
    lv_obj_t *s = f.body;
    label(s, LV_SYMBOL_EYE_CLOSE "  Wallet locked", &lv_font_montserrat_20, COLOR_ACCENT);
    char net[64];
    snprintf(net, sizeof(net), "%s  |  SD: %s", wallet_network_name(), WALLET_FILE + 7);
    label(s, net, &lv_font_montserrat_12, COLOR_MUTED);
    s_unlock_ta = textarea(s, "Password", true, f.kb);
    lv_obj_t *ok = button(s, LV_SYMBOL_OK "  Unlock", on_unlock, NULL);
    lv_obj_set_style_bg_color(ok, COLOR_ACCENT, 0);
    s_unlock_msg = label(s, "", &lv_font_montserrat_14, COLOR_ERR);
    lv_obj_add_event_cb(f.kb, on_unlock, LV_EVENT_READY, NULL);
    form_focus(f.kb, s_unlock_ta);
    form_show(&f);
}

/* ======================================================== password prompt */

static pw_cb_t s_prompt_cb;
static lv_obj_t *s_prompt_ta;

static void on_prompt_ok(lv_event_t *e)
{
    char pw[129];
    strlcpy(pw, lv_textarea_get_text(s_prompt_ta), sizeof(pw));
    ta_wipe(s_prompt_ta);
    pw_cb_t cb = s_prompt_cb;
    s_prompt_cb = NULL;
    if (cb) {
        cb(pw);
    }
    memzero(pw, sizeof(pw));
}

static void on_prompt_cancel(lv_event_t *e)
{
    ta_wipe(s_prompt_ta);
    s_prompt_cb = NULL;
    show_main();
}

static void password_prompt(const char *title, const char *info, pw_cb_t cb)
{
    s_prompt_cb = cb;
    form_t f = form_new();
    lv_obj_t *s = f.body;
    label(s, title, &lv_font_montserrat_20, COLOR_ACCENT);
    if (info) {
        wrap_label(s, info, &lv_font_montserrat_14);
    }
    s_prompt_ta = textarea(s, "Wallet password", true, f.kb);
    lv_obj_t *ok = button(s, LV_SYMBOL_OK "  OK", on_prompt_ok, NULL);
    lv_obj_set_style_bg_color(ok, COLOR_ACCENT, 0);
    button(s, LV_SYMBOL_CLOSE "  Cancel", on_prompt_cancel, NULL);
    lv_obj_add_event_cb(f.kb, on_prompt_ok, LV_EVENT_READY, NULL);
    form_focus(f.kb, s_prompt_ta);
    form_show(&f);
}

/* ======================================================== main: home */

static void job_sync(void *arg)
{
    if (wifi_mgr_status() != WIFI_MGR_CONNECTED) {
        progress_cb("Waiting for Wi-Fi...");
        wifi_mgr_wait_connected(10000);
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (wifi_mgr_status() == WIFI_MGR_CONNECTED) {
        err = account_sync(progress_cb);
    }
    if (ui_lock(-1)) {
        idle();
        if (wifi_mgr_status() != WIFI_MGR_CONNECTED) {
            alert("No network", "Connect to Wi-Fi first (Settings tab).");
        } else if (err != ESP_OK) {
            char m[128];
            snprintf(m, sizeof(m), "Could not reach %s\n(%s)", CONFIG_BTC_ESPLORA_URL, esp_err_to_name(err));
            alert("Sync failed", m);
        }
        main_refresh();
        ui_unlock();
    }
}

static void on_sync(lv_event_t *e)
{
    start_job(job_sync, NULL, "Syncing with the blockchain...");
}

static void on_lock(lv_event_t *e)
{
    do_lock();
}

static void build_home(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tab, 10, 0);
    label(tab, "Balance", &lv_font_montserrat_16, COLOR_MUTED);
    s_bal_btc = label(tab, "-", &lv_font_montserrat_28, lv_color_white());
    s_bal_sub = label(tab, "", &lv_font_montserrat_14, COLOR_MUTED);
    s_sync_lbl = label(tab, "Not synced yet", &lv_font_montserrat_14, COLOR_MUTED);
    button(tab, LV_SYMBOL_REFRESH "  Sync", on_sync, NULL);
    s_fp_lbl = label(tab, "", &lv_font_montserrat_12, COLOR_MUTED);
    button(tab, LV_SYMBOL_EYE_CLOSE "  Lock", on_lock, NULL);
}

/* ======================================================== main: receive */

static void receive_refresh(void)
{
    uint32_t idx = account_receive_index();
    char addr[WALLET_ADDR_MAX];
    if (wallet_address(WALLET_CHAIN_RECEIVE, idx, addr, sizeof(addr)) != ESP_OK) {
        lv_label_set_text(s_rx_addr, "(locked)");
        return;
    }
    char uri[120];
    snprintf(uri, sizeof(uri), "bitcoin:%s", addr);
    lv_qrcode_update(s_rx_qr, uri, strlen(uri));
    lv_label_set_text(s_rx_addr, addr);
    char path[64];
    snprintf(path, sizeof(path), "m/84'/%d'/0'/0/%u", wallet_is_testnet() ? 1 : 0, (unsigned)idx);
    lv_label_set_text(s_rx_path, path);
}

static void on_new_address(lv_event_t *e)
{
    account_summary_t sum;
    account_summary(&sum);
    uint32_t idx = account_receive_index() + 1;
    if (idx >= sum.next_receive + CONFIG_BTC_GAP_LIMIT) {
        alert("Gap limit", "Too many unused addresses in a row - other wallets restoring this "
                           "phrase may not find funds sent there. Reuse an earlier address or sync.");
        return;
    }
    account_set_receive_index(idx);
    receive_refresh();
}

static void build_receive(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tab, 8, 0);
    label(tab, "Receive bitcoin", &lv_font_montserrat_16, COLOR_MUTED);
    s_rx_qr = lv_qrcode_create(tab);
    lv_qrcode_set_size(s_rx_qr, 210);
    lv_qrcode_set_dark_color(s_rx_qr, lv_color_black());
    lv_qrcode_set_light_color(s_rx_qr, lv_color_white());
    lv_obj_set_style_border_color(s_rx_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_rx_qr, 6, 0);
    s_rx_addr = wrap_label(tab, "", &lv_font_montserrat_14);
    lv_obj_set_style_text_align(s_rx_addr, LV_TEXT_ALIGN_CENTER, 0);
    s_rx_path = label(tab, "", &lv_font_montserrat_12, COLOR_MUTED);
    button(tab, LV_SYMBOL_PLUS "  New address", on_new_address, NULL);
}

/* ======================================================== main: send */

static void job_broadcast(void *arg)
{
    char txid[65] = {0}, msg[256];
    esp_err_t err = account_sign_and_broadcast(s_plan, txid, msg, sizeof(msg));
    if (s_plan) {
        memzero(s_plan, sizeof(*s_plan));
    }
    if (err == ESP_OK) {
        progress_cb("Broadcast OK - refreshing...");
        account_sync(progress_cb);
    }
    if (ui_lock(-1)) {
        idle();
        free(s_plan);
        s_plan = NULL;
        show_main();
        if (err == ESP_OK) {
            char m[160];
            snprintf(m, sizeof(m), "Transaction broadcast.\n\nTXID:\n%s", txid);
            alert(LV_SYMBOL_OK " Sent", m);
            ta_wipe(s_send_addr);
            ta_wipe(s_send_amt);
        } else {
            alert("Send failed", msg);
        }
        main_refresh();
        ui_unlock();
    }
}

static void on_confirm_send(lv_event_t *e)
{
    start_job(job_broadcast, NULL, "Signing and broadcasting...");
}

static void on_cancel_send(lv_event_t *e)
{
    if (s_plan) {
        memzero(s_plan, sizeof(*s_plan));
        free(s_plan);
        s_plan = NULL;
    }
    show_main();
}

static void show_confirm(void)
{
    lv_obj_t *s = scr_new();
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    label(s, "Confirm transaction", &lv_font_montserrat_20, COLOR_ACCENT);
    char a[40], f[40], c[40], t[40], buf[200];

    label(s, "To:", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_t *dst = wrap_label(s, s_plan->dest_addr, &lv_font_montserrat_16);
    lv_obj_set_style_text_color(dst, COLOR_ACCENT, 0);

    fmt_btc(s_plan->amount, a, sizeof(a));
    fmt_btc(s_plan->fee, f, sizeof(f));
    fmt_btc(s_plan->amount + s_plan->fee, t, sizeof(t));
    snprintf(buf, sizeof(buf), "Amount:  %s BTC\nFee:     %s BTC\n            (%lld sat, %.1f sat/vB, %u vB)\n"
             "Total:   %s BTC",
             a, f, (long long)s_plan->fee, s_plan->fee_rate, (unsigned)s_plan->vsize, t);
    wrap_label(s, buf, &lv_font_montserrat_16);

    if (s_plan->has_change) {
        fmt_btc(s_plan->outputs[1].value, c, sizeof(c));
        snprintf(buf, sizeof(buf), "Change back to you: %s BTC (m/84'/../1/%u)\nInputs: %d",
                 c, (unsigned)s_plan->change_index, s_plan->n_inputs);
    } else {
        snprintf(buf, sizeof(buf), "No change output. Inputs: %d", s_plan->n_inputs);
    }
    lv_obj_t *l = wrap_label(s, buf, &lv_font_montserrat_12);
    lv_obj_set_style_text_color(l, COLOR_MUTED, 0);
    if (wallet_is_testnet()) {
        label(s, "TESTNET coins", &lv_font_montserrat_14, COLOR_OK);
    }

    lv_obj_t *ok = button(s, LV_SYMBOL_UPLOAD "  Sign & Send", on_confirm_send, NULL);
    lv_obj_set_style_bg_color(ok, COLOR_ACCENT, 0);
    button(s, LV_SYMBOL_CLOSE "  Cancel", on_cancel_send, NULL);
    go(s);
}

static void on_review(lv_event_t *e)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    account_summary_t sum;
    account_summary(&sum);
    if (!sum.synced) {
        alert("Not synced", "Sync the wallet first (Home tab) so the available coins are known.");
        return;
    }
    const char *addr = lv_textarea_get_text(s_send_addr);
    uint8_t script[BTC_MAX_SCRIPT];
    size_t sl;
    if (btc_address_to_script(addr, script, &sl) != ESP_OK) {
        alert("Address", wallet_is_testnet() ? "Not a valid testnet address (tb1... / m / n / 2)."
                                             : "Not a valid bitcoin address (bc1... / 1 / 3).");
        return;
    }
    bool all = lv_obj_has_state(s_send_all, LV_STATE_CHECKED);
    int64_t amount = -1;
    if (!all && !btc_parse_amount(lv_textarea_get_text(s_send_amt), &amount)) {
        alert("Amount", "Enter the amount in BTC, e.g. 0.0005");
        return;
    }
    double rates[3] = { sum.fee_fast, sum.fee_normal, sum.fee_slow };
    double rate = rates[lv_dropdown_get_selected(s_send_fee) % 3];

    btc_tx_plan_t *plan = calloc(1, sizeof(*plan));
    if (!plan) {
        alert("Error", "Out of memory");
        return;
    }
    esp_err_t err = account_plan_send(addr, amount, rate, plan);
    if (err != ESP_OK) {
        free(plan);
        const char *m = err == ESP_ERR_INVALID_SIZE ? "Insufficient funds for amount + network fee." :
                        err == ESP_ERR_INVALID_ARG ? "Amount below dust limit (546 sat)." :
                        err == ESP_ERR_INVALID_STATE ? "Fee would be unreasonably high - refusing." :
                        "Could not build the transaction.";
        alert("Cannot send", m);
        return;
    }
    if (s_plan) {
        free(s_plan);
    }
    s_plan = plan;
    show_confirm();
}

static void on_send_all(lv_event_t *e)
{
    bool all = lv_obj_has_state(s_send_all, LV_STATE_CHECKED);
    if (all) {
        lv_obj_add_state(s_send_amt, LV_STATE_DISABLED);
        lv_textarea_set_text(s_send_amt, "");
        lv_textarea_set_placeholder_text(s_send_amt, "Entire balance minus fee");
    } else {
        lv_obj_remove_state(s_send_amt, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(s_send_amt, "Amount in BTC (e.g. 0.001)");
    }
}

static void build_send(lv_obj_t *tab, lv_obj_t *scr)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 8, 0);
    lv_obj_set_style_pad_bottom(tab, 210, 0);   /* room to scroll above the keyboard */
    s_kb = keyboard(scr, true);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_hide_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_hide_event, LV_EVENT_CANCEL, NULL);

    label(tab, "Recipient address", &lv_font_montserrat_14, COLOR_MUTED);
    s_send_addr = textarea(tab, wallet_is_testnet() ? "tb1..." : "bc1...", false, s_kb);
    lv_textarea_set_max_length(s_send_addr, 90);

    label(tab, "Amount (BTC)", &lv_font_montserrat_14, COLOR_MUTED);
    s_send_amt = textarea(tab, "Amount in BTC (e.g. 0.001)", false, s_kb);
    lv_textarea_set_accepted_chars(s_send_amt, "0123456789.");
    lv_textarea_set_max_length(s_send_amt, 16);

    s_send_all = lv_checkbox_create(tab);
    lv_checkbox_set_text(s_send_all, "Send entire balance");
    lv_obj_add_event_cb(s_send_all, on_send_all, LV_EVENT_VALUE_CHANGED, NULL);

    label(tab, "Network fee", &lv_font_montserrat_14, COLOR_MUTED);
    s_send_fee = lv_dropdown_create(tab);
    lv_dropdown_set_options(s_send_fee, "Fast (~20 min)\nNormal (~1 h)\nSlow (~1 day)");
    lv_dropdown_set_selected(s_send_fee, 1);
    lv_obj_set_width(s_send_fee, lv_pct(100));
    s_send_fee_lbl = label(tab, "Sync to load fee rates", &lv_font_montserrat_12, COLOR_MUTED);

    lv_obj_t *b = button(tab, LV_SYMBOL_RIGHT "  Review", on_review, NULL);
    lv_obj_set_style_bg_color(b, COLOR_ACCENT, 0);
}

/* ======================================================== main: history */

static void history_refresh(void)
{
    lv_obj_clean(s_hist_list);
    account_tx_t *h = malloc(sizeof(account_tx_t) * ACCOUNT_MAX_HISTORY);
    if (!h) {
        return;
    }
    int n = account_history(h, ACCOUNT_MAX_HISTORY);
    account_summary_t sum;
    account_summary(&sum);
    if (n == 0) {
        lv_list_add_text(s_hist_list, sum.synced ? "No transactions yet" : "Sync to load history");
    }
    for (int i = 0; i < n; i++) {
        char amt[32], line[160];
        fmt_btc(h[i].net, amt, sizeof(amt));
        uint32_t conf = (h[i].confirmed && sum.tip_height >= h[i].height) ? sum.tip_height - h[i].height + 1 : 0;
        if (h[i].confirmed) {
            snprintf(line, sizeof(line), "%s%s BTC\n%u conf  |  %.10s...", h[i].net >= 0 ? "+" : "", amt,
                     (unsigned)conf, h[i].txid);
        } else {
            snprintf(line, sizeof(line), "%s%s BTC\nunconfirmed  |  %.10s...", h[i].net >= 0 ? "+" : "", amt,
                     h[i].txid);
        }
        lv_obj_t *btn = lv_list_add_button(s_hist_list, h[i].net >= 0 ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_UPLOAD, line);
        lv_obj_set_style_text_color(btn, h[i].net >= 0 ? COLOR_OK : COLOR_ACCENT, 0);
    }
    free(h);
}

static void build_history(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 4, 0);
    s_hist_list = lv_list_create(tab);
    lv_obj_set_size(s_hist_list, lv_pct(100), lv_pct(100));
}

/* ======================================================== main: settings */

static void show_wifi(void);
static void on_wifi(lv_event_t *e) { show_wifi(); }

static void on_xpub(lv_event_t *e)
{
    char xpub[128];
    if (wallet_account_xpub(xpub, sizeof(xpub)) != ESP_OK) {
        return;
    }
    lv_obj_t *s = scr_new();
    label(s, "Account public key", &lv_font_montserrat_20, COLOR_ACCENT);
    wrap_label(s, "Watch-only key (BIP84). Import into Sparrow / BlueWallet to watch balances. "
                  "It cannot spend, but reveals your whole history.", &lv_font_montserrat_12);
    lv_obj_t *qr = lv_qrcode_create(s);
    lv_qrcode_set_size(qr, 220);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_qrcode_update(qr, xpub, strlen(xpub));
    wrap_label(s, xpub, &lv_font_montserrat_12);
    char fp[64];
    snprintf(fp, sizeof(fp), "Fingerprint %s  m/84'/%d'/0'", wallet_fingerprint_hex(), wallet_is_testnet() ? 1 : 0);
    label(s, fp, &lv_font_montserrat_12, COLOR_MUTED);
    button(s, LV_SYMBOL_LEFT "  Back", on_back_main, NULL);
    go(s);
}

static void reveal_cb(const char *pw)
{
    busy("Checking password...");
    lv_refr_now(NULL);
    esp_err_t err = wallet_reveal_mnemonic(pw, s_mnemonic, sizeof(s_mnemonic));
    idle();
    if (err != ESP_OK) {
        show_main();
        alert("Wrong password", "The recovery phrase was not shown.");
        return;
    }
    show_words(false);
}

static void on_reveal(lv_event_t *e)
{
    password_prompt("Show recovery phrase", "Make sure nobody is watching the screen.", reveal_cb);
}

static void wipe_cb(const char *pw)
{
    busy("Deleting wallet...");
    lv_refr_now(NULL);
    esp_err_t err = wallet_wipe(pw);
    idle();
    if (err == ESP_OK) {
        account_reset();
        lv_obj_t *m = s_main;
        s_main = NULL;
        route();
        if (m) {
            lv_obj_delete_async(m);
        }
        alert("Deleted", "The wallet file was removed from the SD card.");
    } else {
        show_main();
        alert("Not deleted", err == ESP_ERR_INVALID_CRC ? "Wrong password." : "Could not delete the file.");
    }
}

static void on_wipe(lv_event_t *e)
{
    password_prompt("Delete wallet from SD",
                    "This erases wallet.dat. Without your written recovery phrase the funds are LOST.",
                    wipe_cb);
}

static void ota_progress(int pct)
{
    char m[48];
    snprintf(m, sizeof(m), "Updating firmware... %d%%", pct);
    progress_cb(m);
}

static void job_ota(void *arg)
{
    esp_err_t err = sd_update_apply(ota_progress);   /* reboots on success */
    if (ui_lock(-1)) {
        idle();
        char m[96];
        snprintf(m, sizeof(m), "Update failed: %s", esp_err_to_name(err));
        alert("Firmware", m);
        ui_unlock();
    }
}

static void on_ota_confirm(lv_event_t *e)
{
    lv_obj_t *mb = lv_event_get_user_data(e);
    lv_msgbox_close_async(mb);
    start_job(job_ota, NULL, "Updating firmware...");
}

static void on_ota(lv_event_t *e)
{
    char ver[80];
    if (!sd_update_available(ver, sizeof(ver))) {
        alert("Firmware", "Put a valid app image at /firmware.bin on the SD card.");
        return;
    }
    char m[200];
    const esp_app_desc_t *cur = esp_app_get_description();
    snprintf(m, sizeof(m), "Running: %s\nOn SD: %s\n\nInstall and reboot?", cur->version, ver);
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, "Update from SD");
    lv_msgbox_add_text(mb, m);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "Install");
    lv_obj_add_event_cb(ok, on_ota_confirm, LV_EVENT_CLICKED, mb);
    lv_msgbox_add_close_button(mb);
    lv_obj_set_width(mb, 300);
}

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    board_backlight_set((uint8_t)lv_slider_get_value(sl));
}

static void on_about(lv_event_t *e)
{
    char m[320];
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(m, sizeof(m), "Firmware %s\nIDF %s\nNetwork: %s\nAPI: %s\nWallet: %s\nFingerprint: %s",
             d->version, d->idf_ver, wallet_network_name(), CONFIG_BTC_ESPLORA_URL, WALLET_FILE,
             wallet_fingerprint_hex());
    alert("About", m);
}

static void build_settings(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, 4, 0);
    lv_obj_t *list = lv_list_create(tab);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_list_add_text(list, "Network");
    s_wifi_item = lv_list_add_button(list, LV_SYMBOL_WIFI, "Wi-Fi");
    lv_obj_add_event_cb(s_wifi_item, on_wifi, LV_EVENT_CLICKED, NULL);
    lv_list_add_text(list, "Wallet");
    lv_obj_add_event_cb(lv_list_add_button(list, LV_SYMBOL_EYE_OPEN, "Show account xpub"), on_xpub,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_list_add_button(list, LV_SYMBOL_WARNING, "Show recovery phrase"), on_reveal,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_list_add_button(list, LV_SYMBOL_EYE_CLOSE, "Lock now"), on_lock,
                        LV_EVENT_CLICKED, NULL);
    lv_list_add_text(list, "Device");
    lv_obj_t *br = lv_list_add_button(list, LV_SYMBOL_IMAGE, "Brightness");
    lv_obj_t *sl = lv_slider_create(tab);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, 80, LV_ANIM_OFF);
    lv_obj_set_width(sl, lv_pct(90));
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_parent(sl, list);
    lv_obj_move_to_index(sl, lv_obj_get_index(br) + 1);
    lv_obj_add_event_cb(lv_list_add_button(list, LV_SYMBOL_DOWNLOAD, "Update firmware from SD"), on_ota,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_list_add_button(list, LV_SYMBOL_LIST, "About"), on_about, LV_EVENT_CLICKED, NULL);
    lv_list_add_text(list, "Danger zone");
    lv_obj_t *w = lv_list_add_button(list, LV_SYMBOL_TRASH, "Delete wallet from SD");
    lv_obj_set_style_text_color(w, COLOR_ERR, 0);
    lv_obj_add_event_cb(w, on_wipe, LV_EVENT_CLICKED, NULL);
}

/* ======================================================== Wi-Fi screen */


static void job_scan(void *arg)
{
    wifi_mgr_ap_t *aps = calloc(16, sizeof(wifi_mgr_ap_t));
    int n = aps ? wifi_mgr_scan(aps, 16) : 0;
    if (ui_lock(-1)) {
        idle();
        if (s_wifi_dd && lv_obj_is_valid(s_wifi_dd)) {
            lv_dropdown_clear_options(s_wifi_dd);
            for (int i = 0; i < n; i++) {
                lv_dropdown_add_option(s_wifi_dd, aps[i].ssid, LV_DROPDOWN_POS_LAST);
            }
            if (n == 0) {
                lv_dropdown_add_option(s_wifi_dd, "(no networks found)", LV_DROPDOWN_POS_LAST);
            }
        }
        ui_unlock();
    }
    free(aps);
}

static void on_scan(lv_event_t *e)
{
    start_job(job_scan, NULL, "Scanning Wi-Fi...");
}

static void on_wifi_connect(lv_event_t *e)
{
    char ssid[33];
    lv_dropdown_get_selected_str(s_wifi_dd, ssid, sizeof(ssid));
    if (!ssid[0] || ssid[0] == '(') {
        alert("Wi-Fi", "Scan and pick a network first.");
        return;
    }
    wifi_mgr_connect(ssid, lv_textarea_get_text(s_wifi_pw), true);
    ta_wipe(s_wifi_pw);
    lv_label_set_text(s_wifi_state, "Connecting...");
}

static void on_wifi_forget(lv_event_t *e)
{
    wifi_mgr_forget();
    lv_label_set_text(s_wifi_state, "Saved network removed");
}

static void show_wifi(void)
{
    form_t f = form_new();
    lv_obj_t *s = f.body;
    label(s, LV_SYMBOL_WIFI "  Wi-Fi", &lv_font_montserrat_20, COLOR_ACCENT);
    s_wifi_state = label(s, "", &lv_font_montserrat_14, COLOR_MUTED);
    s_wifi_dd = lv_dropdown_create(s);
    lv_obj_set_width(s_wifi_dd, lv_pct(100));
    lv_dropdown_set_options(s_wifi_dd, wifi_mgr_ssid()[0] ? wifi_mgr_ssid() : "(scan first)");
    s_wifi_pw = textarea(s, "Wi-Fi password", true, f.kb);
    lv_obj_t *ok = button(s, LV_SYMBOL_OK "  Connect & save", on_wifi_connect, NULL);
    lv_obj_set_style_bg_color(ok, COLOR_ACCENT, 0);
    button(s, LV_SYMBOL_REFRESH "  Scan again", on_scan, NULL);
    button(s, LV_SYMBOL_TRASH "  Forget saved network", on_wifi_forget, NULL);
    button(s, LV_SYMBOL_LEFT "  Back", on_back_main, NULL);
    lv_obj_add_event_cb(f.kb, on_wifi_connect, LV_EVENT_READY, NULL);
    form_focus(f.kb, s_wifi_pw);
    form_show(&f);
    start_job(job_scan, NULL, "Scanning Wi-Fi...");
}

/* ======================================================== main screen */

static void on_tab_changed(lv_event_t *e)
{
    uint32_t t = lv_tabview_get_tab_active(s_tv);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    if (t == 1) {
        receive_refresh();
    } else if (t == 3) {
        history_refresh();
    }
}

static void status_refresh(void)
{
    if (!s_main) {
        return;
    }
    wifi_mgr_status_t ws = wifi_mgr_status();
    lv_label_set_text(s_status_wifi, ws == WIFI_MGR_CONNECTED ? LV_SYMBOL_WIFI :
                                     ws == WIFI_MGR_CONNECTING ? LV_SYMBOL_WIFI "..." : LV_SYMBOL_WIFI " off");
    lv_obj_set_style_text_color(s_status_wifi, ws == WIFI_MGR_CONNECTED ? COLOR_OK : COLOR_MUTED, 0);

    int mv = board_battery_mv();
    int pct = board_battery_percent(mv);
    char b[32];
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 30 ? LV_SYMBOL_BATTERY_2 : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    if (pct < 0) {
        snprintf(b, sizeof(b), LV_SYMBOL_USB);
    } else {
        snprintf(b, sizeof(b), "%s%s %d%%", board_is_charging() ? LV_SYMBOL_CHARGE : "", sym, pct);
    }
    lv_label_set_text(s_status_bat, b);

    if (s_wifi_item) {
        lv_obj_t *l = lv_obj_get_child(s_wifi_item, 1);
        if (l) {
            char t[64];
            snprintf(t, sizeof(t), "Wi-Fi: %s", ws == WIFI_MGR_CONNECTED ? wifi_mgr_ssid() :
                                               ws == WIFI_MGR_CONNECTING ? "connecting..." : "not connected");
            lv_label_set_text(l, t);
        }
    }
    if (s_wifi_state && lv_obj_is_valid(s_wifi_state) && lv_screen_active() == lv_obj_get_screen(s_wifi_state)) {
        lv_label_set_text(s_wifi_state, ws == WIFI_MGR_CONNECTED ? "Connected" :
                                        ws == WIFI_MGR_CONNECTING ? "Connecting..." :
                                        ws == WIFI_MGR_FAILED ? "Connection failed" : "Not connected");
    }
}

static void main_refresh(void)
{
    if (!s_main) {
        return;
    }
    account_summary_t sum;
    account_summary(&sum);
    char a[32], u[32], t[96];
    if (sum.synced) {
        fmt_btc(sum.confirmed + sum.unconfirmed, a, sizeof(a));
        snprintf(t, sizeof(t), "%s BTC", a);
        lv_label_set_text(s_bal_btc, t);
        fmt_btc(sum.unconfirmed, u, sizeof(u));
        snprintf(t, sizeof(t), "%lld sat%s%s%s", (long long)(sum.confirmed + sum.unconfirmed),
                 sum.unconfirmed ? "  |  pending " : "", sum.unconfirmed ? u : "", sum.unconfirmed ? " BTC" : "");
        lv_label_set_text(s_bal_sub, t);
        int64_t ago = esp_timer_get_time() / 1000000 - sum.synced_at_s;
        snprintf(t, sizeof(t), "Synced %llds ago  |  block %u", (long long)ago, (unsigned)sum.tip_height);
        lv_label_set_text(s_sync_lbl, t);
        snprintf(t, sizeof(t), "Fee rates: fast %.1f / normal %.1f / slow %.1f sat/vB",
                 sum.fee_fast, sum.fee_normal, sum.fee_slow);
        lv_label_set_text(s_send_fee_lbl, t);
    }
    snprintf(t, sizeof(t), "%s  |  fingerprint %s", wallet_network_name(), wallet_fingerprint_hex());
    lv_label_set_text(s_fp_lbl, t);
    receive_refresh();
    history_refresh();
    status_refresh();
}

static void build_main(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s, 0, 0);
    s_main = s;

    /* status bar */
    lv_obj_t *bar = lv_obj_create(s);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 26);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_status_net = label(bar, wallet_network_name(), &lv_font_montserrat_14,
                         wallet_is_testnet() ? COLOR_OK : COLOR_ACCENT);
    lv_obj_align(s_status_net, LV_ALIGN_LEFT_MID, 0, 0);
    s_status_wifi = label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_status_wifi, LV_ALIGN_CENTER, 0, 0);
    s_status_bat = label(bar, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_align(s_status_bat, LV_ALIGN_RIGHT_MID, 0, 0);

    s_tv = lv_tabview_create(s);
    lv_tabview_set_tab_bar_position(s_tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tv, 48);
    lv_obj_set_size(s_tv, lv_pct(100), BOARD_LCD_V_RES - 26);
    lv_obj_align(s_tv, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *t_home = lv_tabview_add_tab(s_tv, LV_SYMBOL_HOME);
    lv_obj_t *t_rx = lv_tabview_add_tab(s_tv, LV_SYMBOL_DOWNLOAD);
    lv_obj_t *t_tx = lv_tabview_add_tab(s_tv, LV_SYMBOL_UPLOAD);
    lv_obj_t *t_hist = lv_tabview_add_tab(s_tv, LV_SYMBOL_LIST);
    lv_obj_t *t_set = lv_tabview_add_tab(s_tv, LV_SYMBOL_SETTINGS);
    lv_obj_t *bar_btns = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_text_font(bar_btns, &lv_font_montserrat_20, 0);

    build_home(t_home);
    build_receive(t_rx);
    build_send(t_tx, s);
    build_history(t_hist);
    build_settings(t_set);
    lv_obj_add_event_cb(s_tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

static void show_main(void)
{
    if (wallet_state() != WALLET_STATE_UNLOCKED) {
        route();
        return;
    }
    s_wifi_dd = NULL;
    s_wifi_state = NULL;
    if (!s_main) {
        build_main();
    }
    go(s_main);
    main_refresh();
}

static void do_lock(void)
{
    if (worker_busy()) {
        return;
    }
    wallet_lock();
    account_reset();
    if (s_plan) {
        memzero(s_plan, sizeof(*s_plan));
        free(s_plan);
        s_plan = NULL;
    }
    memzero(s_mnemonic, sizeof(s_mnemonic));
    lv_obj_t *m = s_main;
    bool main_active = (m == lv_screen_active());
    s_main = NULL;
    s_wifi_item = NULL;
    route();                        /* go() deletes the old screen if it was main */
    if (m && !main_active) {
        lv_obj_delete_async(m);
    }
}

/* ======================================================== routing + timers */

static void route(void)
{
    switch (wallet_refresh_state()) {
    case WALLET_STATE_NO_SD:
        show_no_sd();
        break;
    case WALLET_STATE_EMPTY:
        show_welcome();
        break;
    case WALLET_STATE_LOCKED:
        show_unlock();
        break;
    case WALLET_STATE_UNLOCKED:
        show_main();
        break;
    }
}

static void tick_timer(lv_timer_t *t)
{
    static int n;
    if (++n % 2 == 0) {
        status_refresh();
    }
#if CONFIG_WALLET_AUTOLOCK_SECONDS > 0
    if (wallet_state() == WALLET_STATE_UNLOCKED && !worker_busy() &&
        lv_display_get_inactive_time(NULL) > (uint32_t)CONFIG_WALLET_AUTOLOCK_SECONDS * 1000u) {
        ESP_LOGI(TAG, "auto-lock after inactivity");
        do_lock();
    }
#endif
}

void ui_start(void)
{
    ui_lock(-1);
    lv_obj_t *s = scr_new();
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(s, "SD Bitcoin Wallet", &lv_font_montserrat_28, COLOR_ACCENT);
    label(s, wallet_network_name(), &lv_font_montserrat_16, COLOR_MUTED);
    go(s);
    route();
    lv_timer_create(tick_timer, 1000, NULL);
    ui_unlock();
}
