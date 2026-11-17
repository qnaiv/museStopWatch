/*
 * museStopWatch: Settings > Quiet hours. Included by
 * muse_settings_ui.c after msw_pomodoro_ui.inc. Only built on the
 * StopWatch.
 *
 * dev.26: the night window in which automatic network wakes stop —
 * no periodic sleep check, no dashboard fetch on wake — while local
 * alerts (the timer, the pomodoro) still fire and a voice turn the
 * user starts still works. The values live in muse_ui.c (its NVS,
 * beside the care state) behind muse_ui_quiet_config(); this page
 * cycles each value on a tap, the Pomodoro page's way of choosing:
 * the switch toggles, the times step 30 minutes.
 */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
static lv_obj_t *s_quiet_pg, *s_home_quiet;
static lv_obj_t *s_quiet_vals[3];

static void msw_tick_quiet(void)
{
    muse_quiet_cfg_t c;
    muse_ui_quiet_config(&c);
    set_text(s_quiet_vals[0], c.on ? "ON" : "OFF");
    char buf[32];   /* "%d:%02d" of two ints is 23 chars at the widest */
    snprintf(buf, sizeof(buf), "%d:%02d", c.start_min / 60, c.start_min % 60);
    set_text(s_quiet_vals[1], buf);
    snprintf(buf, sizeof(buf), "%d:%02d", c.end_min / 60, c.end_min % 60);
    set_text(s_quiet_vals[2], buf);
}

static void msw_on_quiet_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    muse_quiet_cfg_t c;
    muse_ui_quiet_config(&c);
    if (idx == 0) {
        c.on = !c.on;
    } else if (idx == 1) {
        c.start_min = (c.start_min + 30) % 1440;
    } else {
        c.end_min = (c.end_min + 30) % 1440;
    }
    muse_ui_set_quiet_config(&c);
    msw_tick_quiet();
}

static void msw_build_quiet_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_quiet_pg = page(tile, "QUIET HOURS", true, &list);
    note(list, "この時間帯は自動で起きて確認しに行かず、返信は時間帯が終わってから届きます。タイマーとポモドーロの通知は鳴ります。");
    row(list, NULL, "静かな時間", &s_quiet_vals[0], msw_on_quiet_pick, (void *)(intptr_t)0);
    row(list, NULL, "開始", &s_quiet_vals[1], msw_on_quiet_pick, (void *)(intptr_t)1);
    row(list, NULL, "終了", &s_quiet_vals[2], msw_on_quiet_pick, (void *)(intptr_t)2);
    note(list, "タップで変わります（時刻は30分刻み）。");
    msw_tick_quiet();
}

static const page_t MSW_QUIET = { &s_quiet_pg, msw_build_quiet_page };
#endif   /* CONFIG_MUSE_BOARD_M5STACK_STOPWATCH */
