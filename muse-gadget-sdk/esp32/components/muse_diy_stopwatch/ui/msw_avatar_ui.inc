/*
 * museStopWatch: Settings > Avatar. Included by muse_settings_ui.c after
 * msw_wake_ui.inc (its page helpers and msw_jp_note).
 *
 * Avatar switching used to live on the tile below the face; the reply
 * history took that tile, so picking an avatar lives here now. The
 * avatar store (miniapp/msw_avatar.c) is reached through weak
 * references, so this page only shows when it is built in.
 */
/* msw_app_info_t's layout (miniapp/msw_app_priv.h), mirrored so the weak
 * declarations below link against msw_avatar.c's own. */
struct msw_avatar_info {
    char id[24];
    char name[32];
    uint16_t ver, api;
    uint32_t bytes;
    void *icon;
};
extern int msw_avatar_list(struct msw_avatar_info *out, int max) __attribute__((weak));
extern bool msw_avatar_use(const char *id, const char **code, char *msg, size_t msg_len) __attribute__((weak));
extern const char *msw_avatar_current(char *name, size_t name_len, float *avg_ms) __attribute__((weak));

#define MSW_AVATAR_PAGE_MAX 16
static lv_obj_t *s_avatar_pg, *s_home_avatar;
static lv_obj_t *s_avatar_checks[MSW_AVATAR_PAGE_MAX + 1];   /* the official one, then the rest */
static const char *s_avatar_ids[MSW_AVATAR_PAGE_MAX + 1];
static struct msw_avatar_info s_avatar_infos[MSW_AVATAR_PAGE_MAX];
static int s_avatar_rows;

static bool msw_avatar_built_in(void)
{
    return msw_avatar_list && msw_avatar_use && msw_avatar_current;
}

static void msw_tick_avatar(void)
{
    const char *cur = msw_avatar_current(NULL, 0, NULL);
    for (int i = 0; i < s_avatar_rows; i++) {
        set_text(s_avatar_checks[i], cur && !strcmp(cur, s_avatar_ids[i]) ? LV_SYMBOL_OK : "");
    }
}

static void msw_on_avatar_pick(lv_event_t *e)
{
    const char *code;
    char msg[160];
    msw_avatar_use((const char *)lv_event_get_user_data(e), &code, msg, sizeof(msg));
    msw_tick_avatar();
}

static void msw_build_avatar_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_avatar_pg = page(tile, "AVATAR", true, &list);
    int n = msw_avatar_list(s_avatar_infos, MSW_AVATAR_PAGE_MAX);
    int r = 0;
    s_avatar_ids[r] = "official";
    row(list, NULL, "公式", &s_avatar_checks[r], msw_on_avatar_pick, (void *)"official");
    lv_obj_set_style_text_color(s_avatar_checks[r], lv_color_hex(COLOR_ACCENT), 0);
    r++;
    for (int i = 0; i < n; i++) {
        s_avatar_ids[r] = s_avatar_infos[i].id;
        row(list, NULL, s_avatar_infos[i].name[0] ? s_avatar_infos[i].name : s_avatar_infos[i].id,
            &s_avatar_checks[r], msw_on_avatar_pick, (void *)s_avatar_infos[i].id);
        lv_obj_set_style_text_color(s_avatar_checks[r], lv_color_hex(COLOR_ACCENT), 0);
        r++;
    }
    s_avatar_rows = r;
    if (!n) {
        msw_jp_note(list, COLOR_DIM, "ほかのアバターは Muse に作ってもらえます");
    }
    msw_tick_avatar();
}

static const page_t MSW_AVATAR = { &s_avatar_pg, msw_build_avatar_page };
