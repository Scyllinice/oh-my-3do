#include "menu_help.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    MenuHelpHold h = {0, 0, 0};
    unsigned int topic, row;
    assert(menu_help_poll(&h, 100, 1, 1, 0, 0) == 0 && h.pending);
    assert(menu_help_poll(&h, 200, 0, 0, 1, 0) == 1 && !h.pending);
    assert(menu_help_poll(&h, 201, 0, 0, 0, 0) == 0);
    assert(menu_help_poll(&h, 300, 1, 1, 0, 0) == 0);
    assert(menu_help_poll(&h, 1049, 1, 0, 0, 0) == 0);
    assert(menu_help_poll(&h, 1050, 1, 0, 0, 0) == 2 && h.active);
    assert(menu_help_poll(&h, 1100, 0, 0, 1, 0) == 3);
    h.active = 0;
    assert(menu_help_poll(&h, 1200, 1, 1, 0, 0) == 0);
    assert(menu_help_poll(&h, 1210, 1, 0, 0, 1) == 0 && !h.pending);
    assert(menu_help_poll(&h, 1300, 0, 0, 1, 0) == 0);
    assert(menu_help_poll(&h, 1400, 1, 1, 0, 1) == 0 && !h.pending);
    assert(menu_help_poll(&h, ULONG_MAX - 100, 1, 1, 0, 0) == 0);
    assert(menu_help_poll(&h, 649, 0, 0, 1, 0) == 2);
    for (topic = 0; topic < HELP_TOPIC_COUNT; ++topic) {
        assert(strlen(menu_help_title((MenuHelpTopic)topic)) > 0);
        assert(strlen(menu_help_line((MenuHelpTopic)topic, 0)) > 0);
        for (row = 0; row < MENU_HELP_ROWS; ++row)
            assert(strlen(menu_help_line((MenuHelpTopic)topic, row)) <= 48);
    }
    assert(!strcmp(menu_help_line(HELP_TOPIC_COUNT, 0), ""));
    assert(!strcmp(menu_help_line(HELP_LIBRARY, MENU_HELP_ROWS), ""));
    puts("Context help tap/hold/cancel/wrap and topics passed");
    return 0;
}
