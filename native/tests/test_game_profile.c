#include "game_profile.h"
#include "macho_loader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const struct lp32_game_profile *portal = lp32_profile_named("portal2");
    const struct lp32_game_profile *l4d = lp32_profile_named("left4dead2");
    assert(portal && l4d && l4d == lp32_profile_named("l4d2"));
    assert(l4d->steam_app_id == 550 && portal->steam_app_id == 620);
    assert(!strcmp(l4d->source_executable, "hl2_osx"));
    assert(!strcmp(l4d->source_game_directory, "left4dead2"));
    assert(l4d->thread_argument_is_direct && l4d->callee_pops_struct_return);
    unsetenv("LP32_GAME");

    struct macho_image32 image = {
        .entry_eip = portal->entry_eip, .max_address = portal->image_end,
    };
    assert(lp32_profile_select(&image) == 0 && lp32_profile() == portal);
    assert(lp32_profile_select_hint(&image, l4d) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    /* A title hint disambiguates matching fingerprints without bypassing them. */
    struct lp32_game_profile collision = *l4d;
    collision.entry_eip = portal->entry_eip;
    collision.image_end = portal->image_end;
    assert(lp32_profile_select_hint(&image, &collision) == 0 && lp32_profile() == &collision);
    assert(lp32_profile_is_source());
    setenv("LP32_GAME", "portal2", 1);
    assert(lp32_profile_select_hint(&image, l4d) == 0 && lp32_profile() == portal);
    setenv("LP32_GAME", "typo", 1);
    assert(lp32_profile_select_hint(&image, l4d) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    unsetenv("LP32_GAME");

    image.entry_eip = l4d->entry_eip;
    image.max_address = l4d->image_end;
    assert(lp32_profile_select(&image) == 0 && lp32_profile() == l4d);
    assert(lp32_profile_select_hint(&image, l4d) == 0 && lp32_profile() == l4d);
    image.entry_eip = 0;
    image.max_address = 0;
    assert(lp32_profile_select(&image) != 0);
    assert(!lp32_profile_is_source());
    const struct lp32_game_profile *lego = lp32_profile_named("pirates");
    image.entry_eip = lego->entry_eip;
    image.max_address = lego->image_end;
    assert(lp32_profile_select_hint(&image, lego) == 0);
    assert(!lp32_profile_is_source() && lp32_profile()->steam_app_id == 0);
    puts("Game profiles: PASS (identity, launcher hint, override, unknown images, ABI metadata)");
    return 0;
}
