/*
 * The menu preview's main() and its tests (see tools/preview-menu.py), last
 * in the generated program so it can call the menu's own static functions.
 *
 * The program takes one argument, the mode:
 *   self-test, browser-render, tools-return, fullscreen-render, gecko-playback,
 *   gecko-tuning-progress   run checks on the real menu code; any failure
 *                           calls abort(), which prints the line and exits 1
 *   font-measure-bench, idle-draw-bench, font-memory
 *                           measure and report costs
 *   anything else           a screen: set_up_screen() puts the menu in that
 *                           state and draw() prints its drawing commands
 */
static Menu *reading_menu;
static int reading_fail, reading_transition;
static int preview_read_directory(Ode *ode, OdeListing *listing)
{
    if (reading_transition) {
        int before = preview_primitives;
        if (reading_menu->listing.count != 4 || !reading_menu->loading_notice ||
            reading_menu->ready || strcmp(reading_menu->breadcrumb, "Root filtered") || before > 3)
            abort();
        preview_now += 100;
        ode->progress(ode->progress_context, 4);
        if (preview_primitives != before)
            abort(); /* Never redraw the old root at the new depth. */
        (void)listing;
        return 0;
    }
    /* Exercise the actual load/progress code while checking old ownership. */
    if (reading_menu->listing.count != 9 || !reading_menu->loading || reading_menu->ready ||
        !reading_menu->view_valid || strcmp(reading_menu->breadcrumb, "/ STORAGE"))
        abort();
    ode->progress(ode->progress_context, 1);
    if (reading_menu->listing.count != 9 || reading_menu->selected != 2)
        abort();
    (void)listing;
    if (reading_fail) {
        ode->error = "Fixture read failed";
        return -1;
    }
    return 0; /* successful empty directory */
}
/* tools-return: opening each tool from the Tools home and backing out returns to the
 * same home selection, including nested settings and close/reopen. */
static int run_tools_return(Menu *menu)
{
    static const int pages[] = {TOOL_SAVES, TOOL_FAVORITES, TOOL_CONFIG, TOOL_HELP,
                                TOOL_DIAG,  TOOL_RECENT,    TOOL_FIND};
    size_t selected;
    for (selected = 0; selected < 7; ++selected) {
        menu->tools = TOOL_HOME;
        menu->tool_selected = selected;
        draw(menu);
        draw(menu);
        tools_input(menu, ControlA);
        if (menu->tools != pages[selected] || menu->home_selected != selected ||
            menu->tool_selected)
            abort();
        draw(menu);
        draw(menu);
        if (selected == 2)
            menu->tool_selected = 11;
        else if (selected == 3)
            menu->tool_selected = 1;
        else if (selected == 4)
            menu->tool_selected = 4;
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_HOME || menu->tool_selected != selected)
            abort();
        preview_primitives = 0;
        draw(menu);
        if (!preview_primitives)
            abort();
        tools_input(menu, ControlB);
        draw(menu);
        toggle_tools(menu);
        if (menu->tools != TOOL_HOME || menu->tool_selected != selected)
            abort();
    }
    menu->tools = TOOL_HOME;
    menu->tool_selected = 2;
    tools_input(menu, ControlA);
    menu->tools = TOOL_CATEGORIES;
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_CONFIG || menu->tool_selected != 8)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_HOME || menu->tool_selected != 2)
        abort();
    tools_input(menu, ControlDown);
    toggle_tools(menu);
    draw(menu);
    toggle_tools(menu);
    if (menu->tools != TOOL_HOME || menu->tool_selected != 3)
        abort();
    tools_input(menu, ControlA);
    toggle_tools(menu);
    draw(menu);
    toggle_tools(menu);
    if (menu->tools != TOOL_HOME || menu->tool_selected != 3)
        abort();
    fprintf(stderr, "Tools selection: all seven tools, nested settings and close/reopen retain "
                    "the home selection and redraw\n");
    return 0;
}

/* font-measure-bench: drawing the same label 1000 times should measure its width once. */
static int run_font_measure_bench(Menu *menu)
{
    unsigned int i;
    preview_width_calls = 0;
    for (i = 0; i < 1000; ++i) {
        menu->text_cursor = 0;
        label_on(menu, 10, 70, "Unchanged menu text", TEXT, UI_FONT_NORMAL, BG);
    }
    fprintf(stderr, "Text width: %lu measurements for 1000 identical draws\n", preview_width_calls);
    return 0;
}

/* fullscreen-render: the full-screen cover and preview views. */
static int run_fullscreen_render(Menu *menu, CCB *cover)
{
    static unsigned short actual[320 * 240];
    CCB video = {5, 0, 0, 0};
    int mode, margin, step, idle = 0, changes = 0;
    char message[80];
    menu->screen.sc_BitmapItems[0] = menu->screen.sc_Screens[0] = 0;
    menu->screen.sc_BitmapItems[1] = menu->screen.sc_Screens[1] = 1;
    preview_raster = 1;
    preview_video_enabled = 1;
    if (preview_video_load(&menu->ode, ode_bus_read_buffer, "fixture", &menu->preview, NULL, NULL))
        abort();
    menu->preview.fps = 30;
    video.ccb_Width = 160;
    video.ccb_Height = 120;
    video.ccb_PRE1 = 78UL << 24;
    video.ccb_SourcePtr = menu->preview.frame_buffer;
    menu->preview_cel = &video;
    menu->preview_audio.status = 1;
    for (mode = 0; mode < 5; ++mode)
        for (margin = 0; margin <= 12; ++margin) {
            menu->config.screen_margin = margin;
            menu->cover = cover;
            menu->full_view = mode == 2 ? 2 : mode == 0 ? 1 : 0;
            menu->full_cleared = 0;
            menu->help_hold.active = mode == 1;
            menu->help_drawn = 0;
            menu->tools = mode == 3 ? TOOL_HOME : 0;
            menu->tools_home_drawn = 0;
            menu->tool_selected = 0;
            menu->tool_message = NULL;
            menu->settings_recent_games = menu->config.recent_games;
            preview_now = 10000;
            menu->preview_since = preview_now;
            menu->message = NULL;
            if (mode == 4) {
                MenuEntry *entry = (MenuEntry *)menu->listing.entries[2];
                entry->name = "A long selected game title that must keep scrolling while the "
                              "video is unchanged.iso";
                menu->selected = 2;
                menu->title_variants_valid = 0;
                menu->selection_since = preview_now - 1200;
            }
            if (usb_preview_frame(&menu->preview, 0, menu->preview.frame_buffer) < 0)
                abort();
            menu->preview_frame = 0;
            menu->panel_frames = menu->overlay_frames = 0;
            menu->preview_priming = 0;
            menu->osd_hidden = 0;
            for (step = 0; step < 80; ++step) {
                int front, current, cleared, help_drawn, home_drawn, panels, overlays, cost;
                unsigned long decoded, inline_decoded;
                preview_now = 10000 + (unsigned long)step * 5;
                if (mode == 1 && step == 30)
                    menu->tools = TOOL_SAVES;
                if (mode == 1 && step == 60)
                    menu->tools = TOOL_CONFIG;
                if (mode == 4) {
                    if (step == 20) {
                        strcpy(message, "A playback notice");
                        menu->message = message;
                    }
                    if (step == 21)
                        strcpy(message, "Changed playback notice");
                    if (step == 30)
                        menu->message = NULL;
                    if (step == 42)
                        menu->selected = 3;
                    if (step == 50)
                        menu->selected = 2;
                }
                if (mode == 3) {
                    if (step == 10)
                        menu->tool_selected = 2;
                    if (step == 20)
                        menu->settings_recent_games = !menu->config.recent_games;
                    if (step == 30) {
                        strcpy(message, "A new message");
                        menu->tool_message = message;
                    }
                    if (step == 31)
                        strcpy(message, "Changed in the same buffer");
                    if (step == 40)
                        menu->tool_message = "";
                    if (step == 42)
                        menu->tool_message = NULL;
                    if (step == 50)
                        menu->tools = TOOL_HELP;
                    if (step == 52)
                        menu->tools = TOOL_HOME;
                    if (step == 60)
                        menu->help_hold.active = 1;
                    if (step == 64)
                        menu->help_hold.active = 0;
                }
                preview_primitives = 0;
                if (mode == 1)
                    draw_context_help(menu);
                else if (mode >= 3)
                    draw(menu);
                else
                    draw_full_cover(menu);
                cost = preview_primitives;
                front = preview_front;
                current = menu->screen.sc_curScreen;
                if (!cost)
                    ++idle;
                else
                    ++changes;
                memcpy(actual, preview_screen[front], sizeof(actual));
                cleared = menu->full_cleared;
                decoded = menu->full_drawn_decode;
                help_drawn = menu->help_drawn;
                home_drawn = menu->tools_home_drawn;
                panels = menu->panel_frames;
                overlays = menu->overlay_frames;
                inline_decoded = menu->preview_drawn_decode;
                menu->screen.sc_curScreen = front ^ 1;
                menu->full_cleared = 0;
                menu->help_drawn = 0;
                menu->tools_home_drawn = 0;
                menu->panel_frames = 0;
                if (mode == 1)
                    draw_context_help(menu);
                else if (mode >= 3)
                    draw(menu);
                else
                    draw_full_cover(menu);
                if (memcmp(actual, preview_screen[front], sizeof(actual))) {
                    fprintf(stderr, "Full screen mismatch: mode %d margin %d step %d\n", mode,
                            margin, step);
                    abort();
                }
                menu->screen.sc_curScreen = current;
                menu->full_cleared = cleared;
                menu->full_drawn_decode = decoded;
                menu->help_drawn = help_drawn;
                menu->tools_home_drawn = home_drawn;
                menu->panel_frames = panels;
                menu->overlay_frames = overlays;
                menu->preview_drawn_decode = inline_decoded;
            }
            menu->help_hold.active = 0;
            menu->full_view = 0;
        }
    if (idle < 2000 || !changes || !audio_restarts)
        abort();
    fprintf(stderr,
            "Static/video rendering: 65 mode/margin combinations match forced redraws; %d idle "
            "polls draw nothing; audio loops continue\n",
            idle);
    /* Returning from Help must invalidate it before opening it again. */
    menu->preview_cel = NULL;
    menu->help_hold.active = 0;
    menu->tools = 0;
    draw(menu);
    if (menu->help_drawn)
        abort();
    menu->help_hold.active = 1;
    preview_primitives = 0;
    draw(menu);
    if (!preview_primitives || menu->help_drawn != 1)
        abort();
    return 0;
}

/* idle-draw-bench: how much an unchanged screen costs to redraw. */
static int run_idle_draw_bench(Menu *menu, CCB *cover)
{
    static CCB video = {2, 0, 0, 0};
    int mode, step;
    video.ccb_Width = 160;
    video.ccb_Height = 120;
    for (mode = 0; mode < 5; ++mode) {
        menu->full_view = mode == 0 ? 1 : mode == 2 ? 2 : 0;
        menu->full_cleared = 0;
        menu->help_hold.active = mode == 1;
        menu->tools = mode == 3 ? TOOL_HOME : 0;
        menu->cover = cover;
        menu->preview_cel = NULL;
        menu->preview.table = NULL;
        menu->panel_frames = menu->overlay_frames = 0;
        if (mode == 2 || mode == 4) {
            menu->preview_cel = &video;
            menu->preview.table = (unsigned char *)"synthetic";
            menu->preview.frames = 1;
            menu->preview.fps = 30;
        }
        preview_primitives = 0;
        display_count = 0;
        for (step = 0; step < 1000; ++step)
            draw(menu);
        if (display_count != (mode == 4 ? 4 : 2))
            abort();
        fprintf(stderr, "Idle %s: %d drawing calls, %d presentations for 1000 polls\n",
                mode == 0   ? "full cover"
                : mode == 1 ? "help"
                : mode == 2 ? "same video frame"
                : mode == 3 ? "Tools home"
                            : "inline video frame",
                preview_primitives, display_count);
    }
    return 0;
}

/* browser-render: the folder browser's layout and redraws. */
static int run_browser_render(Menu *menu, CCB *cover)
{
    static const int selections[] = {2,  2,  2,  3,  4,  2,  2,  12, 13, 12, 1,  0,  24, 23, 23,
                                     23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23};
    static unsigned short actual[320 * 240];
    MenuEntry entries[25];
    const MenuEntry *pointers[25];
    char names[25][96], notice[80];
    int margin, large, step, i, partial = 0, full = 0, idle = 0;
    free_listing(menu);
    for (i = 0; i < 25; ++i) {
        sprintf(names[i],
                i % 2 ? "A very long game title number %02d with extra words.iso" : "Game %02d.iso",
                i);
        entries[i].name = names[i];
        entries[i].toc_id = i + 1;
        entries[i].is_directory = i == 1;
        pointers[i] = entries + i;
    }
    menu->listing.entries = pointers;
    menu->listing.count = 25;
    menu->listing.has_parent = 0;
    menu->screen.sc_BitmapItems[0] = menu->screen.sc_Screens[0] = 0;
    menu->screen.sc_BitmapItems[1] = menu->screen.sc_Screens[1] = 1;
    preview_raster = 1;
    for (large = 0; large < 2; ++large)
        for (margin = 0; margin <= 12; ++margin) {
            menu->config.screen_margin = margin;
            menu->config.large_titles = large;
            menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;
            menu->selected = 2;
            menu->cover = NULL;
            menu->notice_active = 0;
            menu->tools = menu->alphabet_active = 0;
            menu->clear_frames = 2;
            preview_now = 10000;
            menu->selection_since = preview_now;
            for (step = 0; step < 79; ++step) {
                BrowserFrame saved[2];
                int current, cost, front;
                int selected = selections[step < 30 ? step : 29];
                if (menu->selected != (size_t)selected)
                    selection_changed(menu);
                menu->selected = selected;
                if (step == 14) {
                    cover->ccb_Width = 64;
                    cover->ccb_Height = 80;
                    menu->cover = cover;
                }
                if (step == 16)
                    menu->cover = NULL;
                if (step == 17) {
                    strcpy(notice, "Loading canceled");
                    menu->message = notice;
                }
                if (step == 18)
                    strcpy(notice, "Same buffer, different message");
                if (step == 19)
                    menu->message = NULL;
                if (step == 20) {
                    notify(menu, "Disc added");
                }
                if (step == 21)
                    preview_now += 2600;
                if (step == 22)
                    menu->alphabet_active = 1;
                if (step == 23)
                    menu->alphabet_active = 0;
                if (step == 24)
                    menu->tools = TOOL_HOME;
                if (step == 25)
                    menu->tools = 0;
                if (step == 26) {
                    menu->gecko_active = 0;
                    selection_changed(menu);
                }
                if (step > 26)
                    preview_now += 20;
                if (step == 48) {
                    menu->alphabet_active = 1;
                    menu->jump_letter = 0;
                }
                if (step >= 49 && step <= 75)
                    menu->jump_letter = (step - 48) % 27;
                if (step == 76)
                    menu->alphabet_active = 0;
                preview_primitives = 0;
                draw(menu);
                cost = preview_primitives;
                if (step == 2) {
                    if (cost)
                        abort();
                    ++idle;
                }
                if (step >= 28 && step <= 42 && cost > 1)
                    abort(); /* Both buffers primed; only the mascot changes. */
                if (step >= 50 && step <= 75 && cost > 5)
                    abort(); /* Only the old and new letters change. */
                partial += cost;
                front = preview_front;
                current = menu->screen.sc_curScreen;
                memcpy(actual, preview_screen[front], sizeof(actual));
                memcpy(saved, menu->browser_frames, sizeof(saved));
                menu->browser_frames[0].valid = menu->browser_frames[1].valid = 0;
                menu->screen.sc_curScreen = front ^ 1;
                preview_primitives = 0;
                draw(menu);
                full += preview_primitives;
                if (memcmp(actual, preview_screen[front], sizeof(actual))) {
                    for (i = 0; i < 320 * 240; ++i)
                        if (actual[i] != preview_screen[front][i])
                            break;
                    fprintf(stderr, "Browser mismatch: margin %d large %d step %d pixel %d,%d\n",
                            margin, large, step, i % 320, i / 320);
                    abort();
                }
                memcpy(menu->browser_frames, saved, sizeof(saved));
                menu->screen.sc_curScreen = current;
            }
        }
    if (!idle || partial >= full * 3 / 4)
        abort();
    fprintf(stderr,
            "Browser rendering: 26 margin/font combinations match full redraws; %d versus %d "
            "drawing calls; idle draws zero\n",
            partial, full);
    return 0;
}

/* Check the corner mask using the SDK's padded source row layout. */
static void check_corner_mask(Menu *menu)
{
    {
        int x, y;
        unsigned short *pixels;
        crt_cels(menu);
        if (!menu->screen_corner || ((menu->screen_corner->ccb_PRE1 >> 24) & 255) != 3)
            abort();
        pixels = (unsigned short *)menu->screen_corner->ccb_SourcePtr;
        for (y = 0; y < 9; ++y)
            for (x = 0; x < 10; ++x) {
                int dx = 2 * (9 - x) - 1, dy = 2 * (9 - y) - 1;
                unsigned short expected =
                    (x < 9 && dx * dx + dy * dy > 18 * 18) ? (unsigned short)INK : 0;
                if (pixels[y * 10 + x] != expected)
                    abort();
            }
    }
}

/* Widths remain valid across clipping/margin changes, and text/style
 * changes still invalidate them. Growing the available room must grow
 * a cel originally allocated for a clipped label. */
static void check_text_width_cache(Menu *menu)
{
    {
        const char *name = "Long menu text with a changing right edge";
        int original, expected = ui_font_width(name, UI_FONT_NORMAL);
        unsigned long calls;
        menu->text_cursor = 0;
        label_on(menu, 290, 70, name, TEXT, UI_FONT_NORMAL, BG);
        original = menu->text_cache[0].capacity_width;
        calls = preview_width_calls;
        menu->text_cursor = 0;
        label_on(menu, 290, 70, name, TEXT, UI_FONT_NORMAL, BG);
        if (preview_width_calls != calls || menu->text_cache[0].glyph_width != expected)
            abort();
        menu->text_cursor = 0;
        label_on(menu, 10, 70, name, TEXT, UI_FONT_NORMAL, BG);
        if (menu->text_cache[0].capacity_width <= original ||
            menu->text_cache[0].cel->ccb_Width != expected || preview_width_calls != calls)
            abort();
        menu->config.screen_margin = 12;
        menu->text_cursor = 0;
        label_on(menu, 10, 70, name, TEXT, UI_FONT_NORMAL, BG);
        if (preview_width_calls != calls)
            abort();
        menu->text_cursor = 0;
        label_on(menu, 10, 70, name, TEXT, UI_FONT_SMALL, BG);
        if (preview_width_calls != calls + 1)
            abort();
        menu->text_cursor = 0;
        label_on(menu, 10, 70, "Changed text", TEXT, UI_FONT_SMALL, BG);
        if (preview_width_calls != calls + 2)
            abort();
        menu->config.screen_margin = 0;
    }
}

/* A held direction must not keep resetting the remote to frame zero. */
static void check_remote_hold(Menu *menu)
{
    preview_now = 1000;
    menu->gecko_active = 0;
    selection_changed(menu);
    if (!menu->gecko_active || menu->gecko_since != 1000 || channel_gecko_frame(menu, 1000) != 0)
        abort();
    preview_now += 66;
    selection_changed(menu);
    if (menu->gecko_since != 1000 || channel_gecko_frame(menu, 1180) != 9)
        abort();
    if (channel_gecko_frame(menu, 1320) != 0 || menu->gecko_active)
        abort();
}

/* A folder whose only game is listed in the metadata is recognized as that game. */
static void check_folder_game_lookup(Menu *menu)
{
    {
        Menu probe;
        LibraryGameInfo info = {"Folder/Disc.iso", "", "", ""};
        MenuEntry folder = {1, 1, "Folder"};
        memset(&probe, 0, sizeof(probe));
        probe.metadata.entries = &info;
        probe.metadata.count = 1;
        if (!game_entry(&probe, &folder) || probe.lookup_next != 1)
            abort();
        if (!game_entry(&probe, &folder) || probe.lookup_next != 1)
            abort(); /* Cached. */
        probe.metadata.count = 0;
        if (game_entry(&probe, &folder))
            abort();
        probe.metadata.count = 1;
        ++probe.listing_generation;
        if (!game_entry(&probe, &folder))
            abort();
    }
}

/* Titles that differ only by region or revision are labeled apart. */
static void check_title_variants(Menu *menu)
{
    menu->tool_selected = 0;
    {
        Menu titles;
        MenuEntry entries[] = {{1, 0, "Game (Europe).iso"},
                               {2, 0, "Game (Japan) (RE1).iso"},
                               {3, 0, "Game (USA).iso"},
                               {4, 0, "Game (USA) (RE1).iso"},
                               {5, 0, "Game (USA) (Unl).iso"}};
        const MenuEntry *pointers[5];
        int i;
        memset(&titles, 0, sizeof(titles));
        menu_config_defaults(&titles.config);
        for (i = 0; i < 5; ++i)
            pointers[i] = entries + i;
        titles.listing.entries = pointers;
        titles.listing.count = 5;
        titles.ready = titles.view_valid = 1;
        if (title_variant(&titles, 0) != 1 || title_variant(&titles, 2) != 2 ||
            title_variant(&titles, 1) != 1 || title_variant(&titles, 3) != 2 ||
            title_variant(&titles, 4))
            abort();
        titles.selected = 2;
        draw_row(&titles, 2, 0, 2, UI_FONT_SMALL, 14);
        if (strcmp(titles.text_cache[0].value, "Game [2]"))
            abort();
        titles.text_cursor = 0;
        draw_title_card(&titles, entries + 2, 5, NULL, 320, 0);
        if (strcmp(titles.text_cache[0].value, "Game [2]"))
            abort();
        strcpy(titles.filter, "Europe");
        if (title_variant(&titles, 0))
            abort();
        titles.filter[0] = 0;
        if (title_variant(&titles, 2) != 2)
            abort();
        free(titles.title_variants);
        free((void *)titles.visible);
    }
}

/* A scrolling title stops with its last character fully visible. */
static void check_scrolling_title_end(Menu *menu)
{
    {
        Menu label_menu;
        const char *name = "Dennis Miller - That's News To Me";
        size_t length = strlen(name);
        int margin, style, card;
        memset(&label_menu, 0, sizeof(label_menu));
        menu_config_defaults(&label_menu.config);
        for (margin = 0; margin <= 12; ++margin)
            for (style = 0; style < 4; ++style)
                for (card = 0; card < 2; ++card) {
                    int x = card ? 160 : 16, room = card ? 152 : 122, scale = 320 - 2 * margin;
                    int actual = room * scale / 320, limit = (308 - x) * scale / 320;
                    size_t tail;
                    UiText *cached;
                    if (actual > limit)
                        actual = limit;
                    label_menu.config.screen_margin = margin;
                    tail = ui_font_tail_start(name, length, actual, style);
                    label_menu.scroll_elapsed = 1200UL + (unsigned long)tail * 180UL + 300UL;
                    label_menu.scroll_count = 0;
                    label_menu.text_cursor = 0;
                    scroll_label(&label_menu, x, 181, room, name, length, style, TEXT, BG, 1);
                    cached = &label_menu.text_cache[0];
                    if (strcmp(cached->value, name + tail) ||
                        cached->value[strlen(cached->value) - 1] != 'e' ||
                        ui_font_fit(cached->value, strlen(cached->value), cached->width, style) !=
                            strlen(cached->value))
                        abort();
                    if (label_menu.scroll_visible[0] != length - tail)
                        abort(); /* Cache uses the same end stop. */
                }
    }
}

/* Loading a folder from the filtered root never redraws the old root at the new depth. */
static void check_filtered_root_loading(Menu *menu)
{
    {
        Menu root;
        static const char *names[] = {"Games", "Adult", "boot.iso", ".ohmy3do"};
        int i;
        memset(&root, 0, sizeof(root));
        menu_config_defaults(&root.config);
        root.ready = root.view_valid = root.demo = 1;
        strcpy(root.breadcrumb, "Root filtered");
        root.listing.entries = (const MenuEntry **)malloc(4 * sizeof(MenuEntry *));
        root.listing.count = 4;
        for (i = 0; i < 4; ++i) {
            MenuEntry *entry = (MenuEntry *)malloc(sizeof(MenuEntry) + strlen(names[i]) + 1);
            if (!entry)
                abort();
            entry->name = (char *)(entry + 1);
            strcpy((char *)entry->name, names[i]);
            entry->is_directory = i != 2;
            entry->toc_id = i + 1;
            root.listing.entries[i] = entry;
        }
        menu_set_hide_adult(1);
        if (row_count(&root) != 1 || strcmp(browser_entry(&root, 0)->name, "Games"))
            abort();
        draw(&root); /* The already-filtered root that the user sees. */
        root.depth = 1;
        root.path_ids[0] = 1;
        strcpy(root.path[0], "Games");
        root.ode.progress = progress;
        root.ode.progress_context = &root;
        reading_menu = &root;
        reading_transition = 1;
        preview_primitives = 0;
        load_directory(&root, 0);
        if (!root.ready || root.loading || root.listing.count || strcmp(root.breadcrumb, "/ Games"))
            abort();
        reading_transition = 0;
        menu_set_hide_adult(menu->config.hide_adult);
    }
}

/* Directory loading: an empty folder, then a failed read that keeps the old listing. */
static void check_directory_loading(Menu *menu)
{
    reading_menu = menu;
    menu->selected = 2;
    menu->ode.progress = progress;
    menu->ode.progress_context = menu;
    menu->depth = 1;
    strcpy(menu->path[0], "Destination");
    load_directory(menu, 0);
    if (!menu->ready || !menu->view_valid || menu->loading || menu->listing.count ||
        strcmp(menu->breadcrumb, "/ Destination"))
        abort();
    show_demo(menu);
    menu->selected = 2;
    reading_fail = 1;
    load_directory(menu, 0);
    if (menu->ready || menu->view_valid || menu->loading || menu->listing.count != 9 ||
        strcmp(menu->message, "Fixture read failed"))
        abort();
}

/* Moving the selection drops the cover; a notice shows for 2.5 seconds. */
static void check_selection_clears_media(Menu *menu, CCB *cover)
{
    preview_now = 10000;
    menu->cover = cover;
    selection_changed(menu);
    if (menu->selection_since != 10000 || menu->cover || preview_unloads != 1)
        abort();
    notify(menu, "Disc added");
    preview_now += 2499;
    draw(menu);
    if (!menu->notice_active)
        abort();
    ++preview_now;
    draw(menu);
    if (menu->notice_active)
        abort();
}

/* Covers load only after the selection rests, once, and not for folders or files without one. */
static void check_cover_scheduling(Menu *menu, CCB *cover)
{
    show_demo(menu);
    menu->selected = 2;
    menu->covers_enabled = 1;
    preview_now = 20000;
    selection_changed(menu);
    preview_now += COVER_IDLE_MS - 1;
    update_cover(menu);
    if (preview_cel_calls)
        abort();
    ++preview_now;
    update_cover(menu);
    if (preview_cel_calls != 1 || !menu->cover || menu->cover->ccb_XPos != 236L * 65536 ||
        menu->cover->ccb_YPos != 86L * 65536)
        abort();
    update_cover(menu);
    if (preview_cel_calls != 1)
        abort();
    selection_changed(menu);
    if (menu->cover)
        abort();
    preview_cel_fail = 1;
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (preview_cel_calls != 2 || menu->cover)
        abort();
    update_cover(menu);
    if (preview_cel_calls != 2)
        abort();
    menu->selected = 0; /* folder */
    selection_changed(menu);
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (preview_cel_calls != 2)
        abort();
    menu->selected = 5; /* file without a matching cover */
    selection_changed(menu);
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (preview_cel_calls != 2 || !menu->cover_attempted)
        abort();
}

/* Real cover scheduling/cache path; replace only the file decoder. */
static void check_cover_cache_path(Menu *menu)
{
    menu->demo = 0;
    menu->ready = menu->view_valid = menu->covers_enabled = 1;
    menu->config_result = 0;
    strcpy(menu->ode.identity, "LOCODE");
    menu->usb_art.entries = (MenuCover *)malloc(2 * sizeof(MenuCover));
    menu->usb_art.count = 2;
    menu->usb_art.entries[0].path = "Alone in the Dark.iso";
    menu->usb_art.entries[0].file = "Dark.png";
    menu->usb_art.entries[1].path = "Disc 1.iso";
    menu->usb_art.entries[1].file = "Disc.png";
    menu->selected = 2;
    preview_cel_fail = 0;
    selection_changed(menu);
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (!menu->cover || preview_png_loads != 1 || menu->cover_cached ||
        menu->cover->ccb_Width != USB_COVER_WIDTH || menu->cover->ccb_Height != USB_COVER_HEIGHT)
        abort();
    menu->selected = 3;
    selection_changed(menu);
    preview_now += COVER_IDLE_MS - 1;
    update_cover(menu);
    if (menu->cover || preview_png_loads != 1)
        abort();
    ++preview_now;
    update_cover(menu);
    if (!menu->cover || preview_png_loads != 2)
        abort();
    menu->selected = 2;
    selection_changed(menu);
    update_cover(menu);
    if (!menu->cover || preview_png_loads != 2 || !menu->cover_cached || menu->cover_commands ||
        ((unsigned char *)menu->cover->ccb_SourcePtr)[0] != 1)
        abort();
    cover_cache_clear(&menu->cover_cache);
    selection_changed(menu);
    preview_png_fail = 1;
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (menu->cover || cover_cache_contains(&menu->cover_cache, 0, "Dark.png"))
        abort();
    preview_png_fail = 0;
    preview_buttons = ControlB;
    selection_changed(menu);
    preview_now += COVER_IDLE_MS;
    update_cover(menu);
    if (menu->cover || cover_cache_contains(&menu->cover_cache, 0, "Dark.png"))
        abort();
    preview_buttons = 0;
    usb_art_index_free(&menu->usb_art);
}

/* RAM playback uses the real loader scheduling, draw and cleanup code. */
static void check_ram_preview_playback(Menu *menu)
{
    menu->demo = 0;
    menu->selected = 2;
    menu->ready = 1;
    menu->covers_enabled = 1;
    menu->config_result = 0;
    strcpy(menu->ode.identity, "LOCODE");
    menu->usb_previews.entries = (MenuCover *)malloc(sizeof(MenuCover));
    if (!menu->usb_previews.entries)
        abort();
    menu->usb_previews.entries[0].path = "Alone in the Dark.iso";
    menu->usb_previews.entries[0].file = "Game.3dv";
    menu->usb_previews.count = 1;
    menu->config.video_previews = 1;
    menu->config.preview_sound = 1;
    preview_video_enabled = 1;
    preview_cel_fail = 0;
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS - 1;
    update_preview(menu);
    if (preview_video_loads || menu->preview_attempted)
        abort();
    ++preview_now;
    menu->tools = TOOL_HOME;
    update_preview(menu);
    menu->tools = 0;
    menu->tools_requested = 1;
    update_preview(menu);
    menu->tools_requested = 0;
    menu->alphabet_active = 1;
    update_preview(menu);
    menu->alphabet_active = 0;
    menu->review = 1;
    update_preview(menu);
    menu->review = 0;
}

/* A direction queued by a canceled cover read must move the list
 * before another media load can start. */
static void check_queued_direction_after_cancel(Menu *menu)
{
    menu->cancel_directions = ControlDown;
    update_preview(menu);
    menu->cancel_directions = 0;
    menu->config.video_previews = 0;
    update_preview(menu);
    if (preview_video_loads)
        abort();
    menu->config.video_previews = 1;
    preview_load_delay = 5000;
    preview_display_delay = 17;
    update_preview(menu);
    preview_load_delay = preview_display_delay = 0;
    if (preview_video_loads != 1 || !menu->preview_cel || !menu->preview.table ||
        audio_starts != 1 || menu->preview_audio.status != 1)
        abort();
    if (menu->preview_priming || menu->preview_frame || menu->clear_frames ||
        menu->panel_frames != 2 || audio_start_displays != display_count)
        abort();
}

/* The title holds its exact pre-load scroll position through a slow
 * read and both initial redraws, then resumes from that position. */
static void check_title_scroll_during_load(Menu *menu)
{
    if (menu->scroll_paused || menu->scroll_elapsed != PREVIEW_IDLE_MS ||
        preview_now - menu->selection_since != PREVIEW_IDLE_MS)
        abort();
    {
        unsigned long before = overlay_key(menu, NULL);
        preview_now += 20;
        if (overlay_key(menu, NULL) != before)
            abort(); /* Uses this frame's snapshot. */
        preview_now -= 20;
    }
    preview_now += 167;
    draw(menu);
    if (menu->preview_frame != 1 || ((unsigned char *)menu->preview_cel->ccb_SourcePtr)[0] != 0x22)
        abort();
    preview_now += 167;
    draw(menu);
    if (menu->preview_frame || ((unsigned char *)menu->preview_cel->ccb_SourcePtr)[0] != 0x11 ||
        audio_restarts != 1)
        abort();
    free(menu->preview.decoder);
    menu->preview.decoder = NULL;
    preview_now += 167;
    draw(menu);
    if (menu->preview_result != -3)
        abort();
    update_preview(menu);
    if (menu->preview.table || menu->preview.audio || menu->preview_cel ||
        menu->preview_audio.status || preview_video_loads != 1)
        abort();
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    preview_buttons = ControlB;
    update_preview(menu);
    preview_buttons = 0;
}

/* A held button cancels during the channel blink, before any read. */
static void check_cancel_during_blink(Menu *menu)
{
    if (menu->preview.table || menu->preview_cel || !menu->preview_attempted ||
        preview_video_loads != 1 || menu->preview_result != -5)
        abort();
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    preview_cel_fail = 1;
    update_preview(menu);
    preview_cel_fail = 0;
    if (menu->preview.table || menu->preview_cel || menu->preview_result != -4)
        abort();
    menu->config.preview_sound = 0;
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    update_preview(menu);
    if (!menu->preview_cel || menu->preview_audio.status || audio_starts != 1)
        abort();
    menu->config.preview_sound = 1;
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    audio_fail = 1;
    update_preview(menu);
    audio_fail = 0;
    if (!menu->preview_cel || menu->preview_audio.status != -1)
        abort();
    stop_preview(menu);
}

/* X swaps the playing preview for the cover until it is pressed again;
 * the next load starts at once, and moving on forgets the choice. */
static void check_preview_cover_swap(Menu *menu, CCB *cover)
{
    {
        int loads;
        selection_changed(menu);
        preview_now += PREVIEW_IDLE_MS + 270;
        update_preview(menu);
        loads = preview_video_loads;
        if (!menu->preview_cel || !toggle_preview(menu) || !menu->preview_off || menu->preview_cel)
            abort();
        update_preview(menu);
        if (preview_video_loads != loads)
            abort();
        if (!toggle_preview(menu) || menu->preview_off)
            abort();
        update_preview(menu);
        if (preview_video_loads != loads + 1 || !menu->preview_cel)
            abort();
        /* Exercise the actual tap/hold dispatcher while video is playing. */
        if (media_x_input(menu, ControlX, ControlX, 0) || !menu->x_pending || !menu->preview_cel)
            abort();
        preview_now += 100;
        if (!(media_x_input(menu, 0, 0, ControlX) & ControlX) || menu->x_pending)
            abort();
        if (!toggle_preview(menu) || !menu->preview_off || menu->preview_cel)
            abort();
        if (!toggle_preview(menu))
            abort();
        update_preview(menu);
        loads = preview_video_loads;
        if (media_x_input(menu, ControlX, ControlX, 0) || !menu->x_pending)
            abort();
        preview_now += 499;
        media_x_input(menu, ControlX, 0, 0);
        if (menu->full_view)
            abort();
        preview_now += 1;
        media_x_input(menu, ControlX, 0, 0);
        if (menu->full_view != 2 || !menu->preview_cel || menu->x_pending ||
            preview_video_loads != loads)
            abort();
        {
            unsigned int frame = menu->preview_frame;
            long hdx = menu->preview_cel->ccb_HDX;
            menu->preview_since = preview_now;
            preview_now += 167;
            draw_full_cover(menu);
            if (menu->preview_frame == frame || menu->preview_cel->ccb_HDX != hdx)
                abort();
        }
        close_full_cover(menu);
        if (menu->full_view || !menu->preview_cel || preview_video_loads != loads)
            abort();
        if (!toggle_preview(menu))
            abort();
        menu->cover = cover;
        menu->cover_file = NULL;
        media_x_input(menu, ControlX, ControlX, 0);
        preview_now += 500;
        media_x_input(menu, ControlX, 0, 0);
        if (menu->full_view != 1 || menu->x_pending || !menu->preview_off)
            abort();
        /* A held X must not cancel the large cover's USB read; a fresh
         * press after release still cancels immediately. */
        menu->media_cancelled = 0;
        preview_buttons = ControlX;
        if (media_cancel(menu))
            abort();
        preview_buttons = 0;
        if (media_cancel(menu) || menu->suppress_buttons)
            abort();
        preview_buttons = ControlX;
        if (!media_cancel(menu))
            abort();
        preview_buttons = 0;
        menu->media_cancelled = 0;
        close_full_cover(menu);
        menu->cover = NULL;
        if (media_x_input(menu, 0, 0, ControlX) || menu->full_view)
            abort();
        if (!toggle_preview(menu))
            abort();
        update_preview(menu);
        menu->suppress_buttons = 0;
        /* Another button cancels the hold without a delayed X tap. */
        media_x_input(menu, ControlX, ControlX, 0);
        media_x_input(menu, ControlX | ControlDown, ControlDown, 0);
        if (menu->x_pending || media_x_input(menu, 0, 0, ControlX))
            abort();
        menu->preview_off = 1;
        selection_changed(menu);
        if (menu->preview_off || menu->preview_cel)
            abort();
        stop_preview(menu);
    }
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    preview_video_enabled = 2;
    {
        unsigned char cache_pixels[USB_COVER_PIXELS];
        memset(cache_pixels, 0, sizeof(cache_pixels));
        cover_cache_put(&menu->cover_cache, 0, "Cached.png", cache_pixels, sizeof(cache_pixels));
        update_preview(menu);
        if (!menu->preview_cel || menu->preview_cel->ccb_SourcePtr != menu->preview.frame_buffer ||
            ((unsigned char *)menu->preview_cel->ccb_SourcePtr)[0] != 0x11 ||
            !menu->cover_cache.clock)
            abort();
        stop_preview(menu);
        selection_changed(menu);
        preview_now += PREVIEW_IDLE_MS + 270;
        preview_video_reclaim = 1;
        update_preview(menu);
        preview_video_reclaim = 0;
        if (!menu->preview_cel || menu->cover_cache.clock)
            abort();
    }
    stop_preview(menu);
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    preview_video_enabled = USB_PREVIEW_NO_MEMORY;
    update_preview(menu);
    if (menu->preview_result != -4 || menu->ode.navigation_failed || !menu->ready ||
        strcmp(menu->notice, "Preview needs more free memory."))
        abort();
    selection_changed(menu);
    preview_now += PREVIEW_IDLE_MS + 270;
    preview_video_enabled = -1;
    update_preview(menu);
    if (menu->preview_result != -1 || strcmp(menu->notice, "Preview failed. Check diagnostics."))
        abort();
    usb_art_index_free(&menu->usb_previews);
    preview_video_enabled = 0;
    menu->config.video_previews = 0;
    menu->demo = 1;
}

/* Canceling with A must not also open/play on the next frame.
 * Directional cancellation still advances the browser selection. */
static void check_cancel_does_not_activate(Menu *menu)
{
    menu->media_cancelled = 0;
    menu->suppress_buttons = 0;
    preview_buttons = ControlA;
    if (!media_cancel(menu) || !(menu->suppress_buttons & ControlA))
        abort();
    menu->media_cancelled = 0;
    if (media_cancel(menu))
        abort(); /* Still held, already consumed. */
    preview_buttons = 0;
    media_cancel(menu);
    if (menu->suppress_buttons)
        abort();
    menu->cancel_directions = 0;
    preview_buttons = ControlDown;
    if (!media_cancel(menu) || menu->suppress_buttons || !(menu->cancel_directions & ControlDown))
        abort();
    menu->cancel_directions = 0;
    preview_buttons = 0;
}

/* A complete L+R press during a media cancellation poll survives
 * release before the next main-loop iteration. It fires only once. */
static void check_shoulder_chord_during_cancel(Menu *menu)
{
    menu->tools = 0;
    menu->tools_requested = menu->tools_chord_down = 0;
    menu->media_cancelled = 0;
    preview_buttons = ControlLeftShift;
    if (media_cancel(menu))
        abort();
    preview_buttons = ControlLeftShift | ControlRightShift;
    if (!media_cancel(menu))
        abort();
    preview_buttons = 0;
    media_cancel(menu);
    if (!dispatch_tools_request(menu) || menu->tools != TOOL_HOME)
        abort();
    if (dispatch_tools_request(menu))
        abort();
    observe_tools_chord(menu, ControlLeftShift);
    observe_tools_chord(menu, ControlLeftShift | ControlRightShift);
    if (!dispatch_tools_request(menu) || menu->tools)
        abort();
    observe_tools_chord(menu, ControlRightShift);
    observe_tools_chord(menu, ControlLeftShift | ControlRightShift);
    if (dispatch_tools_request(menu))
        abort();
    observe_tools_chord(menu, 0);
    observe_tools_chord(menu, ControlLeftShift | ControlRightShift);
    if (!dispatch_tools_request(menu) || menu->tools != TOOL_HOME)
        abort();
    observe_tools_chord(menu, 0);
    menu->tools = 0;
    menu->media_cancelled = 0;
}

/* L/R is emitted only on a solo release, never on the first half
 * of a staggered chord or after a different action consumes the hold. */
static void check_shoulder_release(Menu *menu)
{
    {
        u32 l = ControlLeftShift, r = ControlRightShift;
        size_t selection = menu->selected;
        menu->shoulder_pending = 0;
        observe_tools_chord(menu, l);
        if (shoulder_input(menu, l, l, 0))
            abort();
        preview_now += 1000;
        observe_tools_chord(menu, l | r);
        if (shoulder_input(menu, l | r, r, 0))
            abort();
        if (!dispatch_tools_request(menu) || menu->tools != TOOL_HOME ||
            menu->selected != selection)
            abort();
        observe_tools_chord(menu, l);
        if (shoulder_input(menu, l, 0, r))
            abort();
        observe_tools_chord(menu, 0);
        if (shoulder_input(menu, 0, 0, l))
            abort();
        menu->tools = 0;
        observe_tools_chord(menu, r);
        if (shoulder_input(menu, r, r, 0))
            abort();
        observe_tools_chord(menu, l | r);
        if (shoulder_input(menu, l | r, l, 0))
            abort();
        if (!dispatch_tools_request(menu))
            abort();
        observe_tools_chord(menu, 0);
        if (shoulder_input(menu, 0, 0, l | r))
            abort();
        menu->tools = 0;
        if (shoulder_input(menu, l, l, 0) || shoulder_input(menu, l, 0, 0))
            abort();
        if (shoulder_input(menu, 0, 0, l) != l || shoulder_input(menu, 0, 0, 0))
            abort();
        if (shoulder_input(menu, r, r, 0) || shoulder_input(menu, r, 0, 0))
            abort();
        if (shoulder_input(menu, 0, 0, r) != r)
            abort();
        shoulder_input(menu, l, l, 0);
        if (shoulder_input(menu, l | ControlA, ControlA, 0) != ControlA)
            abort();
        if (shoulder_input(menu, 0, 0, l | ControlA))
            abort();
        menu->tools_requested = 1;
        shoulder_input(menu, l, l, 0);
        if (!dispatch_tools_request(menu) || menu->shoulder_pending)
            abort();
        observe_tools_chord(menu, 0);
        menu->tools = 0;
    }
}

/* Root index reads are cached even when empty/missing. Virtual roots
 * retain the cache; changing volume or explicit refresh reloads it. */
static void check_root_index_cache(Menu *menu)
{
    show_demo(menu);
    menu->demo = 0;
    menu->config_result = 0;
    strcpy(menu->ode.identity, "LOCODE");
    menu->config.usb_covers = menu->config.video_previews = 1;
    menu->depth = 1;
    menu->path_ids[0] = 0x01000000UL;
    preview_index_reads = 0;
    refresh_usb_art(menu);
    refresh_usb_art(menu);
    menu->depth = 2;
    refresh_usb_art(menu);
    menu->depth = 1;
    refresh_usb_art(menu);
    if (preview_index_reads != 3)
        abort();
    {
        const MenuEntry volume = {0x01000000UL, 1, "USB"};
        const MenuEntry *volumes[1];
        const MenuEntry **saved = menu->listing.entries;
        size_t count = menu->listing.count;
        volumes[0] = &volume;
        menu->listing.entries = volumes;
        menu->listing.count = 1;
        menu->depth = 0;
        refresh_usb_art(menu);
        menu->listing.entries = saved;
        menu->listing.count = count;
        menu->depth = 1;
        refresh_usb_art(menu);
        if (preview_index_reads != 3)
            abort();
    }
    menu->path_ids[0] = 0x02000000UL;
    refresh_usb_art(menu);
    if (preview_index_reads != 6)
        abort();
    menu->art_indexes_valid = 0;
    refresh_usb_art(menu);
    if (preview_index_reads != 9)
        abort();
    show_demo(menu);
}

/* Details must leave the browser selection and storage untouched. */
static void check_details_read_only(Menu *menu)
{
    {
        size_t before = menu->selected, last;
        int reads = preview_reads, writes = preview_writes, nvram = preview_nvram_reads;
        menu->tools = 0;
        menu->demo = 0;
        menu->ready = 1;
        browser_shortcuts(menu, ControlC, ControlC, 0);
        if (browser_shortcuts(menu, ControlC | ControlStart, ControlStart, 0) || menu->c_pending)
            abort();
        menu->demo = 1;
        if (menu->tools != TOOL_GAME_DETAILS || menu->game_details_page)
            abort();
        draw(menu);
        if (!menu->game_details_pages)
            abort();
        /* A full-length name must survive wrapping across several pages. */
        {
            char long_name[1024];
            size_t rows = 0;
            memset(long_name, 'W', sizeof(long_name) - 1);
            long_name[sizeof(long_name) - 1] = 0;
            game_details_text(menu, long_name, &rows, TEXT);
            if (rows <= 10)
                abort();
            menu->game_details_pages = (rows + 9) / 10;
        }
        last = menu->game_details_pages - 1;
        while (menu->game_details_page < last)
            tools_input(menu, ControlRight);
        tools_input(menu, ControlRight);
        if (menu->game_details_page != last)
            abort();
        while (menu->game_details_page)
            tools_input(menu, ControlLeft);
        tools_input(menu, ControlLeft);
        if (menu->game_details_page || menu->selected != before)
            abort();
        tools_input(menu, ControlB);
        if (menu->tools || menu->selected != before || reads != preview_reads ||
            writes != preview_writes || nvram != preview_nvram_reads)
            abort();
    }
}

/* Tools wraps across seven entries; viewing Help/Diagnostics is read-only. */
static void check_tools_navigation(Menu *menu)
{
    menu->tools = TOOL_HOME;
    menu->tool_selected = 0;
    tools_input(menu, ControlUp);
    if (menu->tool_selected != 3)
        abort();
    tools_input(menu, ControlDown);
    if (menu->tool_selected != 0)
        abort();
    tools_input(menu, ControlRight);
    if (menu->tool_selected != 4)
        abort();
    tools_input(menu, ControlLeft);
    if (menu->tool_selected != 0)
        abort();
    menu->tool_selected = 3;
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_HELP || menu->usb_tested)
        abort();
    {
        Menu slots;
        char path[40];
        memset(&slots, 0, sizeof(slots));
        if (slot_count(&slots) != 10)
            abort();
        slot_select(&slots, 8);
        if (slots.slot_page != 1 || slots.tool_selected || slot_count(&slots) != 8)
            abort();
        slot_path(path, slot_at(&slots, 0));
        if (strcmp(path, "saves/slot09.3ds"))
            abort();
        slot_select(&slots, 999999998);
        if (slot_count(&slots) != 7 || slots.tool_selected != 6)
            abort();
        slot_path(path, slot_at(&slots, 6));
        if (strcmp(path, "saves/slot999999999.3ds"))
            abort();
        slot_select(&slots, SAVE_RECOVERY_SLOT);
        if (slots.slot_page || slots.tool_selected != 8)
            abort();
        slot_path(path, slot_at(&slots, 8));
        if (strcmp(path, "saves/recovery.3ds"))
            abort();
        slots.demo = slots.ready = 1;
        slots.tools = TOOL_SLOTS;
        tools_input(&slots, ControlStart);
        if (slots.tools != TOOL_SLOT_FIND)
            abort();
        tools_input(&slots, ControlA);
        if (slots.tools != TOOL_SLOT_FIND || !slots.tool_message ||
            strcmp(slots.tool_message, "USB save access unavailable."))
            abort();
        slots.demo = slots.ready = 1;
        slots.tools = TOOL_ARCHIVE;
        slots.tool_selected = 1;
        slots.backup_slot = 12;
        preview_nvram_fixture(&slots.backup);
        tools_input(&slots, ControlStart);
        if (slots.tools != TOOL_SLOTS || !slots.backup_merge || slots.pending.count != 1 ||
            slots.copy_original.count != 3 || slots.backup.bytes)
            abort();
        if (strcmp(slots.pending.entries[0].name, "NFS_Config"))
            abort();
        tools_input(&slots, ControlB);
        if (slots.tools != TOOL_ARCHIVE || slots.backup.count != 3 || slots.backup_slot != 12 ||
            slots.tool_selected != 1 || slots.copy_original.bytes)
            abort();
        save_archive_free(&slots.backup);
    }
}

/* Actual tools/controller code: entering/canceling never writes. */
static void check_tools_never_write(Menu *menu)
{
    menu->tools = TOOL_HOME;
    menu->tool_selected = 0;
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_SAVES || preview_nvram_reads != 1 || preview_nvram_writes)
        abort();
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_SLOTS || menu->pending.count != 1 || preview_nvram_writes)
        abort();
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_SLOTS || !menu->backup_merge || preview_nvram_writes)
        abort();
}

/* Demo I/O cannot read a merge destination. Simulate its reviewed page. */
static void check_merge_review(Menu *menu)
{
    menu->tools = TOOL_BACKUP_DETAILS;
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_CONFIRM_BACKUP || preview_nvram_writes)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_SLOTS || preview_nvram_writes)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_SAVES || menu->backup_mode || menu->pending.count)
        abort();
    tools_input(menu, ControlRight);
    if (menu->tools != TOOL_SLOTS || menu->backup_mode || menu->tool_selected)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_HOME)
        abort();
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_SAVES)
        abort();
    tools_input(menu, ControlLeft);
    if (menu->tools != TOOL_SLOTS || menu->backup_mode)
        abort();
    tools_input(menu, ControlRight);
    if (menu->tools != TOOL_SAVES || menu->tool_selected || preview_nvram_writes)
        abort();
    tools_input(menu, ControlLeft);
    preview_nvram_fixture(&menu->backup);
    menu->tools = TOOL_ARCHIVE;
    menu->tool_selected = 0;
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_CONFIRM_RESTORE || preview_nvram_writes)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_ARCHIVE || preview_nvram_writes)
        abort();
    tools_input(menu, ControlA);
    tools_input(menu, ControlA);
    if (preview_nvram_writes || menu->restore_result != -1)
        abort(); /* demo USB blocked */
}

/* Hex edits are private; canceling a byte or discarding preserves source.
 * Save as rebuilds CRCs and cannot write USB/NVRAM before confirmation. */
static void check_hex_editor(Menu *menu)
{
    preview_nvram_fixture(&menu->backup);
    menu->tools = TOOL_ARCHIVE;
    menu->tool_selected = 0;
    {
        unsigned char original = menu->backup.entries[0].data[0];
        SaveArchive parsed;
        memset(&parsed, 0, sizeof(parsed));
        tools_input(menu, ControlC);
        if (menu->tools != TOOL_HEX || menu->hex_dirty || menu->hex_archive.count != 3)
            abort();
        tools_input(menu, ControlA);
        tools_input(menu, ControlUp);
        tools_input(menu, ControlB);
        if (menu->hex_dirty || menu->hex_archive.entries[0].data[0] != original)
            abort();
        tools_input(menu, ControlA);
        tools_input(menu, ControlRight);
        tools_input(menu, ControlUp);
        tools_input(menu, ControlA);
        if (!menu->hex_dirty || menu->hex_archive.entries[0].data[0] == original ||
            menu->hex_original.entries[0].data[0] != original)
            abort();
        tools_input(menu, ControlC);
        if (menu->tools != TOOL_SLOTS || !menu->backup_mode || preview_nvram_writes)
            abort();
        if (save_archive_parse(&parsed, menu->pending.bytes, menu->pending.length) < 0 ||
            parsed.count != 1 || parsed.entries[0].data[0] == original)
            abort();
        save_archive_free(&parsed);
        /* Reviewing a destination replaces the working USB listing, not
         * the editor's preserved source archive. */
        preview_nvram_fixture(&menu->backup);
        menu->tools = TOOL_BACKUP_DETAILS;
        menu->backup_slot = 6;
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_SLOTS)
            abort();
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_HEX || !menu->hex_dirty)
            abort();
        toggle_tools(menu);
        if (menu->tools != TOOL_HEX_DISCARD)
            abort();
        tools_input(menu, ControlB);
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_HEX_DISCARD)
            abort();
        tools_input(menu, ControlA);
        if (menu->tools != TOOL_ARCHIVE || menu->backup.entries[0].data[0] != original ||
            menu->hex_archive.bytes || preview_nvram_writes)
            abort();
    }
    {
        SaveEntry entry;
        unsigned char data[65];
        memset(&entry, 0, sizeof(entry));
        memset(data, 255, sizeof(data));
        strcpy(entry.name, "Bounds");
        entry.size = sizeof(data);
        entry.data = data;
        save_archive_free(&menu->backup);
        if (save_archive_build(&menu->backup, &entry, 1) < 0)
            abort();
        menu->save_selected = 0;
        open_hex(menu);
        tools_input(menu, ControlRightShift);
        if (menu->hex_offset != 32)
            abort();
        tools_input(menu, ControlRightShift);
        if (menu->hex_offset != 64)
            abort();
        tools_input(menu, ControlRightShift);
        tools_input(menu, ControlRight);
        tools_input(menu, ControlDown);
        if (menu->hex_offset != 64)
            abort();
        tools_input(menu, ControlA);
        tools_input(menu, ControlUp);
        tools_input(menu, ControlA);
        if (menu->hex_archive.entries[0].data[64] != 15)
            abort();
        tools_input(menu, ControlLeftShift);
        tools_input(menu, ControlLeftShift);
        tools_input(menu, ControlLeftShift);
        if (menu->hex_offset)
            abort();
        close_hex(menu);
        save_archive_free(&menu->backup);
        entry.size = 0;
        if (save_archive_build(&menu->backup, &entry, 1) < 0)
            abort();
        open_hex(menu);
        tools_input(menu, ControlA);
        tools_input(menu, ControlRightShift);
        draw(menu);
        if (menu->hex_offset || menu->hex_editing || menu->hex_dirty)
            abort();
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_ARCHIVE || preview_nvram_writes)
            abort();
    }
    {
        SaveEntry entry;
        unsigned char data[40];
        unsigned long words[10] = {0x4c57, 2, 1, 15, 15, 3, 1, 2, 0, 12345};
        size_t i;
        SaveArchive parsed;
        memset(&parsed, 0, sizeof(parsed));
        memset(&entry, 0, sizeof(entry));
        for (i = 0; i < 9; ++i)
            words[9] += words[i];
        for (i = 0; i < 10; ++i)
            save_put_word(data + i * 4, words[i]);
        strcpy(entry.name, "DoomPrefs");
        entry.data = data;
        entry.size = 40;
        save_archive_free(&menu->backup);
        if (save_archive_build(&menu->backup, &entry, 1) < 0)
            abort();
        menu->tools = TOOL_ARCHIVE;
        menu->tool_selected = 0;
        tools_input(menu, ControlX);
        if (menu->tools != TOOL_CHEATS || menu->hex_dirty)
            abort();
        draw(menu);
        tools_input(menu, ControlA);
        if (!menu->hex_dirty || save_word(menu->hex_archive.entries[0].data + 24) != 23 ||
            save_word(menu->hex_original.entries[0].data + 24) != 1 ||
            !save_cheats_find(&menu->hex_archive.entries[0]))
            abort();
        tools_input(menu, ControlC);
        if (menu->tools != TOOL_SLOTS || preview_nvram_writes ||
            save_archive_parse(&parsed, menu->pending.bytes, menu->pending.length) < 0 ||
            !save_cheats_find(&parsed.entries[0]))
            abort();
        save_archive_free(&parsed);
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_CHEATS)
            abort();
        tools_input(menu, ControlDown);
        tools_input(menu, ControlA);
        if (save_word(menu->hex_archive.entries[0].data + 4) != 2)
            abort();
        tools_input(menu, ControlRight);
        if (save_word(menu->hex_archive.entries[0].data + 4) != 3 ||
            !save_cheats_find(&menu->hex_archive.entries[0]))
            abort();
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_HEX_DISCARD)
            abort();
        tools_input(menu, ControlB);
        if (menu->tools != TOOL_CHEATS)
            abort();
        tools_input(menu, ControlX);
        if (menu->tools != TOOL_HEX)
            abort();
        tools_input(menu, ControlX);
        if (menu->tools != TOOL_CHEATS)
            abort();
        tools_input(menu, ControlB);
        tools_input(menu, ControlA);
        if (menu->tools != TOOL_ARCHIVE || save_word(menu->backup.entries[0].data + 24) != 1 ||
            preview_nvram_writes)
            abort();
        menu->tools = TOOL_ARCHIVE;
        menu->tool_selected = 0;
        tools_input(menu, ControlC);
        ((unsigned char *)menu->hex_archive.entries[0].data)[0] ^= 1;
        tools_input(menu, ControlX);
        if (menu->tools != TOOL_HEX)
            abort();
        close_hex(menu);
    }
    menu->ode.navigation_failed = 1;
    menu->tools = TOOL_HOME;
    menu->tool_selected = 4;
    tools_input(menu, ControlA);
    draw(menu);
    if (menu->tools != TOOL_DIAG || preview_nvram_writes)
        abort();
    tools_input(menu, ControlRight);
    draw(menu);
    tools_input(menu, ControlRight);
    draw(menu);
    tools_input(menu, ControlRight);
    draw(menu);
    if (menu->tool_selected != 3)
        abort();
    tools_input(menu, ControlRight);
    draw(menu);
    if (menu->tool_selected != 4)
        abort();
    tools_input(menu, ControlRight);
    if (menu->tool_selected)
        abort();
    tools_input(menu, ControlLeft);
    if (menu->tool_selected != 4)
        abort();
    menu->ode.navigation_failed = 0;
}

/* Filtering leaves source entries and directory IDs intact. */
static void check_filtering(Menu *menu)
{
    {
        const MenuEntry hidden = {123, 0, "boot.iso"};
        const MenuEntry game = {456, 0, "Game.iso"};
        const MenuEntry cue = {457, 0, "Game.cue"};
        const MenuEntry bin = {458, 0, "Game.BIN"};
        const MenuEntry volume = {0x01000000UL, 1, "covers"};
        const MenuEntry *entries[4] = {&hidden, &game, &cue, &bin};
        OdeListing original = menu->listing;
        menu->listing.entries = entries;
        menu->listing.count = 4;
        menu->listing.has_parent = 0;
        menu->depth = 0;
        if (row_count(menu) != 2 || browser_entry(menu, 0) != &game ||
            browser_entry(menu, 1) != &cue || menu->listing.count != 4 ||
            browser_entry(menu, 0)->toc_id != 456 || browser_entry(menu, 1)->toc_id != 457)
            abort();
        menu->depth = 1;
        menu->path_ids[0] = 9;
        if (row_count(menu) != 3 || browser_entry(menu, 0) != &hidden)
            abort();
        menu->path_ids[0] = 0x01000000UL;
        if (row_count(menu) != 2)
            abort();
        if (hidden_entry(menu, &volume))
            abort();
        menu->listing = original;
    }
}

/* All INI controls; navigation stays active until reboot, bypass never writes. */
static void check_settings_controls(Menu *menu)
{
    menu->tools = TOOL_CONFIG;
    menu->tool_selected = 0;
    menu->config.screen_margin = 0;
    tools_input(menu, ControlLeft);
    if (menu->config.screen_margin != 12)
        abort();
    menu->tool_selected = 1;
    tools_input(menu, ControlA);
    if (!menu->config.large_titles)
        abort();
    menu->settings_folder_workaround = menu->config.folder_workaround = 1;
    menu->settings_usb_covers = menu->config.usb_covers = 0;
    menu->tool_selected = 2;
    tools_input(menu, ControlA);
    menu->tool_selected = 3;
    tools_input(menu, ControlRight);
}

/* The workaround setting cycles automatic, always, off: from always it goes off. */
static void check_workaround_setting(Menu *menu)
{
    if (menu->settings_folder_workaround != MENU_WORKAROUND_OFF || !menu->settings_usb_covers ||
        !menu->config.folder_workaround || menu->config.usb_covers)
        abort();
    menu->config_result = 2;
    tools_input(menu, ControlC);
    if (strcmp(menu->tool_message, "Load a valid INI before saving."))
        abort();
    menu->tool_selected = 9;
    tools_input(menu, ControlA);
    if (menu->config.screen_margin || menu->config.large_titles)
        abort();
    if (menu->settings_folder_workaround != MENU_WORKAROUND_OFF || !menu->settings_usb_covers)
        abort();
    tools_input(menu, ControlB);
    menu->tool_selected = 2;
    tools_input(menu, ControlA);
    if (menu->tools != TOOL_CONFIG || menu->settings_folder_workaround != MENU_WORKAROUND_OFF ||
        !menu->settings_usb_covers)
        abort();
    tools_input(menu, ControlUp);
    if (menu->tool_selected != 11)
        abort();
    tools_input(menu, ControlA);
    if (strcmp(menu->tool_message, "Load a valid INI before saving."))
        abort();
    menu->config_result = 0;
    menu->ready = 1;
    menu->demo = 1;
    menu->saved_config = menu->config;
    menu->tool_selected = 7;
    tools_input(menu, ControlA);
    if (menu->config.preview_sound || !config_dirty(menu))
        abort();
    tools_input(menu, ControlX);
    if (!menu->config.preview_sound || config_dirty(menu))
        abort();
    menu->demo = 1;
}

/* Unsaved marker tracks all settings; discard restores the saved snapshot. */
static void check_unsaved_settings(Menu *menu)
{
    menu->saved_config = menu->config;
    menu->settings_folder_workaround = menu->config.folder_workaround;
    menu->settings_usb_covers = menu->config.usb_covers;
    menu->settings_remember_position = menu->config.remember_position;
    menu->settings_recent_games = menu->config.recent_games;
    if (config_dirty(menu))
        abort();
    menu->tool_selected = 4;
    tools_input(menu, ControlA);
    if (!config_dirty(menu))
        abort();
    tools_input(menu, ControlX);
    if (config_dirty(menu))
        abort();
    menu->tools = TOOL_FIND;
    menu->find_query[0] = 0;
    menu->find_key = 0;
    tools_input(menu, ControlA);
    tools_input(menu, ControlRight);
    tools_input(menu, ControlA);
    if (strcmp(menu->find_query, "AB"))
        abort();
    tools_input(menu, ControlX);
    if (strcmp(menu->find_query, "A"))
        abort();
    tools_input(menu, ControlC);
    if (*menu->find_query)
        abort();
    tools_input(menu, ControlLeft);
    if (menu->find_key != 0)
        abort();
    tools_input(menu, ControlA);
    tools_input(menu, ControlStart);
    if (menu->tools != TOOL_FIND || !menu->tool_message)
        abort();
    menu->demo = 0;
    menu->ready = 1;
    menu->depth = 0;
    menu->catalog.valid = 1;
    menu->catalog.paths = (char **)calloc(2, sizeof(char *));
    menu->catalog.paths[0] = strcpy((char *)malloc(40), "/Storage/Games/Alpha.iso");
    menu->catalog.paths[1] = strcpy((char *)malloc(40), "/Storage/Other/Beta.iso");
    menu->catalog.count = 2;
    tools_input(menu, ControlStart);
    if (menu->tools != TOOL_RESULTS || library_catalog_count(&menu->catalog, menu->find_query) != 2)
        abort();
    tools_input(menu, ControlB);
    if (menu->tools != TOOL_FIND)
        abort();
    library_catalog_free(&menu->catalog);
    menu->demo = 1;
    menu->tools = 0;
    toggle_tools(menu);
    if (menu->tools != TOOL_HOME)
        abort();
    menu->tools = TOOL_FIND;
    toggle_tools(menu);
    if (menu->tools || menu->review)
        abort();
    menu->tools = TOOL_CONFIG;
    toggle_tools(menu);
    if (menu->tools)
        abort();
    preview_buttons = ControlB;
    if (!search_progress(menu, 0) || menu->suppress_buttons != ControlB)
        abort();
    preview_buttons = 0;
    menu->suppress_buttons = 0;
    menu->filter[0] = 0;
}

/* Move cancellation is local and never writes to the demo USB bus. */
static void check_favorite_move_cancel(Menu *menu)
{
    menu_favorites_parse(&menu->favorites, "First.iso\nSecond.iso\n", 21);
    menu->favorites_valid = 1;
    menu->tools = TOOL_FAVORITES;
    menu->tool_selected = 0;
    tools_input(menu, ControlRightShift);
    tools_input(menu, ControlDown);
    if (!menu->favorites_move || strcmp(menu->favorites.paths[0], "Second.iso"))
        abort();
    tools_input(menu, ControlB);
    if (menu->favorites_move || strcmp(menu->favorites.paths[0], "First.iso"))
        abort();
    tools_input(menu, ControlRightShift);
    tools_input(menu, ControlDown);
    toggle_tools(menu);
    if (menu->tools || menu->favorites_move || strcmp(menu->favorites.paths[0], "First.iso"))
        abort();
    menu_favorites_free(&menu->favorites);
}

/* Actual persistence controllers, with only the USB file transport replaced. */
static void check_persistence(Menu *menu)
{
    preview_text_enabled = 1;
    preview_reads = preview_writes = 0;
    menu->demo = menu->tools = menu->review = 0;
    menu->ready = 1;
    menu->depth = 0;
    menu->selected = 2;
    menu->config_result = 0;
    menu->config.remember_position = menu->config.recent_games = 1;
    strcpy(menu->ode.identity, "LOCODE");
    menu->ode.context = &menu->bus;
    menu->bus.timed_out = menu->ode.navigation_failed = 0;
    menu->position_result = 0;
    menu->position_dirty = 1;
    memset(&menu->last_position, 0, sizeof(menu->last_position));
    menu->selection_since = 0;
    preview_now = 9000;
    remember_position(menu, 0);
    if (preview_writes != 1 || menu->position_result != 1 ||
        !strstr(preview_position, "/Storage\n"))
        abort();
    remember_position(menu, 0);
    if (preview_writes != 1)
        abort();
    menu->selected = 3;
    selection_changed(menu);
    remember_position(menu, 0);
    if (preview_writes != 1)
        abort();
    preview_now += 2000;
    remember_position(menu, 0);
    if (preview_writes != 2)
        abort();
    menu->config_result = 2;
    menu->selected = 4;
    remember_position(menu, 1);
    if (preview_writes != 2)
        abort();
    menu->config_result = 0;
    menu->config.remember_position = 0;
    record_launch(menu, "/Storage/Games/D (Disc 1).iso");
    if (menu->recent_result != 1 || strcmp(preview_recent, "Games/D (Disc 1).iso\n"))
        abort();
    record_launch(menu, "/Storage/Other.iso");
    if (strcmp(preview_recent, "Other.iso\nGames/D (Disc 1).iso\n"))
        abort();
    {
        int reads = preview_reads, writes = preview_writes;
        record_launch(menu, "/Storage/Other.iso");
        if (preview_reads != reads || preview_writes != writes)
            abort();
    }
    {
        int writes = preview_writes;
        menu->recent_valid = 0;
        strcpy(preview_recent, "../bad.iso\n");
        record_launch(menu, "/Storage/New.iso");
        if (preview_writes != writes || menu->recent_result != -1)
            abort();
        preview_read_fail = 1;
        record_launch(menu, "/Storage/New.iso");
        if (preview_writes != writes)
            abort();
        preview_read_fail = 0;
        preview_recent[0] = 0;
        preview_write_fail = 1;
        record_launch(menu, "/Storage/New.iso");
        if (menu->recent_result != -1)
            abort();
        preview_write_fail = 0;
    }
    menu->bus.last_command = 0x93;
    menu->bus.failed_command = 0xe3;
    menu->bus.failed_status = 0x10;
    export_diagnostics(menu);
    if (menu->diagnostics_result != 1 || !strstr(preview_diagnostics, "last_command=93\n") ||
        !strstr(preview_diagnostics, "last_fault_command=e3\n") || menu->bus.last_command != 0xe4)
        abort();
    {
        int reads = preview_reads, writes = preview_writes;
        menu->bus.timed_out = 1;
        export_diagnostics(menu);
        if (menu->diagnostics_result != -1 || preview_reads != reads || preview_writes != writes)
            abort();
        menu->bus.timed_out = 0;
        menu->config_result = 2;
        export_diagnostics(menu);
        if (preview_reads != reads || preview_writes != writes)
            abort();
        menu->config_result = 0;
    }
    strcpy(preview_favorites, "First.iso\nSecond.iso\n");
    menu_favorites_parse(&menu->favorites, preview_favorites, strlen(preview_favorites));
    menu->favorites_valid = 1;
    menu->tools = TOOL_FAVORITES;
    menu->tool_selected = 0;
    begin_favorites_move(menu);
    menu_favorites_move(&menu->favorites, 0, 1);
    save_favorites_order(menu);
    if (menu->favorites_move || strcmp(preview_favorites, "Second.iso\nFirst.iso\n"))
        abort();
    begin_favorites_move(menu);
    menu_favorites_move(&menu->favorites, 0, 1);
    strcpy(preview_favorites, "PC changed.iso\n");
    {
        int writes = preview_writes;
        save_favorites_order(menu);
        if (!menu->favorites_move || preview_writes != writes ||
            !strstr(menu->tool_message, "changed"))
            abort();
    }
    cancel_favorites_move(menu);
    menu_favorites_free(&menu->favorites);
    menu_favorites_free(&menu->recent);
    menu->config.remember_position = 1;
    menu->position_result = 0;
    menu->selected = 5;
    preview_write_fail = 1;
    remember_position(menu, 1);
    if (menu->position_result != -1)
        abort();
    {
        int writes = preview_writes;
        remember_position(menu, 1);
        if (preview_writes != writes)
            abort();
    }
    preview_write_fail = 0;
    preview_text_enabled = 0;
    menu->config_result = 2;
    menu->demo = 1;
    menu->tools = 0;
}

/* Actual alphabet focus/input: cancel is local, confirm only selects. */
static void check_alphabet(Menu *menu)
{
    menu->tools = menu->review = 0;
    menu->ready = 1;
    menu->selected = 0;
    menu->c_pending = 1;
    if (!alphabet_input(menu, ControlUp) || !menu->alphabet_active || menu->c_pending)
        abort();
    alphabet_input(menu, ControlLeft);
    alphabet_input(menu, ControlRight);
    menu->jump_letter = 0;
    alphabet_input(menu, ControlLeft);
    if (menu->jump_letter != 26)
        abort();
    alphabet_input(menu, ControlRight);
    if (menu->jump_letter)
        abort();
    {
        size_t selected = menu->selected;
        unsigned long commands = menu->bus.commands;
        unsigned char available[27];
        alphabet_available(menu, available);
        if (!available[4] || available[26])
            abort();
        menu->jump_letter = 26;
        alphabet_input(menu, ControlA);
        if (!menu->alphabet_active || menu->selected != selected)
            abort();
        alphabet_input(menu, ControlC | ControlStart | ControlX);
        if (!menu->alphabet_active || menu->c_pending || menu->bus.commands != commands)
            abort();
        alphabet_input(menu, ControlB);
        if (menu->alphabet_active || menu->selected != selected)
            abort();
        alphabet_input(menu, ControlUp);
        menu->jump_letter = 4;
        alphabet_input(menu, ControlA);
        if (menu->alphabet_active || menu_initial(browser_entry(menu, menu->selected)->name) != 4 ||
            menu->bus.commands != commands)
            abort();
    }
    menu->selected = row_count(menu) - 1;
    if (!alphabet_input(menu, ControlDown) || !menu->alphabet_active)
        abort();
    alphabet_input(menu, ControlDown);
    if (menu->alphabet_active || menu->selected)
        abort();
    strcpy(menu->filter, "Road");
    {
        unsigned char available[27];
        alphabet_available(menu, available);
        if (!available[18] || available[4])
            abort();
    }
    menu->filter[0] = 0;
    menu->review = 1;
    menu->alphabet_active = 1;
    if (alphabet_input(menu, ControlUp) || menu->alphabet_active)
        abort();
    menu->review = 0;
    menu->ready = 0;
    if (alphabet_input(menu, ControlUp) || menu->alphabet_active)
        abort();
    menu->ready = 1;
    menu->demo = 0;
    menu->selected = 2;
    menu->c_pending = 1;
    if (browser_shortcuts(menu, ControlC | ControlUp, ControlUp, 0) || !menu->alphabet_active ||
        menu->c_pending)
        abort();
    if (browser_shortcuts(menu, 0, 0, ControlC) || menu->c_pending)
        abort();
    alphabet_input(menu, ControlB);
}

/* Holding C for a letter jump never queues an image. */
static void check_letter_jump_no_image(Menu *menu)
{
    menu->tools = 0;
    menu->review = menu->demo = 0;
    menu->ready = 1;
    menu->playlist.synchronized = 1;
    if (browser_shortcuts(menu, ControlC, ControlC, 0) & ControlC)
        abort();
    browser_shortcuts(menu, ControlC | ControlRight, ControlRight, 0);
    browser_shortcuts(menu, 0, 0, ControlC);
    if (menu->playlist.count || menu->c_pending)
        abort();
    menu->demo = 1;
}

/* Only visible favorites participate in selection and reorder; the
 * complete file, including hidden slots, survives moving/canceling. */
static void check_favorites_visible_only(Menu *menu)
{
    {
        static const char favorites[] =
            "Adult/Japan/Hidden.iso\nGames/A.iso\nAdult/USA/Hidden2.iso\nGames/B.iso\n";
        char out[FAVORITES_BYTES];
        menu->tools = TOOL_FAVORITES;
        menu->config.hide_adult = 1;
        menu->favorites_valid = 1;
        menu->tool_selected = 0;
        if (menu_favorites_parse(&menu->favorites, favorites, strlen(favorites)))
            abort();
        tools_input(menu, ControlRightShift);
        if (!menu->favorites_move)
            abort();
        tools_input(menu, ControlDown);
        if (menu->tool_selected != 1 ||
            strcmp(menu->favorites.paths[0], "Adult/Japan/Hidden.iso") ||
            strcmp(menu->favorites.paths[1], "Games/B.iso") ||
            strcmp(menu->favorites.paths[2], "Adult/USA/Hidden2.iso") ||
            strcmp(menu->favorites.paths[3], "Games/A.iso"))
            abort();
        if (menu_favorites_format(&menu->favorites, out, sizeof(out)) ||
            !strstr(out, "Hidden2.iso"))
            abort();
        tools_input(menu, ControlB);
        if (menu->favorites_move || menu_favorites_format(&menu->favorites, out, sizeof(out)) ||
            strcmp(out, favorites))
            abort();
        menu_favorites_free(&menu->favorites);
        menu->config.hide_adult = 0;
        menu->tools = 0;
        menu->tool_selected = 0;
    }
}

/* A folder holding one game (a disc, or a complete disc set) acts as
 * that game; nested folders or unrelated discs keep it a folder. */
static void check_single_game_folders(Menu *menu)
{
    {
        static LibraryGameInfo games[] = {{"A/A (USA).iso", "Racing", "Pub", "1994"},
                                          {"B/B (Disc 1).iso", "", "", ""},
                                          {"B/B (Disc 2).iso", "", "", ""},
                                          {"C/x/C.iso", "", "", ""},
                                          {"D/D.iso", "", "", ""},
                                          {"D/E.iso", "", "", ""}};
        const MenuEntry a = {10, 1, "A"}, b = {11, 1, "b"}, c = {12, 1, "C"}, d = {13, 1, "D"},
                        disc = {14, 0, "A (USA).iso"};
        const char *discs[ODE_MAX_DISCS];
        LibraryMetadata saved = menu->metadata;
        unsigned int depth = menu->depth;
        menu->metadata.entries = games;
        menu->metadata.count = 6;
        menu->depth = 0;
        if (folder_game(menu, &a, discs) != 1 || strcmp(discs[0], "A (USA).iso"))
            abort();
        if (folder_game(menu, &b, discs) != 2 || strcmp(discs[0], "B (Disc 1).iso") ||
            strcmp(discs[1], "B (Disc 2).iso"))
            abort();
        if (folder_game(menu, &c, discs) || folder_game(menu, &d, discs) ||
            folder_game(menu, &disc, discs))
            abort();
        if (!game_entry(menu, &a) || game_entry(menu, &d) || !game_entry(menu, &disc))
            abort();
        if (game_info(menu, &a) != games || game_info(menu, &b) != games + 1 || game_info(menu, &d))
            abort();
        menu->metadata = saved;
        menu->depth = depth;
    }
}

/* A disc of a set queues itself and the discs after it; the last
 * disc alone, or a held C (single_disc), launches only that disc. */
static void check_disc_set_queue(Menu *menu)
{
    {
        const MenuEntry first = {1, 0, "D (Disc 1).iso"}, second = {2, 0, "D (Disc 2).iso"},
                        third = {3, 0, "D (Disc 3).iso"};
        const MenuEntry *entries[3] = {&second, &third, &first};
        OdeListing original = menu->listing;
        menu->listing.entries = entries;
        menu->listing.count = 3;
        menu->listing.has_parent = 0;
        menu->depth = 0;
        menu->playlist.synchronized = 1;
        if (!queue_set(menu, &first) || menu->playlist.count != 3 || !menu->playlist.dirty ||
            strcmp(menu->playlist.paths[0], "/Storage/D (Disc 1).iso") ||
            strcmp(menu->playlist.paths[2], "/Storage/D (Disc 3).iso"))
            abort();
        ode_playlist_dispose(&menu->playlist);
        menu->playlist.synchronized = 1;
        if (!queue_set(menu, &second) || menu->playlist.count != 2 ||
            strcmp(menu->playlist.paths[0], "/Storage/D (Disc 2).iso"))
            abort();
        ode_playlist_dispose(&menu->playlist);
        menu->playlist.synchronized = 1;
        if (queue_set(menu, &third) || menu->playlist.count)
            abort();
        menu->selected = 0;
        menu->ready = 1;
        jump_letter(menu, 4);
        if (menu->selected != 0)
            abort();
        menu->listing = original;
    }
    {
        Menu launching;
        memset(&launching, 0, sizeof(launching));
        launching.ready = 1;
        launching.preview_audio.status = 1;
        launching.playlist.synchronized = 1;
        launching.ode.transfer = preview_mount;
        launching.ode.context = &launching;
        start_queue(&launching);
        if (launching.preview_audio.status != 1 || preview_mounts || launching.launching)
            abort();
        if (ode_playlist_append_local(&launching.ode, &launching.playlist, "/Storage/Game.iso"))
            abort();
        launching.playlist.dirty = 0;
        start_queue(&launching);
        if (launching.preview_audio.status || preview_mounts != 1 || !launching.launching)
            abort();
        {
            int displayed = display_count;
            /* The mounted image owns the handoff. Its released browser
             * listing must never replace the last launch frame. */
            draw(&launching);
            if (display_count != displayed)
                abort();
        }
        ode_playlist_dispose(&launching.playlist);
    }
    save_archive_free(&menu->nvram);
    save_archive_free(&menu->pending);
    save_archive_free(&menu->backup);
    ode_listing_free(&menu->listing);
}

/* self-test: the menu's behavior: preview scheduling, playback and cancellation,
 * the alphabet bar, the browser, settings, Find, history, favorites and diagnostics.
 * The checks run in order on the same menu, each starting from the state the last
 * one left. Any failed check stops the program (see abort in host-stubs.c). */
static int run_self_test(Menu *menu, CCB *cover)
{
    check_corner_mask(menu);
    check_text_width_cache(menu);
    check_remote_hold(menu);
    check_folder_game_lookup(menu);
    check_title_variants(menu);
    check_scrolling_title_end(menu);
    check_filtered_root_loading(menu);
    check_directory_loading(menu);
    check_selection_clears_media(menu, cover);
    check_cover_scheduling(menu, cover);
    check_cover_cache_path(menu);
    check_ram_preview_playback(menu);
    check_queued_direction_after_cancel(menu);
    check_title_scroll_during_load(menu);
    check_cancel_during_blink(menu);
    check_preview_cover_swap(menu, cover);
    check_cancel_does_not_activate(menu);
    check_shoulder_chord_during_cancel(menu);
    check_shoulder_release(menu);
    check_root_index_cache(menu);
    check_details_read_only(menu);
    check_tools_navigation(menu);
    check_tools_never_write(menu);
    check_merge_review(menu);
    check_hex_editor(menu);
    check_filtering(menu);
    check_settings_controls(menu);
    check_workaround_setting(menu);
    check_unsaved_settings(menu);
    check_favorite_move_cancel(menu);
    check_persistence(menu);
    check_alphabet(menu);
    check_letter_jump_no_image(menu);
    check_favorites_visible_only(menu);
    check_single_game_folders(menu);
    check_disc_set_queue(menu);
    return 0;
}

/* gecko-playback: which parts of the screen each playback frame redraws. */
static int run_gecko_playback(Menu *menu)
{
    /* Playback: two full frames fill both screen buffers with the list
     * panel; later frames redraw only the picture side and caption. */
    static CCB video = {2, 0, 0, 0};
    int step;
    menu->selected = 2;
    video.ccb_Width = 160;
    video.ccb_Height = 120;
    menu->preview_cel = &video;
    menu->preview.table = (unsigned char *)"synthetic";
    menu->preview.frames = 1;
    menu->preview.fps = 15;
    for (step = 0; step < 4; ++step) {
        printf("FRAME %d\n", step);
        draw(menu);
    }
    if (menu->panel_frames != 2)
        abort();
    menu->selected = 3;
    printf("FRAME 4\n");
    draw(menu);
    if (menu->panel_frames != 1)
        abort();
    return 0;
}

/* gecko-tuning-progress: the static shown while a preview loads. */
static int run_gecko_tuning_progress(Menu *menu, CCB *cover)
{
    /* Loading: the loader polls for cancel before each 2 KiB block; the
     * static redraws about 15 times a second, fully only for the first two. */
    int step;
    menu->cover = cover;
    menu->tuning = 1;
    preview_now = 5000;
    for (step = 0; step < 5; ++step) {
        preview_now += 70;
        printf("FRAME %d\n", step);
        media_cancel(menu);
    }
    if (menu->tuning_draws != 5 || menu->tuning_full != 2 || menu->tuning_bytes != 5 * 2048)
        abort();
    return 0;
}

/* Set up the menu for a screen mode before it is drawn. Several blocks can apply to one
 * mode (every "gecko..." mode gets the gecko block first), so they run in this order. */
static void set_up_screen(Menu *menu, CCB *cover, const char *mode)
{
    if (!strcmp(mode, "cover")) {
        menu->covers_enabled = 1;
        menu->selected = 2;
        menu->cover = cover;
    }
    if (!strcmp(mode, "loading")) {
        draw(menu);
        menu->loading = 1;
        menu->ready = 0;
        menu->read_count = 37;
    }
    if (!strcmp(mode, "nested")) {
        menu->depth = 3;
        strcpy(menu->path[1], "Adventure");
        strcpy(menu->path[2], "Multi Disc");
        update_breadcrumb(menu);
    }
    if (!strcmp(mode, "queue")) {
        menu->review = 1;
        menu->demo = 0;
        menu->playlist.count = 2;
        menu->playlist.paths[0] = "/Storage/Wing Commander III/Disc 1.iso";
        menu->playlist.paths[1] = "/Storage/Wing Commander III/Disc 2.iso";
        menu->config.folder_workaround = 1;
    }
    if (!strcmp(mode, "tools"))
        menu->tools = TOOL_HOME;
    if (!strcmp(mode, "context-help"))
        menu->help_hold.active = 1;
    if (!strcmp(mode, "context-hex")) {
        menu->help_hold.active = 1;
        menu->tools = TOOL_HEX;
    }
    if (!strcmp(mode, "context-confirm")) {
        menu->help_hold.active = 1;
        menu->tools = TOOL_CONFIRM_RESTORE;
    }
    if (!strcmp(mode, "help"))
        menu->tools = TOOL_HELP;
    if (!strcmp(mode, "diagnostics")) {
        menu->tools = TOOL_DIAG;
        memcpy(menu->ode.identity, "LOCODE", 7);
        menu->ode.revision[0] = menu->ode.revision[1] = 1;
        {
            unsigned char raw[] = {0x93, 1, 'L', 'O', 'C', 'O', 'D', 'E', 1, 1, 0, 0};
            memcpy(menu->ode.identity_reply, raw, 12);
        }
    }
    if ((!strcmp(mode, "diagnostics-state") || !strcmp(mode, "diagnostics-checked"))) {
        menu->tools = TOOL_DIAG;
        menu->tool_selected = 1;
        menu->usb_tested = !strcmp(mode, "diagnostics-checked") ? 1 : 0;
    }
    if (!strcmp(mode, "diagnostics-preview")) {
        menu->tools = TOOL_DIAG;
        menu->tool_selected = 3;
        menu->preview_result = 1;
        menu->preview_load_ms = 31420;
        menu->preview_read_ms = 24100;
        menu->preview_check_ms = 30;
        menu->preview_navigation_ms = 7100;
        menu->preview_setup_ms = 190;
        menu->preview_bytes = 1015000;
        menu->preview_frames = 225;
        menu->preview_fps = 15;
        menu->preview_commands = 1620;
    }
    if (!strcmp(mode, "diagnostics-file")) {
        menu->tools = TOOL_DIAG;
        menu->tool_selected = 4;
        menu->ode.navigation_failed = menu->bus.timed_out = 1;
        strcpy(menu->ode.file_path, "Road Rash (USA)/.ohmy3do/cover-fde7132585997845f9befacb.3dc");
        menu->usb_art.count = 536;
        menu->usb_previews.count = 282;
        menu->metadata.count = 483;
    }
    if (!strcmp(mode, "saves")) {
        static const char names[] = "=EA_RRX\tRoad Rash\n*NFS_\tThe Need for Speed\n";
        if (save_names_parse(&menu->save_names, names, sizeof(names) - 1, "", 0))
            abort();
        menu->save_names_loaded = 1;
        menu->tools = TOOL_SAVES;
        scan_nvram(menu);
    }
    if (!strcmp(mode, "save-hex")) {
        preview_nvram_fixture(&menu->backup);
        menu->save_selected = 0;
        open_hex(menu);
    }
    if (!strcmp(mode, "save-usb")) {
        menu->tools = TOOL_SLOTS;
        menu->backup_mode = 0;
    }
    if (!strcmp(mode, "restore")) {
        preview_nvram_fixture(&menu->backup);
        menu->tools = TOOL_CONFIRM_RESTORE;
    }
    if (!strcmp(mode, "display")) {
        menu->tools = TOOL_CONFIG;
        menu->settings_usb_covers = menu->config.usb_covers;
        menu->settings_folder_workaround = menu->config.folder_workaround;
    }
    if (!strcmp(mode, "display-save")) {
        menu->tools = TOOL_CONFIG;
        menu->tool_selected = 11;
    }
    if (!strcmp(mode, "results")) {
        menu->tools = TOOL_RESULTS;
        strcpy(menu->find_query, "doom");
        menu->catalog.valid = 1;
        menu->catalog.paths = (char **)calloc(2, sizeof(char *));
        menu->catalog.paths[0] = "/Storage/Games/US/Doom.iso";
        menu->catalog.paths[1] = "/Storage/Games/Japan/Doom.cue";
        menu->catalog.count = 2;
        menu->search_count = 2;
        menu->search_matches[0] = 0;
        menu->search_matches[1] = 1;
    }
    if ((!strcmp(mode, "formats") || !strcmp(mode, "formats-large"))) {
        size_t i;
        for (i = 0; i < menu->listing.count; ++i) {
            MenuEntry *entry = (MenuEntry *)menu->listing.entries[i];
            if (!entry->is_directory && i % 2) {
                char *name = (char *)entry->name;
                size_t n = strlen(name);
                memcpy(name + n - 4, ".cue", 5);
            }
        }
        if (!strcmp(mode, "formats-large")) {
            menu->config.large_titles = 1;
            menu->config.screen_margin = 12;
        }
    }
    if (!strcmp(mode, "find")) {
        menu->tools = TOOL_FIND;
        strcpy(menu->find_query, "ROAD");
        menu->find_key = 17;
    }
    if (!strcmp(mode, "filtered"))
        strcpy(menu->filter, "disc 1");
    if (!strcmp(mode, "unsaved")) {
        menu->tools = TOOL_CONFIG;
        menu->settings_recent_games = 0;
    }
    if (!strcmp(mode, "recent")) {
        menu->tools = TOOL_RECENT;
        menu_favorites_parse(&menu->recent, "Games/D (Disc 1).iso\nRoad Rash.iso\n",
                             strlen("Games/D (Disc 1).iso\nRoad Rash.iso\n"));
    }
    if (!strcmp(mode, "alphabet")) {
        menu->selected = 0;
        alphabet_input(menu, ControlUp);
        menu->jump_letter = 4;
    }
    if (!strcmp(mode, "alphabet-large")) {
        menu->config.large_titles = 1;
        menu->config.screen_margin = 12;
        menu->selected = 0;
        alphabet_input(menu, ControlUp);
        menu->jump_letter = 18;
    }
    if (!strcmp(mode, "large"))
        menu->config.large_titles = 1;
    if (!strcmp(mode, "margin"))
        menu->config.screen_margin = 12;
    if (!strcmp(mode, "tools-margin")) {
        menu->tools = TOOL_HOME;
        menu->config.screen_margin = 12;
    }
    if (!strcmp(mode, "settings-margin")) {
        menu->tools = TOOL_CONFIG;
        menu->config.screen_margin = 12;
    }
    if (!strcmp(mode, "favorites")) {
        menu->tools = TOOL_FAVORITES;
        menu_favorites_parse(&menu->favorites, "Road Rash.iso\nGames/D (Disc 1).iso",
                             strlen("Road Rash.iso\nGames/D (Disc 1).iso"));
    }
    if (!strcmp(mode, "slot-details")) {
        preview_nvram_fixture(&menu->backup);
        menu->tools = TOOL_BACKUP_DETAILS;
        menu->backup_slot_valid = 1;
    }
    if (!strncmp(mode, "gecko", 5)) {
        static LibraryGameInfo gecko_info = {"Alone in the Dark.iso", "Adventure", "Infogrames",
                                             "1994-09-29"};
        menu->selected = 2;
        if (strcmp(mode, "gecko") && strcmp(mode, "gecko-folder")) {
            menu->metadata.entries = &gecko_info;
            menu->metadata.count = 1;
        }
        if (!strcmp(mode, "gecko-folder"))
            menu->selected = 0;
        if (!strcmp(mode, "gecko-cover"))
            menu->cover = cover;
        if (!strcmp(mode, "gecko-full")) {
            menu->cover = cover;
            menu->full_view = 1;
            menu->preview_off = 1;
            menu->preview_has = 1;
        }
        if (!strcmp(mode, "gecko-tools"))
            menu->tools = TOOL_HOME;
        if (!strcmp(mode, "gecko-settings")) {
            menu->tools = TOOL_CONFIG;
            menu->tool_selected = 2;
        }
        if (!strcmp(mode, "gecko-margin"))
            menu->config.screen_margin = 12;
        if (!strcmp(mode, "gecko-large"))
            menu->config.large_titles = 1;
        if (!strcmp(mode, "gecko-alphabet")) {
            menu->selected = 0;
            alphabet_input(menu, ControlUp);
        }
        if (!strcmp(mode, "gecko-folder-game")) {
            /* "Multi Disc" holds one game, so it lists and plays as that game. */
            static LibraryGameInfo folder_info = {"Multi Disc/Alone in the Dark.iso", "Adventure",
                                                  "Infogrames", "1994-09-29"};
            menu->metadata.entries = &folder_info;
            menu->metadata.count = 1;
            menu->selected = 1;
        }
        if (!strcmp(mode, "gecko-queued")) {
            menu->playlist.synchronized = 1;
            if (ode_playlist_append_local(&menu->ode, &menu->playlist, "/Storage/D (Disc 1).iso") <
                    0 ||
                ode_playlist_append_local(&menu->ode, &menu->playlist, "/Storage/D (Disc 2).iso") <
                    0)
                abort();
        }
        if (!strcmp(mode, "gecko-tuning")) {
            menu->cover = cover;
            menu->tuning = 1;
            menu->tuning_bytes = 700UL * 1024UL;
            preview_now = 1340;
        }
        menu->saved_config = menu->config;
    }
    if ((!strcmp(mode, "gecko-preview") || !strcmp(mode, "gecko-preview-full"))) {
        static CCB video = {2, 0, 0, 0};
        video.ccb_Width = 160;
        video.ccb_Height = 120;
        menu->preview_cel = &video;
        menu->preview.table = (unsigned char *)"synthetic";
        menu->preview.frames = 1;
        menu->preview.fps = 15;
    }
    if (!strncmp(mode, "metadata", 8)) {
        static LibraryGameInfo info = {"Alone in the Dark.iso", "Adventure", "Infogrames",
                                       "1994-09-29"};
        menu->metadata.entries = &info;
        menu->metadata.count = 1;
        menu->selected = 2;
        menu->cover = cover;
        if (!strcmp(mode, "metadata-large"))
            menu->config.large_titles = 1;
        if (!strcmp(mode, "metadata-margin"))
            menu->config.screen_margin = 12;
        if (!strcmp(mode, "metadata-missing"))
            menu->selected = 3;
        if (!strcmp(mode, "metadata-long")) {
            info.genre = "Adventure; Role-Playing; Strategy";
            info.publisher = "A very long publishing company name";
        }
    }
}

int main(int argc, char **argv)
{
    Menu menu;
    CCB logo = {1, 0, 0, 0}, cover = {2, 0, 0, 0};
    memset(&menu, 0, sizeof(menu));
    menu_config_defaults(&menu.config);
    menu.saved_config = menu.config;
    menu.settings_remember_position = menu.settings_recent_games = 1;
    menu.settings_usb_covers = 1;
    menu.settings_video_previews = menu.config.video_previews;
    menu.font_ccb = *GetCurrentFont()->font_CCB;
    menu.have_font = 1;
    logo.ccb_XPos = 16L << 16;
    logo.ccb_YPos = 8L << 16;
    logo.ccb_HDX = 1L << 20;
    logo.ccb_VDY = 1L << 16;
    cover.ccb_Width = 64;
    cover.ccb_Height = 80;
    cover.ccb_XPos = 236L << 16;
    cover.ccb_YPos = 86L << 16;
    cover.ccb_HDX = 1L << 20;
    cover.ccb_VDY = 1L << 16;
    show_demo(&menu);
    init_channel_gecko(&menu);
    menu.logo = &logo;
    menu.message = NULL;
    const char *mode = argc > 1 ? argv[1] : "";

    /* Modes that run their own checks. */
    if (!strcmp(mode, "tools-return"))
        return run_tools_return(&menu);
    if (!strcmp(mode, "font-measure-bench"))
        return run_font_measure_bench(&menu);
    if (!strcmp(mode, "fullscreen-render"))
        return run_fullscreen_render(&menu, &cover);
    if (!strcmp(mode, "idle-draw-bench"))
        return run_idle_draw_bench(&menu, &cover);
    if (!strcmp(mode, "browser-render"))
        return run_browser_render(&menu, &cover);
    if (!strcmp(mode, "self-test"))
        return run_self_test(&menu, &cover);

    /* Screen modes: set up, then draw once (printing the drawing commands). */
    set_up_screen(&menu, &cover, mode);
    if (!strcmp(mode, "gecko-playback"))
        return run_gecko_playback(&menu);
    if (!strcmp(mode, "gecko-tuning-progress"))
        return run_gecko_tuning_progress(&menu, &cover);
    if (!strcmp(mode, "gecko-preview-full"))
        menu.full_view = 2;
    draw(&menu);
    if (!strcmp(mode, "font-memory")) {
        unsigned long compact = 0, baseline = 0;
        int slot;
        for (slot = 0; slot < UI_TEXT_CACHE; ++slot)
            if (menu.text_cache[slot].cel) {
                compact += (unsigned long)menu.text_cache[slot].capacity_width / 2 * UI_FONT_HEIGHT;
                baseline += UI_FONT_STRIDE * UI_FONT_HEIGHT;
            }
        if (!baseline || compact >= baseline / 2)
            abort();
        fprintf(stderr, "Font texture storage: %lu bytes; previously %lu bytes\n", compact,
                baseline);
    }
    menu_favorites_free(&menu.favorites);
    save_archive_free(&menu.nvram);
    save_archive_free(&menu.backup);
    ode_listing_free(&menu.listing);
    return 0;
}
