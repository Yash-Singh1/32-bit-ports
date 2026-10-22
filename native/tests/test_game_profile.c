#include "game_profile.h"
#include "macho_loader.h"
#include "mouse_buttons.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    unsetenv("LP32_GAME");
    const char *names[] = {"pirates", "clonewars", "marvel", "saga-retail", "LEGOCompleteSaga10", "saga"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const struct lp32_game_profile *profile = lp32_profile_named(names[i]);
        assert(profile);
        struct macho_image32 image = {
            .entry_eip = profile->entry_eip,
            .max_address = profile->image_end,
        };
        assert(lp32_profile_select(&image, NULL) == 0);
        assert(lp32_profile() == profile);
        /* Neither half of the fingerprint alone may select a patch layout. */
        ++image.max_address;
        assert(lp32_profile_select(&image, NULL) == -1);
        --image.max_address;
        ++image.entry_eip;
        assert(lp32_profile_select(&image, NULL) == -1);
        assert(profile->thread_argument_is_direct == (i >= 2));
        assert(profile->callee_pops_struct_return == (i == 2 || i == 5));
        assert((profile->steam_achievement_guard != NULL) == (i == 2));
        assert(!profile->loading_screen);
        assert(profile->steam_app_id == (i == 2 ? 249130u : 0u));
        if (profile->title != LP32_TITLE_COMPLETE_SAGA)
            assert(profile->controller && profile->display);
    }
    const char *cod_names[] = {"cod4", "cod4mp"};
    for (unsigned i = 0; i < 2; ++i) {
        const struct lp32_game_profile *p = lp32_profile_named(cod_names[i]);
        assert(p && p->steam_app_id == 7940 && p->callee_pops_struct_return);
        assert(!p->controller && !p->startup_latch && !p->steam_achievement_guard);
        assert(!p->code_patches); /* MW2 synchronization patches must not reach MW1. */
        assert((p->loading_screen != NULL) == (i == 0));
        struct macho_image32 image = {.entry_eip = p->entry_eip, .max_address = p->image_end,
                                     .import_count = 1};
        image.imports[0].name = "_SteamAPI_Init";
        assert(lp32_profile_select(&image, NULL) == 0 && lp32_profile() == p);
        ++image.max_address;
        assert(lp32_profile_select(&image, NULL) == -1);
        --image.max_address;
        ++image.entry_eip;
        assert(lp32_profile_select(&image, NULL) == -1);
        assert(p->depth_capability_check.length == 7);
        image.import_count = 0;
        assert(lp32_profile_select(&image, "COD4.image") == 0);
        assert(lp32_profile()->steam_app_id == 0);
        ++image.max_address;
        assert(lp32_profile_select(&image, i ? "COD4MP.image" : "COD4.image") == 0);
        assert(lp32_profile()->depth_capability_check.length == 0);
        assert(lp32_profile()->main_address == 0);
        assert(lp32_profile()->loading_screen == NULL);
    }
    const char *cod2_names[] = {"cod2", "cod2mp"};
    for (unsigned i = 0; i < 2; ++i) {
        const struct lp32_game_profile *p = lp32_profile_named(cod2_names[i]);
        assert(p && p->steam_app_id == 2630 && p->callee_pops_struct_return);
        assert(p->title == (i ? LP32_TITLE_COD2_MP : LP32_TITLE_COD2));
        assert(lp32_title_is_cod_carbon(p->title) && !lp32_title_is_mw_sdl(p->title));
        assert(!p->controller && !p->startup_latch && !p->code_patches && !p->loading_screen);
        assert(p->depth_capability_check.length == 4 && p->license_log_return_address);
        struct macho_image32 image = {.entry_eip = p->entry_eip, .max_address = p->image_end,
                                     .import_count = 1};
        image.imports[0].name = "_SteamAPI_Init";
        assert(lp32_profile_select(&image, NULL) == 0 && lp32_profile() == p);
        ++image.entry_eip;
        assert(lp32_profile_select(&image, NULL) == -1);
        /* An untested build keeps the title but none of the tested addresses. */
        image.import_count = 0;
        assert(lp32_profile_select(&image, i ? "COD2MP.image" : "COD2.image") == 0);
        assert(lp32_profile()->title == p->title && lp32_profile()->steam_app_id == 0);
        assert(!lp32_profile()->main_address && !lp32_profile()->license_log_return_address);
        assert(lp32_profile()->depth_capability_check.length == 0);
    }
    assert(lp32_title_is_cod_carbon(LP32_TITLE_COD4) && lp32_title_is_cod_carbon(LP32_TITLE_COD4_MP));
    assert(!lp32_title_is_cod_carbon(LP32_TITLE_MW2) && !lp32_title_is_cod_carbon(LP32_TITLE_MARVEL));
    assert(lp32_profile_named("cod2-mp") == lp32_profile_named("cod2mp"));
    const char *mw2_names[] = {"mw2", "mw2mp"};
    for (unsigned i = 0; i < 2; ++i) {
        const struct lp32_game_profile *p = lp32_profile_named(mw2_names[i]);
        assert(p && p->steam_app_id == (i ? 10190u : 10180u));
        assert(p->title == (i ? LP32_TITLE_MW2_MP : LP32_TITLE_MW2));
        assert(p->callee_pops_struct_return && p->thread_argument_is_direct);
        assert(!p->depth_capability_check.length && !p->loading_screen);
        assert(!p->license_log_return_address && !p->server_name_compare.length);
        assert(!p->main_address && !p->controller && !p->startup_latch);
        assert((p->code_patches != NULL) == (i == 0));
        struct macho_image32 image = {.entry_eip = 0x2ff7, .max_address = 0x21ee150};
        assert(lp32_profile_select(&image, i ? "/tmp/MW2MP.image" : "/tmp/MW2.image") == 0);
        assert(lp32_profile() == p);
        assert(lp32_profile_select(&image, "/tmp/unknown.image") == -1);
        assert(lp32_title_is_mw_sdl(p->title));
    }
    const char *mw3_names[] = {"mw3", "mw3mp"};
    for (unsigned i = 0; i < 2; ++i) {
        const struct lp32_game_profile *p = lp32_profile_named(mw3_names[i]);
        assert(p && p->steam_app_id == (i ? 42690u : 42680u));
        assert(p->title == (i ? LP32_TITLE_MW3_MP : LP32_TITLE_MW3));
        assert(p->callee_pops_struct_return && p->thread_argument_is_direct);
        assert(lp32_title_is_mw_sdl(p->title));
        /* Each MW3 executable carries its own byte-checked patches, never MW2's. */
        assert(p->code_patches && p->code_patches != lp32_profile_named(i ? "mw3" : "mw3mp")->code_patches);
        assert(p->code_patches != lp32_profile_named("mw2")->code_patches);
        assert(!p->depth_capability_check.length && !p->loading_screen);
        assert(!p->main_address && !p->controller && !p->startup_latch);
        struct macho_image32 image = {.entry_eip = 0x2ff7, .max_address = 0x21ee150};
        assert(lp32_profile_select(&image, i ? "/tmp/MW3MP.image" : "/tmp/MW3.image") == 0);
        assert(lp32_profile() == p);
    }
    assert(!lp32_title_is_mw_sdl(LP32_TITLE_COD4) && !lp32_title_is_mw_sdl(LP32_TITLE_COD4_MP));
    assert(lp32_profile_named("codmw3") == lp32_profile_named("mw3"));
    assert(lp32_profile_named("mw3-mp") == lp32_profile_named("mw3mp"));
    const struct lp32_game_profile *asphalt = lp32_profile_named("asphalt6");
    const struct lp32_game_profile *gangstar = lp32_profile_named("gangstar");
    assert(asphalt && gangstar && asphalt != gangstar);
    assert(asphalt->title == LP32_TITLE_ASPHALT6);
    assert(asphalt->callee_pops_struct_return && asphalt->thread_argument_is_direct);
    assert(!asphalt->code_patches && !asphalt->controller && !asphalt->steam_app_id);
    assert(asphalt->asphalt6_controller && !gangstar->asphalt6_controller);
    assert(asphalt->asphalt6_controller->commit_call.length == 5);
    assert(asphalt->asphalt6_controller->steering_call.length == 5);
    assert(lp32_title_is_gameloft(asphalt->title) && lp32_title_is_gameloft(gangstar->title));
    assert(!lp32_title_is_gameloft(LP32_TITLE_MARVEL));
    assert(!lp32_title_is_mw_sdl(asphalt->title) && !lp32_title_is_cod_carbon(asphalt->title));
    struct macho_image32 gameloft = {.entry_eip = 0x1234, .max_address = 0x800000};
    assert(lp32_profile_select(&gameloft, "/tmp/Asphalt6.image") == 0);
    assert(lp32_profile() == asphalt);
    assert(lp32_profile_select(&gameloft, "/tmp/Gangstar.image") == 0);
    assert(lp32_profile() == gangstar);
    struct macho_image32 empty = {0};
    assert(lp32_profile_select(&empty, NULL) == -1);
    assert(lp32_profile_named("codmw2") == lp32_profile_named("mw2"));
    assert(lp32_profile_named("mw2-mp") == lp32_profile_named("mw2mp"));
    assert(lp32_profile_named("cod") == lp32_profile_named("cod4"));
    assert(lp32_profile_named("cod4-mp") == lp32_profile_named("cod4mp"));
    assert(lp32_profile_named("LEGOMARVEL") == lp32_profile_named("marvel"));
    assert(lp32_profile_named("lsw3") == lp32_profile_named("clonewars"));
    assert(lp32_profile_named("lswc") == lp32_profile_named("saga"));
    assert(lp32_profile_named("completesaga") == lp32_profile_named("saga"));
    assert(lp32_profile_named("saga-steam") == lp32_profile_named("saga"));
    assert(lp32_profile_named("saga-retail") != lp32_profile_named("saga"));
    struct macho_image32 modern = {.entry_eip = 0x1234, .main_address = 0x1234};
    assert(lp32_profile_main_address(&modern) == 0x1234);
    assert(!lp32_profile_named(NULL));
    assert(!lp32_profile_named("unsupported"));
    struct macho_image32 unknown = {0};
    setenv("LP32_GAME", "unsupported", 1);
    assert(lp32_profile_select(&unknown, NULL) == -1);
    setenv("LP32_GAME", "marvel", 1);
    assert(lp32_profile_select(&unknown, NULL) == 0);
    assert(lp32_profile()->title == LP32_TITLE_MARVEL);
    unsetenv("LP32_GAME");
    uint32_t held = 0;
    assert(lp32_released_mouse_buttons(&held, 0) == 0 && held == 0);
    assert(lp32_released_mouse_buttons(&held, 0x1) == 0 && held == 0x1);
    assert(lp32_released_mouse_buttons(&held, 0x1) == 0 && held == 0x1);
    assert(lp32_released_mouse_buttons(&held, 0) == 0x1 && held == 0);
    held = 0x1f;
    assert(lp32_released_mouse_buttons(&held, 0x2) == 0x1d && held == 0x2);
    assert(lp32_released_mouse_buttons(&held, 0x20) == 0x2 && held == 0);
    puts("game-profile PASS (fingerprints, aliases, overrides, ABI isolation)");
    return 0;
}
