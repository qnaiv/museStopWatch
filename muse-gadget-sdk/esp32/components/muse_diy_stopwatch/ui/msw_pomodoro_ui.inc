/*
 * museStopWatch: Settings > Pomodoro. Included by muse_settings_ui.c
 * after msw_yellow_ui.inc. Only built on the StopWatch.
 *
 * dev.22: the pomodoro's lengths — focus / break / long break in
 * minutes, and how many focuses make a long break. The values live
 * in muse_ui.c (its NVS, beside the care state) behind
 * muse_ui_pomodoro_config(); this page only cycles each value
 * through its presets on a tap, the Sleep page's way of choosing.
 * A change mid-session lands at the next phase: a phase's length is
 * read when the phase starts, never under a running countdown.
 */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH
static lv_obj_t *s_pomo_pg, *s_home_pomo;
static lv_obj_t *s_pomo_vals[4];

static const int MSW_POMO_PRESETS[4][6] = {
    { 15, 20, 25, 30, 45, 60 },   /* focus minutes */
    { 3, 5, 10, 15, 20, 0 },      /* break minutes */
    { 10, 15, 20, 30, 45, 0 },    /* long-break minutes */
    { 2, 3, 4, 5, 6, 8 },         /* focuses per round */
};
static const int MSW_POMO_PRESET_N[4] = { 6, 5, 5, 6 };

static int msw_pomo_field(const muse_pomo_cfg_t *c, int idx)
{
    return idx == 0 ? c->focus_min : idx == 1 ? c->break_min
           : idx == 2 ? c->long_min : c->sets;
}

static void msw_tick_pomodoro(void)
{
    muse_pomo_cfg_t c;
    muse_ui_pomodoro_config(&c);
    for (int i = 0; i < 4; i++) {
        if (i == 3) {
            set_val(s_pomo_vals[i], "%d回", msw_pomo_field(&c, i));
        } else {
            set_val(s_pomo_vals[i], "%d分", msw_pomo_field(&c, i));
        }
    }
}

static void msw_on_pomo_pick(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    muse_pomo_cfg_t c;
    muse_ui_pomodoro_config(&c);
    int cur = msw_pomo_field(&c, idx);
    int next = MSW_POMO_PRESETS[idx][0];
    for (int i = 0; i < MSW_POMO_PRESET_N[idx]; i++) {
        if (MSW_POMO_PRESETS[idx][i] == cur) {
            next = MSW_POMO_PRESETS[idx][(i + 1) % MSW_POMO_PRESET_N[idx]];
            break;
        }
        if (MSW_POMO_PRESETS[idx][i] > cur) {   /* an off-preset value steps up */
            next = MSW_POMO_PRESETS[idx][i];
            break;
        }
    }
    if (idx == 0) c.focus_min = next;
    else if (idx == 1) c.break_min = next;
    else if (idx == 2) c.long_min = next;
    else c.sets = next;
    muse_ui_set_pomodoro_config(&c);
    msw_tick_pomodoro();
}

static void msw_build_pomodoro_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_pomo_pg = page(tile, "POMODORO", true, &list);
    note(list, "タップで次の候補に変わります。実行中のフェーズには、次のフェーズから反映されます。");
    row(list, NULL, "集中", &s_pomo_vals[0], msw_on_pomo_pick, (void *)(intptr_t)0);
    row(list, NULL, "休憩", &s_pomo_vals[1], msw_on_pomo_pick, (void *)(intptr_t)1);
    row(list, NULL, "長い休憩", &s_pomo_vals[2], msw_on_pomo_pick, (void *)(intptr_t)2);
    row(list, NULL, "長い休憩までの集中", &s_pomo_vals[3], msw_on_pomo_pick, (void *)(intptr_t)3);
    msw_tick_pomodoro();
}

static const page_t MSW_POMODORO = { &s_pomo_pg, msw_build_pomodoro_page };
#endif   /* CONFIG_MUSE_BOARD_M5STACK_STOPWATCH */
