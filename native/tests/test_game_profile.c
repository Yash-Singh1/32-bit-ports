#include "game_profile.h"
#include "macho_loader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const struct lp32_game_profile *portal = lp32_profile_named("portal2");
    const struct lp32_game_profile *mel = lp32_profile_named("PortalStoriesMel");
    assert(mel && mel->steam_app_id == 317400);
    assert(!strcmp(mel->source_game_directory, "portal_stories"));
    assert(!strcmp(mel->source_data_directory, "PortalStoriesMel"));
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
    assert(lp32_profile_select(&image) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    assert(lp32_profile_select_hint(&image, portal) == 0 && lp32_profile() == portal);
    assert(lp32_profile_select_hint(&image, mel) == 0 && lp32_profile() == mel);
    assert(lp32_profile_is_source());
    assert(mel->thread_argument_is_direct && mel->callee_pops_struct_return);
    assert(lp32_profile_image_hint(NULL) == NULL);
    assert(lp32_profile_image_hint("portal2_osx") == NULL);
    assert(lp32_profile_image_hint("/tmp/Portal2.image.bak") == NULL);
    assert(lp32_profile_select_hint(&image, lp32_profile_image_hint("Portal2.image")) == 0);
    assert(lp32_profile() == portal);
    assert(lp32_profile_select_hint(&image, lp32_profile_image_hint("/tmp/PortalStoriesMel.image")) == 0);
    assert(lp32_profile() == mel);
    assert(lp32_profile_select_hint(&image, l4d) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    /* A title hint disambiguates matching fingerprints without bypassing them. */
    struct lp32_game_profile collision = *l4d;
    collision.entry_eip = portal->entry_eip;
    collision.image_end = portal->image_end;
    assert(lp32_profile_select_hint(&image, &collision) == 0 && lp32_profile() == &collision);
    assert(lp32_profile_is_source());
    setenv("LP32_GAME", "PortalStoriesMel", 1);
    assert(lp32_profile_select_hint(&image, portal) == 0 && lp32_profile() == mel);
    assert(lp32_profile_select(&image) == 0 && lp32_profile() == mel);
    setenv("LP32_GAME", "portal2", 1);
    assert(lp32_profile_select_hint(&image, mel) == 0 && lp32_profile() == portal);
    setenv("LP32_GAME", "typo", 1);
    assert(lp32_profile_select_hint(&image, l4d) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    unsetenv("LP32_GAME");

    image.entry_eip = l4d->entry_eip;
    image.max_address = l4d->image_end;
    assert(lp32_profile_select(&image) != 0); /* L4D1 shares this fingerprint. */
    assert(lp32_profile_select_hint(&image, l4d) == 0 && lp32_profile() == l4d);
    assert(lp32_profile_select_hint(&image, mel) != 0);
    assert(lp32_profile()->title == LP32_TITLE_UNKNOWN);
    const struct lp32_game_profile *l4d1 = lp32_profile_named("left4dead");
    assert(l4d1 && l4d1 == lp32_profile_named("l4d1"));
    assert(l4d1->steam_app_id == 500);
    assert(!strcmp(l4d1->source_game_directory, "left4dead"));
    assert(lp32_profile_select_hint(&image, l4d1) == 0 && lp32_profile() == l4d1);
    assert(lp32_profile_is_source());
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
