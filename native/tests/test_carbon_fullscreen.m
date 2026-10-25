#include "../src/carbon_ui.m"
#include <assert.h>
#include <unistd.h>

static struct lp32_game_profile profile;
const struct lp32_game_profile *lp32_profile(void) { return &profile; }
int carbon_text_is_control(void *view) { (void)view; return 0; }
void *carbon_text_copy_value(void *view) { (void)view; return NULL; }
void carbon_text_set_value(void *view, void *value) { (void)view; (void)value; }
void objc_bridge32_note_agl_frame(void) {}
void audio_bridge32_note_frame_presented(void) {}
void objc_bridge32_install_test_input(void *window) { (void)window; }
int hitch_recorder_enabled;
uint64_t hitch_now(void) { return 0; }
void hitch_frame(uint64_t swap, uint64_t work, uint64_t flush, uint64_t present,
                 uint64_t target, bool active) {
  (void)swap; (void)work; (void)flush; (void)present; (void)target; (void)active;
}
static int input_active;
void objc_bridge32_carbon_focus_changed(int active) { input_active = active; }

static NSEvent *management_event;
static unsigned management_events_delivered;
@interface LP32CarbonEventTestApplication : NSApplication
@end
@implementation LP32CarbonEventTestApplication
- (void)sendEvent:(NSEvent *)event {
  if (management_event && event.type == NSEventTypeAppKitDefined &&
      event.subtype == management_event.subtype) {
    ++management_events_delivered;
    carbon_ui_sync_focus(); /* Event handlers may reenter the guest pump. */
  } else [super sendEvent:event];
}
@end

static void test_appkit_event_delivery(void) {
  NSEvent *key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
      modifierFlags:0 timestamp:0 windowNumber:0 context:nil characters:@"a"
      charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
  NSEvent *application = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
      location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
      subtype:0 data1:0 data2:0];
  management_event = [NSEvent otherEventWithType:NSEventTypeAppKitDefined
      location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
      subtype:NSEventSubtypeApplicationActivated data1:0 data2:0];
  [NSApp postEvent:key atStart:NO];
  [NSApp postEvent:application atStart:NO];
  [NSApp postEvent:management_event atStart:NO];
  carbon_ui_sync_focus();
  assert(!management_events_delivered); /* Background probes cannot activate. */
  unsetenv("LP32_BACKGROUND_TEST");
  carbon_ui_sync_focus();
  assert(management_events_delivered == 1);
  NSEvent *remaining_key = [NSApp nextEventMatchingMask:NSEventMaskKeyDown untilDate:[NSDate distantPast]
      inMode:NSDefaultRunLoopMode dequeue:YES];
  assert(remaining_key && [remaining_key.characters isEqualToString:@"a"]);
  NSEvent *remaining_application = [NSApp nextEventMatchingMask:NSEventMaskApplicationDefined
      untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
  assert(remaining_application && remaining_application.type == NSEventTypeApplicationDefined);
  management_event = nil;
  setenv("LP32_BACKGROUND_TEST", "1", 1);
}

static void *new_window(void) {
  Rect bounds = {100, 100, 340, 420};
  void *window = NULL;
  assert(!((OSStatus (*)(uint32_t, uint32_t, const Rect *, void **))api("CreateNewWindow"))(
      13, kWindowCompositingAttribute, &bounds, &window));
  return window;
}

static void dispose_window(void *window) {
  carbon_ui_dispose(window);
  ((void (*)(void *))api("DisposeWindow"))(window);
}

int main(void) {
  alarm(20);
  setenv("LP32_BACKGROUND_TEST", "1", 1);
  @autoreleasepool {
    [LP32CarbonEventTestApplication sharedApplication];
    NSApplicationLoad();
    NSApplicationPresentationOptions original = [NSApp presentationOptions];
    NSApplicationPresentationOptions normal = NSApplicationPresentationAutoHideDock |
        NSApplicationPresentationAutoHideMenuBar;
    NSApplicationPresentationOptions fullscreen = NSApplicationPresentationHideDock |
        NSApplicationPresentationHideMenuBar;
    [NSApp setPresentationOptions:normal];
    for (unsigned edition = 0; edition < 2; ++edition) {
      profile.title = edition ? LP32_TITLE_COD4_MP : LP32_TITLE_COD4;
      void *main = new_window(), *dialog = new_window();
      carbon_ui_paint(main, NULL);
      carbon_ui_show(main);
      LP32CarbonPanel *backing = panel_for(main, NO);
      assert([backing->window isVisible]);
      for (unsigned iteration = 0; iteration < 3; ++iteration) {
        carbon_ui_set_menu_bar_visible(NO);
        void *port = new_window();
        carbon_ui_fullscreen(port, CGMainDisplayID(), 640, 480);
        LP32CarbonPanel *surface = panel_for(port, NO);
        assert([surface->window isVisible] && ![backing->window isVisible]);
        assert([NSApp presentationOptions] == normal); /* Background test isolation. */
        [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationWillResignActiveNotification object:NSApp];
        assert(!input_active);
        [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidBecomeActiveNotification object:NSApp];
        assert(input_active);
        /* Activation must restore the GL presenter even if AppKit ordered it
           out while the legacy Carbon event window remained alive. */
        [surface->window orderOut:nil];
        unsetenv("LP32_BACKGROUND_TEST");
        if (!edition && !iteration) {
          AlertStdCFStringAlertParamRec params = {0};
          assert(!((OSStatus (*)(void *, uint32_t))api("GetStandardAlertDefaultParams"))(&params, 1));
          void *alert = NULL;
          assert(!((OSStatus (*)(uint16_t, CFStringRef, CFStringRef, void *, void **))api("CreateStandardAlert"))(
              1, CFSTR("Modal focus test"), CFSTR("Automated regression"), &params, &alert));
          carbon_ui_prepare_standard_alert(alert, 1, CFSTR("Modal focus test"), CFSTR("Automated regression"), 1, 2);
          __block BOOL tested = NO;
          CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL,
              CFAbsoluteTimeGetCurrent() + .05, 0, 0, 0, ^(CFRunLoopTimerRef timer) {
            CFRunLoopTimerInvalidate(timer);
            [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidBecomeActiveNotification object:NSApp];
            assert([NSApp modalWindow] && [NSApp modalWindow] != surface->window);
            assert(![surface->window isVisible]); /* Activation must not raise the game over the alert. */
            tested = YES;
            assert(carbon_ui_test_click_standard_alert(alert, 1));
          });
          CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, (CFStringRef)NSModalPanelRunLoopMode);
          int16_t item = 0;
          assert(!carbon_ui_run_standard_alert(alert, &item) && tested && item == 1);
          CFRelease(timer);
          assert(![NSApp modalWindow]);
        }
        carbon_application_focus_changed(YES);
        assert([surface->window isVisible] && ![backing->window isVisible]);
        assert(!lp32_suppress_background_input());
        assert([NSApp presentationOptions] == fullscreen);
        carbon_application_focus_changed(NO);
        assert(lp32_suppress_background_input());
        assert([NSApp presentationOptions] == normal);
        carbon_application_focus_changed(YES); /* Cmd-Tab back into fullscreen. */
        assert([NSApp presentationOptions] == fullscreen);
        carbon_ui_set_menu_bar_visible(YES);
        assert([NSApp presentationOptions] == normal);
        carbon_ui_set_menu_bar_visible(NO);
        assert([NSApp presentationOptions] == fullscreen);
        carbon_ui_hide(port);
        assert([NSApp presentationOptions] == normal);
        carbon_ui_show(port);
        assert([NSApp presentationOptions] == fullscreen);
        carbon_application_focus_changed(NO);
        setenv("LP32_BACKGROUND_TEST", "1", 1);
        carbon_ui_show(main);
        carbon_ui_paint(main, NULL);
        assert(carbon_ui_select(main));
        assert(![backing->window isVisible] && carbon_ui_is_visible(main));
        assert(!((Boolean (*)(void *))api("IsWindowVisible"))(main));
        carbon_ui_show(dialog);
        assert(carbon_ui_is_visible(dialog));
        carbon_ui_hide(dialog);
        if (iteration == 2) carbon_ui_hide(main);
        unsetenv("LP32_BACKGROUND_TEST");
        carbon_application_focus_changed(YES);
        dispose_window(port);
        assert([NSApp presentationOptions] == normal);
        carbon_application_focus_changed(NO);
        setenv("LP32_BACKGROUND_TEST", "1", 1);
        assert([backing->window isVisible] == (iteration != 2));
      }
      dispose_window(dialog);
      dispose_window(main);
      assert(!fullscreen_panel && ![panels count]);
    }
    test_appkit_event_delivery();
    [NSApp setPresentationOptions:original];
  }
  puts("Carbon fullscreen PASS (modal alert focus, AppKit activation delivery, reentrant pump, Carbon input preserved, SP/MP presenter and menu-bar restoration, background isolation, fullscreen exit, hide, disposal)");
}
