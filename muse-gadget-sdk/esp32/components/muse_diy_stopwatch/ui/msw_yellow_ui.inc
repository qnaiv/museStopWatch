/*
 * museStopWatch: Settings > Yellow button. Included by muse_settings_ui.c
 * after msw_avatar_ui.inc. Only built on the StopWatch.
 *
 * dev.16: only the yellow button's LONG press (1.5 s) is assignable —
 * a short press always switches the face between the avatar and the
 * clock (dev.14, fixed), and there is no double-click assignment. The
 * assignment itself lives in muse_ui.c (its NVS, beside face_mode);
 * the installed apps come from the mini-app store through a weak
 * reference, the Avatar page's way. The page is rebuilt on every open
 * (Settings keeps only home), so an app installed since the last open
 * is listed, and the checks are re-read from the live assignment.
 */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
/* msw_app_info_t's layout (miniapp/msw_app_priv.h), mirrored so the weak
 * declaration below links against msw_app_core.c's own. */
struct msw_yellow_app_info {
    char id[24];
    char name[32];
    uint16_t ver, api;
    uint32_t bytes;
    void *icon;
};
extern int msw_app_list(struct msw_yellow_app_info *out, int max) __attribute__((weak));

#define MSW_YELLOW_PAGE_MAX 16
static lv_obj_t *s_yellow_pg, *s_home_yellow;
static lv_obj_t *s_yellow_checks[MSW_YELLOW_PAGE_MAX + 4];
static struct msw_yellow_app_info s_yellow_infos[MSW_YELLOW_PAGE_MAX];
static int s_yellow_rows;   /* option rows built: 4 fixed, then the apps */
static int s_yellow_apps;   /* how many of the rows are apps */

/* One option row's action, by row: 0 the timer, 1 the pomodoro
 * (dev.22), 2 the face switch, 3 none, 4+i app i. A stored app that
 * has since been removed falls back to showing the timer — muse_ui
 * still holds its id, and the long press then just fails to start
 * it; picking any row rewrites the assignment cleanly. */
static int msw_yellow_current(void)
{
    int act = muse_ui_yellow_long_action();
    if (act == MUSE_YEL_LONG_POMODORO) return 1;
    if (act == MUSE_YEL_LONG_FACE) return 2;
    if (act == MUSE_YEL_LONG_NONE) return 3;
    if (act == MUSE_YEL_LONG_APP) {
        const char *id = muse_ui_yellow_long_app();
        for (int i = 0; i < s_yellow_apps; i++) {
            if (!strcmp(id, s_yellow_infos[i].id)) return 4 + i;
        }
    }
    return 0;
}

static void msw_tick_yellow(void)
{
    int cur = msw_yellow_current();
    for (int i = 0; i < s_yellow_rows; i++) {
        set_text(s_yellow_checks[i], i == cur ? LV_SYMBOL_OK : "");
    }
}

static void msw_on_yellow_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == 0) {
        muse_ui_set_yellow_long(MUSE_YEL_LONG_TIMER, "");
    } else if (idx == 1) {
        muse_ui_set_yellow_long(MUSE_YEL_LONG_POMODORO, "");
    } else if (idx == 2) {
        muse_ui_set_yellow_long(MUSE_YEL_LONG_FACE, "");
    } else if (idx == 3) {
        muse_ui_set_yellow_long(MUSE_YEL_LONG_NONE, "");
    } else {
        muse_ui_set_yellow_long(MUSE_YEL_LONG_APP, s_yellow_infos[idx - 4].id);
    }
    msw_tick_yellow();
}

static void msw_build_yellow_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_yellow_pg = page(tile, "YELLOW BUTTON", true, &list);
    note(list, "長押し（1.5秒）の動作です。短押しは顔と時計の切替で固定です。");
    int r = 0;
    row(list, NULL, "タイマー", &s_yellow_checks[r], msw_on_yellow_pick, (void *)(intptr_t)r);
    r++;
    row(list, NULL, "ポモドーロ", &s_yellow_checks[r], msw_on_yellow_pick, (void *)(intptr_t)r);
    r++;
    row(list, NULL, "顔・時計の切替", &s_yellow_checks[r], msw_on_yellow_pick, (void *)(intptr_t)r);
    r++;
    row(list, NULL, "なし", &s_yellow_checks[r], msw_on_yellow_pick, (void *)(intptr_t)r);
    r++;
    int n = msw_app_list ? msw_app_list(s_yellow_infos, MSW_YELLOW_PAGE_MAX) : 0;
    s_yellow_apps = n;
    for (int i = 0; i < n; i++) {
        row(list, NULL, s_yellow_infos[i].name[0] ? s_yellow_infos[i].name : s_yellow_infos[i].id,
            &s_yellow_checks[r], msw_on_yellow_pick, (void *)(intptr_t)r);
        r++;
    }
    s_yellow_rows = r;
    if (!n) {
        note(list, "ミニアプリはまだありません");
    }
    for (int i = 0; i < s_yellow_rows; i++) {
        lv_obj_set_style_text_color(s_yellow_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    msw_tick_yellow();
}

static const page_t MSW_YELLOW = { &s_yellow_pg, msw_build_yellow_page };
#endif   /* CONFIG_MUSE_BOARD_M5STACK_STOPWATCH */
