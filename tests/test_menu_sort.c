#include "menu_sort.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_pair(const char *first, const char *second)
{
    MenuEntry a = {99, 0, NULL};
    MenuEntry b = {1, 0, NULL};
    const MenuEntry *view[2];
    a.name = first;
    b.name = second;
    view[0] = &b;
    view[1] = &a;
    menu_sort(view, 2);
    assert(view[0] == &a && view[1] == &b);
    menu_sort(view, 2);
    assert(view[0] == &a && view[1] == &b);
}

static void check_directory(void)
{
    MenuEntry entries[] = {{6, 0, "Disc 10.iso"}, {2, 1, "zeta"},      {9, 0, "Disc 2.iso"},
                           {4, 1, "Alpha"},       {3, 0, "alpha.iso"}, {7, 0, "Disc 1.iso"}};
    const unsigned long expected[] = {4, 2, 3, 7, 9, 6};
    const MenuEntry *view[6];
    MenuEntry original[6];
    size_t i;
    memcpy(original, entries, sizeof(entries));
    for (i = 0; i < 6; ++i)
        view[i] = &entries[i];
    menu_sort(view, 6);
    for (i = 0; i < 6; ++i)
        assert(view[i]->toc_id == expected[i]);
    /* Selection must use this ID, not row index 4 + 1 == 5. */
    assert(view[4] == &entries[2] && view[4]->toc_id == 9);
    for (i = 0; i < 6; ++i) {
        assert(entries[i].toc_id == original[i].toc_id);
        assert(entries[i].is_directory == original[i].is_directory);
        assert(entries[i].name == original[i].name);
    }
}

static void check_full_listing(void)
{
    /* Deliberately span several 14-entry transport batches/UI pages. */
    MenuEntry entries[100];
    const MenuEntry *view[100];
    char names[100][32];
    int i;
    int run;
    for (i = 0; i < 100; ++i) {
        sprintf(names[i], "Game %d.iso", i + 1);
        entries[i].toc_id = (unsigned long)(100 - i);
        entries[i].is_directory = 0;
        entries[i].name = names[i];
        view[i] = &entries[i];
    }
    for (run = 0; run < 100; ++run) {
        for (i = 99; i > 0; --i) {
            int j = rand() % (i + 1);
            const MenuEntry *temp = view[i];
            view[i] = view[j];
            view[j] = temp;
        }
        menu_sort(view, 100);
        for (i = 0; i < 100; ++i)
            assert(view[i] == &entries[i]);
    }
}

int main(void)
{
    MenuEntry duplicate_a = {1, 0, "Same.iso"};
    MenuEntry duplicate_b = {2, 0, "Same.iso"};
    const MenuEntry *duplicates[2];
    const MenuEntry *single = &duplicate_a;
    menu_sort(NULL, 0);
    menu_sort(&single, 1);
    assert(single == &duplicate_a);
    check_pair("", "a");
    check_pair("Alpha.iso", "beta.iso");
    check_pair("disc 2.iso", "Disc 10.iso");
    check_pair("Disc 2 Part 3.iso", "Disc 2 Part 10.iso");
    check_pair("Game 999999999999999999999.iso", "Game 1000000000000000000000.iso");
    check_pair("Disc 02.iso", "Disc 2.iso");
    check_pair("Disc 002 Part 2.iso", "Disc 2 Part 10.iso");
    check_pair("Disc 0.iso", "Disc 1.iso");
    check_pair("Disc 0.iso", "Disc 00.iso");
    check_pair("GAME.iso", "game.iso");
    check_pair("a", "aa");
    duplicates[0] = &duplicate_b;
    duplicates[1] = &duplicate_a;
    menu_sort(duplicates, 2);
    assert(duplicates[0] == &duplicate_a);
    check_directory();
    check_full_listing();
    puts("menu_sort: all tests passed");
    return 0;
}
