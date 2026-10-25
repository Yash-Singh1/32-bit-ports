#include "carbon_ui.h"
#include "objc_bridge.h"
#include "carbon_text.h"
#include "audio_bridge.h"
#include "game_profile.h"
#include "focus_policy.h"
#include "hitch_recorder.h"
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void *carbon_ui_copy_bundle(void *bundle) {
  return bundle ? (void *)CFBundleCreate(NULL, (CFURLRef)[(NSBundle *)bundle bundleURL]) : NULL;
}

/* Carbon remains the guest's control model and event target. AppKit presents
 * that model because several Carbon widgets no longer exist in 64-bit macOS. */
static void *carbon;
static void *api(const char *name) {
  if (!carbon)
    carbon = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon",
                    RTLD_NOW | RTLD_LOCAL);
  return dlsym(carbon, name);
}
@interface LP32FlippedView : NSView
@end
@implementation LP32FlippedView
- (BOOL)isFlipped {
  return YES;
}
@end
@interface LP32CarbonSurfaceWindow : NSWindow
@end
@implementation LP32CarbonSurfaceWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end
@interface LP32CarbonPanel : NSObject <NSWindowDelegate, NSTextFieldDelegate> {
@public
  void *native;
  NSWindow *window;
  NSMutableDictionary *controls;
  NSTimer *timer;
  NSImageView *bitmap;
  BOOL game_surface;
  int32_t surface_size[2];
  BOOL modal, native_rendering, syncing, requested_visible;
}
- (void)sync;
- (void)action:(NSControl *)sender;
@end
static NSMutableDictionary *panels;
static NSMutableDictionary *gl_contexts;
static LP32CarbonPanel *fullscreen_panel;
static BOOL carbon_focus_installed, carbon_app_active;
static BOOL carbon_restoring_focus, carbon_menu_bar_hidden, carbon_presentation_saved;
static NSApplicationPresentationOptions carbon_previous_presentation;
@interface LP32CarbonStandardAlert : NSAlert {
@public
  NSMutableArray *carbon_items;
}
@end
@implementation LP32CarbonStandardAlert
- (void)dealloc { [carbon_items release]; [super dealloc]; }
@end
static NSMutableDictionary *standard_alerts;
static unsigned standard_alert_depth;

static void carbon_apply_fullscreen_presentation(void) {
  BOOL fullscreen = carbon_app_active && fullscreen_panel && fullscreen_panel->requested_visible &&
      carbon_menu_bar_hidden && !getenv("LP32_BACKGROUND_TEST");
  if (fullscreen) {
    if (!carbon_presentation_saved) {
      carbon_previous_presentation = [NSApp presentationOptions];
      carbon_presentation_saved = YES;
    }
    NSApplicationPresentationOptions options = carbon_previous_presentation &
        ~(NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideDock);
    /* HideMenuBar requires HideDock. Keep Cmd-Tab and force quit available. */
    [NSApp setPresentationOptions:options | NSApplicationPresentationHideMenuBar |
        NSApplicationPresentationHideDock];
  } else if (carbon_presentation_saved) {
    [NSApp setPresentationOptions:carbon_previous_presentation];
    carbon_presentation_saved = NO;
  }
}

void carbon_ui_set_menu_bar_visible(int visible) {
  carbon_menu_bar_hidden = !visible;
  carbon_apply_fullscreen_presentation();
}

static BOOL carbon_process_is_frontmost(void) {
  ProcessSerialNumber front = {0}, current = {0};
  OSStatus (*get_front)(ProcessSerialNumber *) = api("GetFrontProcess");
  OSStatus (*get_current)(ProcessSerialNumber *) = api("GetCurrentProcess");
  if (get_front && get_current && !get_front(&front) && !get_current(&current))
    return front.highLongOfPSN == current.highLongOfPSN && front.lowLongOfPSN == current.lowLongOfPSN;
  return [[NSRunningApplication currentApplication] isActive];
}

static void carbon_application_focus_changed(BOOL active) {
  BOOL changed = active != carbon_app_active;
  carbon_app_active = active;
  lp32_set_managed_input_active(active);
  if (getenv("LP32_TRACE_INPUT")) fprintf(stderr, "compat32: Carbon application %s\n", active ? "active" : "inactive");
  if (active && !getenv("LP32_BACKGROUND_TEST") && !carbon_restoring_focus) {
    carbon_restoring_focus = YES;
    /* Carbon can be frontmost before AppKit completes activation. Raising a
       window alone leaves the previous application's menu bar on screen. */
    if (carbon_process_is_frontmost()) {
      if (@available(macOS 14.0, *)) [NSApp activate];
      else [NSApp activateIgnoringOtherApps:YES];
    }
    LP32CarbonPanel *presenter = fullscreen_panel;
    if (!presenter)
      for (LP32CarbonPanel *panel in [panels allValues])
        if (panel->game_surface && panel->requested_visible) { presenter = panel; break; }
    if (presenter && presenter->requested_visible && !standard_alert_depth && ![NSApp modalWindow]) {
      [presenter->window makeKeyAndOrderFront:nil];
      [presenter->window makeFirstResponder:[presenter->window contentView]];
      for (NSOpenGLContext *context in [gl_contexts allValues])
        if ([[context view] window] == presenter->window) {
          /* Restoring an ordered-out window also needs a fresh GL drawable. */
          NSView *view = [[context view] retain];
          [context clearDrawable];
          [context setView:view];
          [context update];
          [view release];
        }
    }
    carbon_restoring_focus = NO;
  }
  carbon_apply_fullscreen_presentation();
  objc_bridge32_carbon_focus_changed(active);
  if (changed)
    fprintf(stderr, "compat32: Carbon focus active=%d AppKit=%d presentation=%lu system=%lu\n",
        active, [NSApp isActive], (unsigned long)[NSApp presentationOptions],
        (unsigned long)[NSApp currentSystemPresentationOptions]);
}

static void install_carbon_focus_notifications(void) {
  if (carbon_focus_installed) return;
  carbon_focus_installed = YES;
  carbon_app_active = carbon_process_is_frontmost();
  lp32_set_managed_input_active(carbon_app_active);
  if (getenv("LP32_TRACE_INPUT")) fprintf(stderr, "compat32: initial Carbon focus=%d AppKit=%d\n", carbon_app_active, [NSApp isActive]);
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  [center addObserverForName:NSApplicationWillResignActiveNotification object:NSApp queue:nil
      usingBlock:^(NSNotification *note) { (void)note; carbon_application_focus_changed(NO); }];
  [center addObserverForName:NSApplicationDidBecomeActiveNotification object:NSApp queue:nil
      usingBlock:^(NSNotification *note) { (void)note; carbon_application_focus_changed(YES); }];
  NSNotificationCenter *workspace = [[NSWorkspace sharedWorkspace] notificationCenter];
  for (NSString *name in @[NSWorkspaceDidActivateApplicationNotification, NSWorkspaceDidDeactivateApplicationNotification])
    [workspace addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification *note) {
      NSRunningApplication *app = note.userInfo[NSWorkspaceApplicationKey];
      if (app.processIdentifier == [[NSRunningApplication currentApplication] processIdentifier])
        carbon_application_focus_changed([note.name isEqualToString:NSWorkspaceDidActivateApplicationNotification]);
    }];
  /* The guest uses NSApplicationLoad and its own Carbon loop, never -run. */
  [NSApp finishLaunching];
}

void carbon_ui_sync_focus(void) {
  static BOOL pumping;
  if (!carbon_focus_installed || getenv("LP32_BACKGROUND_TEST") || pumping ||
      ![NSThread isMainThread]) return;
  pumping = YES;
  @autoreleasepool {
    /* Raising an NSWindow cannot complete the queued AppKit activation.
       Dispatch only window-management events; Carbon must retain game input. */
    for (unsigned i = 0; i < 32; ++i) {
      NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAppKitDefined
          untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
      if (!event) break;
      if (getenv("LP32_TRACE_INPUT"))
        fprintf(stderr, "compat32: AppKit management event subtype=%d\n", event.subtype);
      [NSApp sendEvent:event];
    }
    BOOL active = carbon_process_is_frontmost();
    if (active != carbon_app_active) carbon_application_focus_changed(active);
    [NSApp updateWindows];
  }
  pumping = NO;
}

static BOOL suppress_fullscreen_backing_window(LP32CarbonPanel *panel) {
  if (!fullscreen_panel || panel == fullscreen_panel || !panel->game_surface)
    return NO;
  [panel->window orderOut:nil];
  ((void (*)(void *))api("HideWindow"))(panel->native);
  return YES;
}
static void place_test_panel(NSWindow *window) {
  const char *setting = getenv("LP32_TEST_DISPLAY");
  if (!setting || !*setting || [window sheetParent])
    return;
  CGDirectDisplayID display = (CGDirectDisplayID)strtoul(setting, NULL, 0);
  for (NSScreen *screen in [NSScreen screens]) {
    if ([[[screen deviceDescription] objectForKey:@"NSScreenNumber"]
            unsignedIntValue] != display)
      continue;
    NSRect available = [screen visibleFrame], frame = [window frame];
    NSPoint origin = NSMakePoint(
        NSMidX(available) - frame.size.width / 2,
        NSMaxY(available) - frame.size.height -
            fmax(0, (available.size.height - frame.size.height) / 3));
    if (!NSEqualPoints(frame.origin, origin))
      [window setFrameOrigin:origin];
    return;
  }
}
static NSString *title(void *view, BOOL text) {
  CFStringRef string = NULL;
  if (text) {
    ByteCount size = 0;
    ((OSStatus (*)(void *, int16_t, uint32_t, ByteCount, void *,
                   ByteCount *))api("GetControlData"))(
        view, 0, 'cfst', sizeof(string), &string, &size);
  }
  if (!string)
    ((OSStatus (*)(void *, CFStringRef *))api("CopyControlTitleAsCFString"))(
        view, &string);
  return string ? [(NSString *)string autorelease] : @"";
}
static void sync_children(LP32CarbonPanel *panel, void *parent, void *root,
                          NSMutableSet *seen, unsigned depth) {
  if (depth > 32)
    return;
  void *view = ((void *(*)(void *))api("HIViewGetFirstSubview"))(parent);
  for (; view; view = ((void *(*)(void *))api("HIViewGetNextView"))(view)) {
    if (!((Boolean (*)(void *))api("HIViewIsLatentlyVisible"))(view))
      continue;
    ControlKind kind = {0};
    ((OSStatus (*)(void *, ControlKind *))api("GetControlKind"))(view, &kind);
    BOOL edit = carbon_text_is_control(view), label = kind.kind == 'stxt';
    BOOL button = kind.kind == 'push' || kind.kind == 'cbox' ||
                  kind.kind == 'rdio' || kind.kind == 'bevl' ||
                  kind.kind == 'rndb' || kind.kind == 'cgrp';
    BOOL popup = kind.kind == 'popb', slider = kind.kind == 'sldr';
    if (edit || label || button || popup || slider) {
      NSValue *key = [NSValue valueWithPointer:view];
      [seen addObject:key];
      NSControl *control = [panel->controls objectForKey:key];
      if (!control) {
        if (edit || label) {
          NSTextField *field = [[NSTextField alloc] initWithFrame:NSZeroRect];
          [field setEditable:edit];
          [field setSelectable:edit];
          [field setBezeled:edit];
          [field setDrawsBackground:edit];
          [field setFont:[NSFont systemFontOfSize:13]];
          if (edit)
            [field setDelegate:panel];
          control = field;
        } else if (popup)
          control = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                               pullsDown:NO];
        else if (slider)
          control = [[NSSlider alloc] initWithFrame:NSZeroRect];
        else {
          NSButton *b = [[NSButton alloc] initWithFrame:NSZeroRect];
          [b setButtonType:kind.kind == 'cbox' || kind.kind == 'cgrp'
                               ? NSSwitchButton
                           : kind.kind == 'rdio' ? NSRadioButton
                                                 : NSMomentaryPushInButton];
          [b setBezelStyle:NSBezelStyleRounded];
          control = b;
        }
        [control setTag:(NSInteger)view];
        [control setTarget:panel];
        [control setAction:@selector(action:)];
        [[panel->window contentView] addSubview:control];
        [panel->controls setObject:control forKey:key];
        [control release];
      }
      CGRect rect = {0};
      ((OSStatus (*)(void *, CGRect *))api("HIViewGetBounds"))(view, &rect);
      ((OSStatus (*)(CGRect *, void *, void *))api("HIViewConvertRect"))(
          &rect, view, root);
      if (edit) {
        rect.origin.y -= 2;
        rect.size.height = fmax(22, rect.size.height + 4);
      } else if (button || popup) {
        rect.origin.y -= 2;
        rect.size.height = fmax(24, rect.size.height + 4);
      }
      [control setFrame:rect];
      [control setHidden:NO];
      [control setEnabled:((Boolean (*)(void *))api("HIViewIsEnabled"))(view)];
      if (edit) {
        NSString *value = (NSString *)carbon_text_copy_value(view);
        if (![[control stringValue] isEqualToString:value])
          [control setStringValue:value ?: @""];
        [value release];
      } else if (label)
        [control setStringValue:title(view, YES)];
      else if (button) {
        [(NSButton *)control setTitle:title(view, NO)];
        if (kind.kind == 'cbox' || kind.kind == 'cgrp' || kind.kind == 'rdio')
          [(NSButton *)control
              setState:((int32_t (*)(void *))api("HIViewGetValue"))(view)
                           ? NSControlStateValueOn
                           : NSControlStateValueOff];
      } else if (slider) {
        [(NSSlider *)control
            setMinValue:((int32_t (*)(void *))api("HIViewGetMinimum"))(view)];
        [(NSSlider *)control
            setMaxValue:((int32_t (*)(void *))api("HIViewGetMaximum"))(view)];
        [control
            setIntValue:((int32_t (*)(void *))api("HIViewGetValue"))(view)];
      } else if (popup) {
        void *menu = NULL;
        ByteCount size = 0;
        ((OSStatus (*)(void *, int16_t, uint32_t, ByteCount, void *,
                       ByteCount *))api("GetControlData"))(
            view, 0, 'mhan', sizeof(menu), &menu, &size);
        if (menu) {
          uint16_t count = ((uint16_t (*)(void *))api("CountMenuItems"))(menu);
          NSPopUpButton *p = (NSPopUpButton *)control;
          if ([p numberOfItems] != count) {
            [p removeAllItems];
            for (uint16_t i = 1; i <= count; ++i) {
              CFStringRef text = NULL;
              ((OSStatus (*)(void *, uint16_t, CFStringRef *))api(
                  "CopyMenuItemTextAsCFString"))(menu, i, &text);
              [p addItemWithTitle:(NSString *)text ?: @""];
              if (text)
                CFRelease(text);
            }
          }
          int32_t value = ((int32_t (*)(void *))api("HIViewGetValue"))(view);
          if (value > 0 && value <= count)
            [p selectItemAtIndex:value - 1];
        }
      }
    }
    sync_children(panel, view, root, seen, depth + 1);
  }
}
@implementation LP32CarbonPanel
- (void)dealloc {
  [timer invalidate];
  [timer release];
  [window setDelegate:nil];
  [window close];
  [window release];
  [controls release];
  [bitmap release];
  [super dealloc];
}
- (void)sync {
  if (syncing || native_rendering || game_surface)
    return;
  syncing = YES;
  place_test_panel(window);
  void *root = NULL;
  ((OSStatus (*)(void *, void **))api("GetRootControl"))(native, &root);
  NSMutableSet *seen = [NSMutableSet set];
  if (root)
    sync_children(self, root, root, seen, 0);
  for (NSValue *key in controls)
    if (![seen containsObject:key])
      [[controls objectForKey:key] setHidden:YES];
  syncing = NO;
}
- (void)action:(NSControl *)sender {
  [self retain];
  void *view = (void *)[sender tag];
  if ([sender isKindOfClass:[NSPopUpButton class]])
    ((OSStatus (*)(void *, int32_t))api("HIViewSetValue"))(
        view, (int32_t)[(NSPopUpButton *)sender indexOfSelectedItem] + 1);
  if ([sender isKindOfClass:[NSSlider class]])
    ((OSStatus (*)(void *, int32_t))api("HIViewSetValue"))(view,
                                                           [sender intValue]);
  ControlKind kind = {0};
  ((OSStatus (*)(void *, ControlKind *))api("GetControlKind"))(view, &kind);
  int16_t part = kind.kind == 'cbox' || kind.kind == 'cgrp' ? 11
                 : kind.kind == 'rdio'                      ? 12
                                                            : 10,
          clicked = 0;
  OSStatus (*click)(void *, int16_t, uint32_t, int16_t *) =
      api("HIViewSimulateClick");
  OSStatus status = click ? click(view, part, 0, &clicked) : eventNotHandledErr;
  if (!clicked && (status == noErr || status == eventNotHandledErr)) {
    EventRef event = NULL;
    CreateEvent(NULL, kEventClassControl, kEventControlHit,
                GetCurrentEventTime(), 0, &event);
    SetEventParameter(event, kEventParamDirectObject, typeControlRef,
                      sizeof(view), &view);
    SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                      sizeof(part), &part);
    uint32_t modifiers = 0;
    SetEventParameter(event, kEventParamKeyModifiers, typeUInt32,
                      sizeof(modifiers), &modifiers);
    status = SendEventToEventTarget(
        event, ((void *(*)(void *))api("GetControlEventTarget"))(view));
    ReleaseEvent(event);
  }
  if (status == eventNotHandledErr) {
    HICommandExtended command = {0};
    ((OSStatus (*)(void *, uint32_t *))api("HIViewGetCommandID"))(
        view, &command.commandID);
    if (command.commandID) {
      command.attributes = kHICommandFromControl;
      command.source.control = view;
      EventRef event = NULL;
      CreateEvent(NULL, kEventClassCommand, kEventCommandProcess,
                  GetCurrentEventTime(), 0, &event);
      SetEventParameter(event, kEventParamDirectObject, typeHICommand,
                        sizeof(command), &command);
      status = SendEventToEventTarget(
          event, ((void *(*)(void *))api("GetWindowEventTarget"))(native));
      ReleaseEvent(event);
    }
    fprintf(stderr, "compat32: control command=%#x status=%d\n",
            command.commandID, status);
  }
  [self sync];
  [self release];
}
- (void)controlTextDidChange:(NSNotification *)note {
  if (syncing)
    return;
  NSTextField *field = [note object];
  carbon_text_set_value((void *)[field tag], [field stringValue]);
  [self sync];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
  (void)sender;
  for (NSControl *control in [controls allValues])
    if ([control isKindOfClass:[NSButton class]] &&
        [[(NSButton *)control title] isEqualToString:@"Quit"] &&
        ![control isHidden]) {
      [self action:control];
      return NO;
    }
  if (modal)
    [NSApp stopModal];
  return YES;
}
@end
static LP32CarbonPanel *panel_for(void *native, BOOL create) {
  if (!panels && create)
    panels = [[NSMutableDictionary alloc] init];
  NSValue *key = [NSValue valueWithPointer:native];
  LP32CarbonPanel *panel = [panels objectForKey:key];
  if (panel || !create)
    return panel;
  NSApplicationLoad();
  panel = [[LP32CarbonPanel alloc] init];
  panel->native = native;
  panel->controls = [[NSMutableDictionary alloc] init];
  Rect bounds = {0};
  ((OSStatus (*)(void *, uint32_t, Rect *))api("GetWindowBounds"))(native, 33,
                                                                   &bounds);
  CGRect screen = CGDisplayBounds(CGMainDisplayID());
  NSRect rect =
      NSMakeRect(bounds.left, screen.size.height - bounds.bottom,
                 bounds.right - bounds.left, bounds.bottom - bounds.top);
  Class window_class = lp32_title_is_cod_carbon(lp32_profile()->title) ?
      [LP32CarbonSurfaceWindow class] : [NSWindow class];
  panel->window = [[window_class alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [panel->window setReleasedWhenClosed:NO];
  if (window_class == [LP32CarbonSurfaceWindow class]) {
    install_carbon_focus_notifications();
    [panel->window setAcceptsMouseMovedEvents:YES];
  }
  [panel->window setDelegate:panel];
  [panel->window setTitle:[[NSBundle mainBundle]
                              objectForInfoDictionaryKey:@"CFBundleName"]
                              ?: @"Game"];
  [panel->window
      setContentView:[[[LP32FlippedView alloc]
                         initWithFrame:NSMakeRect(0, 0, rect.size.width,
                                                  rect.size.height)]
                         autorelease]];
  [panels setObject:panel forKey:key];
  [panel release];
  return panel;
}
void carbon_ui_paint(void *native, void *raw_image) {
  LP32CarbonPanel *panel = panel_for(native, YES);
  panel->game_surface = YES;
  suppress_fullscreen_backing_window(panel);
  if (!panel->bitmap) {
    panel->bitmap = [[NSImageView alloc] initWithFrame:[[panel->window contentView] bounds]];
    [panel->bitmap setImageScaling:NSImageScaleAxesIndependently];
    [panel->bitmap setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [[panel->window contentView] addSubview:panel->bitmap];
  }
  NSImage *image = raw_image ? [[NSImage alloc] initWithCGImage:raw_image size:NSZeroSize] : nil;
  [panel->bitmap setImage:image];
  [image release];
}
void carbon_ui_geometry_changed(void *native) {
  LP32CarbonPanel *panel = panel_for(native, NO);
  if (!panel || !panel->game_surface) return;
  Rect bounds;
  if (((OSStatus (*)(void *, uint32_t, Rect *))api("GetWindowBounds"))(native, 33, &bounds)) return;
  CGFloat top = NSMaxY([[[NSScreen screens] firstObject] frame]);
  NSRect content = NSMakeRect(bounds.left, top - bounds.bottom, bounds.right - bounds.left, bounds.bottom - bounds.top);
  [panel->window setFrame:[panel->window frameRectForContentRect:content] display:YES];
  for (NSOpenGLContext *context in [gl_contexts allValues])
    if ([[context view] window] == panel->window) [context update];
}
int carbon_ui_bind_gl(void *agl, void *cgl, void *native) {
  if (!gl_contexts) gl_contexts = [[NSMutableDictionary alloc] init];
  NSValue *key = [NSValue valueWithPointer:agl];
  NSOpenGLContext *context = [gl_contexts objectForKey:key];
  if (!context && cgl) {
    context = [[[NSOpenGLContext alloc] initWithCGLContextObj:cgl] autorelease];
    if (context) [gl_contexts setObject:context forKey:key];
  }
  if (!context) return 0;
  [context clearDrawable];
  if (native) {
    LP32CarbonPanel *panel = panel_for(native, YES);
    panel->game_surface = YES;
    [panel->bitmap setHidden:YES];
    NSView *view = [panel->window contentView];
    [view setWantsBestResolutionOpenGLSurface:NO];
    [context setView:view];
    if (panel->surface_size[0] && panel->surface_size[1]) {
      CGLSetParameter(cgl, kCGLCPSurfaceBackingSize, panel->surface_size);
      CGLEnable(cgl, kCGLCESurfaceBackingSize);
    }
    [context update];
  }
  return 1;
}
int carbon_ui_update_gl(void *agl) {
  NSOpenGLContext *context = [gl_contexts objectForKey:[NSValue valueWithPointer:agl]];
  if (!context) return 0;
  [context update];
  return 1;
}
static void write_capture_pixels(const char *path, const unsigned char *pixels,
                                 GLint width, GLint height) {
  NSString *temporary = [[NSString stringWithUTF8String:path] stringByAppendingString:@".tmp"];
  FILE *file = fopen(temporary.fileSystemRepresentation, "wb");
  if (!file) return;
  size_t row = (size_t)width * 3;
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  for (int y = height - 1; y >= 0; --y) fwrite(pixels + y * row, row, 1, file);
  bool complete = !ferror(file);
  if (fclose(file)) complete = false;
  if (complete) rename(temporary.fileSystemRepresentation, path);
}

/* LP32_AGL_CAPTURE_FRAME=<path>: every LP32_AGL_CAPTURE_EVERY swaps
   (default 60), write the back buffer as a PPM before it is presented. */
void carbon_ui_capture_gl_frame(NSOpenGLContext *context, uint64_t swaps) {
  const char *capture = getenv("LP32_AGL_CAPTURE_FRAME");
  const char *interval_text = getenv("LP32_AGL_CAPTURE_EVERY");
  const char *gate = getenv("LP32_AGL_CAPTURE_GATE");
  if (gate && access(gate, F_OK) != 0) return;
  unsigned interval = interval_text ? (unsigned)strtoul(interval_text, NULL, 10) : 60;
  if (capture && interval && swaps % interval == 0) {
    char sequence_path[PATH_MAX];
    if (getenv("LP32_AGL_CAPTURE_SEQUENCE")) {
      snprintf(sequence_path, sizeof(sequence_path), "%s.%09llu.ppm", capture,
               (unsigned long long)swaps);
      capture = sequence_path;
    }
    CGLContextObj previous = CGLGetCurrentContext();
    CGLSetCurrentContext([context CGLContextObj]);
    GLint viewport[4], framebuffer, read_framebuffer, buffer, pack, alignment, row_length, skip_rows, skip_pixels;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glReadBuffer(GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    if (viewport[2] > 0 && viewport[3] > 0 && viewport[2] <= 16384 && viewport[3] <= 16384) {
      size_t row = (size_t)viewport[2] * 3;
      unsigned char *pixels = malloc(row * viewport[3]);
      if (pixels) {
        glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels);
        write_capture_pixels(capture, pixels, viewport[2], viewport[3]);
        free(pixels);
      }
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    glReadBuffer(buffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    CGLSetCurrentContext(previous);
  }
}

int carbon_ui_swap_gl(void *agl) {
  carbon_ui_sync_focus();
  NSOpenGLContext *context = [gl_contexts objectForKey:[NSValue valueWithPointer:agl]];
  if (!context) return 0;
  static uint64_t swaps;
  ++swaps;
  carbon_ui_capture_gl_frame(context, swaps);
  [context update];
  if (swaps <= 3) {
    CGLContextObj previous=CGLGetCurrentContext(),current=[context CGLContextObj];CGLSetCurrentContext(current);
    GLint framebuffer=0,depth=0,stencil=0,requested=0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&framebuffer);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);
    glGetIntegerv(GL_DEPTH_BITS,&depth);glGetIntegerv(GL_STENCIL_BITS,&stencil);
    CGLDescribePixelFormat(CGLGetPixelFormat(current),0,kCGLPFADepthSize,&requested);
    fprintf(stderr,"compat32: AGL drawable depth=%d stencil=%d formatDepth=%d\n",depth,stencil,requested);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,framebuffer);CGLSetCurrentContext(previous);
  }
  uint64_t work_end = hitch_recorder_enabled ? hitch_now() : 0;
  [context flushBuffer];
  if (hitch_recorder_enabled) {
    uint64_t present_end = hitch_now();
    hitch_frame(swaps, work_end, present_end, present_end, 0,
                carbon_app_active || getenv("LP32_BACKGROUND_TEST"));
  }
  objc_bridge32_note_agl_frame();
  audio_bridge32_note_frame_presented();
  static int frame_stats = -1;
  if (frame_stats < 0) frame_stats = getenv("LP32_FRAME_STATS") != NULL;
  if (swaps == 1) {
    fprintf(stderr, "compat32: AGL presented frame %llu\n", (unsigned long long)swaps);
  } else if (frame_stats && swaps % 300 == 0) {
    static uint64_t interval_start_ns;
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    double fps = interval_start_ns ? 300e9 / (double)(now_ns - interval_start_ns) : 0;
    interval_start_ns = now_ns;
    fprintf(stderr, "compat32: AGL presented frame %llu fps=%.1f\n", (unsigned long long)swaps, fps);
  }
  return 1;
}
void carbon_ui_release_gl(void *agl) {
  NSValue *key = [NSValue valueWithPointer:agl];
  [[gl_contexts objectForKey:key] clearDrawable];
  [gl_contexts removeObjectForKey:key];
}
void carbon_ui_fullscreen(void *native, uint32_t display, int32_t width, int32_t height) {
  LP32CarbonPanel *panel = panel_for(native, YES);
  panel->game_surface = YES;
  panel->surface_size[0] = width; panel->surface_size[1] = height;
  if (lp32_title_is_cod_carbon(lp32_profile()->title)) {
    /* COD4 keeps its painted Carbon main window as the input/event target,
       then moves AGL to a separate display port. Present only that port. */
    fullscreen_panel = panel;
    for (LP32CarbonPanel *other in [panels allValues])
      suppress_fullscreen_backing_window(other);
  }
  for (NSScreen *screen in [NSScreen screens]) {
    if ([[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue] != display) continue;
    [panel->window setStyleMask:NSWindowStyleMaskBorderless];
    [panel->window setFrame:screen.frame display:YES];
    break;
  }
  carbon_ui_show(native);
}
void carbon_ui_show(void *native) {
  LP32CarbonPanel *p = panel_for(native, YES);
  p->requested_visible = YES;
  if (suppress_fullscreen_backing_window(p)) return;
  if (p->native_rendering) {
    ((void (*)(void *))api("ShowWindow"))(native);
    return;
  }
  [p sync];
  if (getenv("LP32_BACKGROUND_TEST"))
    [p->window orderBack:nil];
  else
    [p->window makeKeyAndOrderFront:nil];
  objc_bridge32_install_test_input(p->window);
  carbon_apply_fullscreen_presentation();
}
void carbon_ui_hide(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p) {
    p->requested_visible = NO;
    [p->window orderOut:nil];
    carbon_apply_fullscreen_presentation();
  }
}
int carbon_ui_select(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (!p || p->native_rendering) return 0;
  if (p->requested_visible) carbon_ui_show(native);
  return 1;
}
void carbon_ui_dispose(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p && p->modal)
    [NSApp stopModal];
  BOOL leaving_fullscreen = p && p == fullscreen_panel;
  if (leaving_fullscreen) fullscreen_panel = nil;
  carbon_apply_fullscreen_presentation();
  [panels removeObjectForKey:[NSValue valueWithPointer:native]];
  if (leaving_fullscreen)
    for (LP32CarbonPanel *other in [panels allValues])
      if (other->game_surface && other->requested_visible)
        carbon_ui_show(other->native);
}
int32_t carbon_ui_run_modal(void *native) {
  LP32CarbonPanel *p = [panel_for(native, YES) retain];
  carbon_ui_show(native);
  p->modal = YES;
  p->timer = [[NSTimer timerWithTimeInterval:0.15
                                      target:p
                                    selector:@selector(sync)
                                    userInfo:nil
                                     repeats:YES] retain];
  [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSModalPanelRunLoopMode];
  [NSApp runModalForWindow:p->window];
  [p->timer invalidate];
  [p->timer release];
  p->timer = nil;
  p->modal = NO;
  [p release];
  return 0;
}
/* RunStandardAlert's legacy Carbon loop can draw its dialog without delivering
 * user input in an NSApplicationLoad process. AppKit owns this modal loop and
 * its buttons. Keep the Carbon dialog for guest references until it completes. */
void carbon_ui_prepare_standard_alert(void *dialog, uint32_t type,
    const void *raw_title, const void *raw_message, int16_t default_item, int16_t cancel_item) {
  LP32CarbonStandardAlert *alert = [[LP32CarbonStandardAlert alloc] init];
  alert->carbon_items = [[NSMutableArray alloc] init];
  [alert setMessageText:(NSString *)raw_title ?: @""];
  [alert setInformativeText:(NSString *)raw_message ?: @""];
  [alert setAlertStyle:type == kAlertStopAlert ? NSAlertStyleCritical :
      type == kAlertCautionAlert ? NSAlertStyleWarning : NSAlertStyleInformational];
  for (int16_t item = 1; item <= 3; ++item) {
    void *control = NULL;
    if (((OSStatus (*)(void *, int16_t, void **))api("GetDialogItemAsControl"))(
        dialog, item, &control) || !control ||
        !((Boolean (*)(void *))api("HIViewIsLatentlyVisible"))(control)) continue;
    CFStringRef text = NULL;
    ((OSStatus (*)(void *, CFStringRef *))api("CopyControlTitleAsCFString"))(control, &text);
    NSButton *button = [alert addButtonWithTitle:(NSString *)text ?: @""];
    /* NSAlert owns the button tags and uses them as its modal response. */
    [alert->carbon_items addObject:@(item)];
    [button setKeyEquivalent:item == default_item ? @"\r" : item == cancel_item ? @"\033" : @""];
    if (text) CFRelease(text);
  }
  /* AppKit presents the alert. Never leave its Carbon backing window on
     screen while the guest is busy loading after a button is selected. */
  void *window = ((void *(*)(void *))api("GetDialogWindow"))(dialog);
  ((void (*)(void *))api("HideWindow"))(window);
  if (!standard_alerts) standard_alerts = [[NSMutableDictionary alloc] init];
  [standard_alerts setObject:alert forKey:[NSValue valueWithPointer:dialog]];
  [alert release];
}
int32_t carbon_ui_run_standard_alert(void *dialog, int16_t *item) {
  NSValue *key = [NSValue valueWithPointer:dialog];
  LP32CarbonStandardAlert *alert = [[standard_alerts objectForKey:key] retain];
  if (!alert || ![alert.buttons count]) { [alert release]; return paramErr; }
  ++standard_alert_depth;
  fprintf(stderr, "compat32: entering AppKit standard alert buttons=%lu\n", (unsigned long)[alert.buttons count]);
  NSModalResponse response = [alert runModal];
  /* Guest initialization may not pump AppKit for several seconds. Dismiss
     the window now instead of relying on a later autorelease/event cycle. */
  [alert.window orderOut:nil];
  NSInteger index = response - NSAlertFirstButtonReturn;
  int32_t status = index >= 0 && (NSUInteger)index < [alert.buttons count] ? noErr : userCanceledErr;
  if (!status && item) *item = [alert->carbon_items[index] shortValue];
  --standard_alert_depth;
  [standard_alerts removeObjectForKey:key];
  [alert release];
  /* DisposeDialog has no LP64 export. Its dialog window owns the native
     controls and can be destroyed through the surviving Window Manager. */
  void *window = ((void *(*)(void *))api("GetDialogWindow"))(dialog);
  ((void (*)(void *))api("DisposeWindow"))(window);
  [NSApp updateWindows];
  fprintf(stderr, "compat32: AppKit standard alert returned status=%d item=%d\n", status, !status && item ? *item : 0);
  return status;
}
/* Used only by the explicitly enabled bridge self-test. */
int carbon_ui_test_click_standard_alert(void *dialog, int16_t item) {
  LP32CarbonStandardAlert *alert = [standard_alerts objectForKey:[NSValue valueWithPointer:dialog]];
  if (![NSApp modalWindow] || [NSApp modalWindow] != alert.window) return 0;
  for (NSUInteger index = 0; index < [alert.buttons count]; ++index)
    if ([alert->carbon_items[index] shortValue] == item) {
      [alert.buttons[index] performClick:nil]; return 1;
    }
  return 0;
}
int carbon_ui_stop_modal(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (!p || !p->modal)
    return 0;
  [NSApp stopModal];
  return 1;
}
void carbon_ui_use_native(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p) {
    p->native_rendering = YES;
    [p->window orderOut:nil];
  }
  ((void (*)(void *))api("ShowWindow"))(native);
}

int carbon_ui_convert_game_point(void *native, int16_t *point, int to_local) {
  LP32CarbonPanel *source = panel_for(native, NO);
  if (!source || !source->game_surface) return 0;
  for (LP32CarbonPanel *panel in [panels allValues]) {
    if (!panel->surface_size[0] || !panel->surface_size[1] || ![panel->window isVisible]) continue;
    NSRect frame = [panel->window convertRectToScreen:[[panel->window contentView] bounds]];
    CGFloat top = NSMaxY([[[NSScreen screens] firstObject] frame]);
    double x = frame.origin.x, y = top - NSMaxY(frame);
    if (frame.size.width <= 0 || frame.size.height <= 0) return 0;
    if (getenv("LP32_TRACE_INPUT")) {
      static unsigned samples;
      if (!(samples++ % 120)) fprintf(stderr, "compat32: fullscreen input frame=%.0f,%.0f %.0fx%.0f logical=%dx%d\n",x,y,frame.size.width,frame.size.height,panel->surface_size[0],panel->surface_size[1]);
    }
    if (to_local) {
      point[1] = (int16_t)lrint((point[1] - x) * panel->surface_size[0] / frame.size.width);
      point[0] = (int16_t)lrint((point[0] - y) * panel->surface_size[1] / frame.size.height);
    } else {
      point[1] = (int16_t)lrint(x + point[1] * frame.size.width / panel->surface_size[0]);
      point[0] = (int16_t)lrint(y + point[0] * frame.size.height / panel->surface_size[1]);
    }
    return 1;
  }
  return 0;
}

int carbon_ui_is_active(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p && p->game_surface && p->requested_visible && fullscreen_panel && p != fullscreen_panel)
    return [fullscreen_panel->window isKeyWindow];
  return !p || p->native_rendering ? -1 : [p->window isKeyWindow];
}

int carbon_ui_is_visible(void *native) {
  LP32CarbonPanel *p = panel_for(native, NO);
  if (p && p->game_surface && fullscreen_panel && p != fullscreen_panel)
    return p->requested_visible;
  return !p || p->native_rendering ? -1 : [p->window isVisible];
}

int32_t carbon_ui_show_sheet(void *sheet, void *parent) {
  LP32CarbonPanel *p = panel_for(sheet, YES), *owner = panel_for(parent, YES);
  [p sync];
  if (!p->timer) {
    p->timer = [[NSTimer timerWithTimeInterval:0.15
                                        target:p
                                      selector:@selector(sync)
                                      userInfo:nil
                                       repeats:YES] retain];
    [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSModalPanelRunLoopMode];
    [[NSRunLoop mainRunLoop] addTimer:p->timer forMode:NSDefaultRunLoopMode];
  }
  [owner->window beginSheet:p->window completionHandler:nil];
  return 0;
}
int32_t carbon_ui_hide_sheet(void *sheet) {
  LP32CarbonPanel *p = panel_for(sheet, NO);
  if (!p)
    return -50;
  NSWindow *parent = [p->window sheetParent];
  if (parent)
    [parent endSheet:p->window];
  [p->window orderOut:nil];
  [p->timer invalidate];
  [p->timer release];
  p->timer = nil;
  return 0;
}
