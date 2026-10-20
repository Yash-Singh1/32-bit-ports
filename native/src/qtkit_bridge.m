#include "qtkit_bridge.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include <stdlib.h>
#include <string.h>

const char *const kQTMovieDidEndNotificationName = "QTMovieDidEndNotification";

struct guest_qttime {
    int64_t value;
    int32_t scale;
    int32_t flags;
};

static bool trace(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_TRACE_QTKIT") != NULL;
    return enabled;
}

/* QTMovie: an AVPlayer over the file, paused until -play, posting
   QTMovieDidEndNotification (object: the movie) when playback ends. */
@interface LP32QTMovie : NSObject
@property(nonatomic, retain) AVPlayer *player;
@end

@implementation LP32QTMovie
+ (id)movieWithFile:(NSString *)path error:(NSError **)error
{
    return [[[self alloc] initWithFile:path error:error] autorelease];
}
+ (id)movieWithURL:(NSURL *)url error:(NSError **)error
{
    return [[[self alloc] initWithURL:url error:error] autorelease];
}
+ (BOOL)canInitWithFile:(NSString *)path
{
    return [[NSFileManager defaultManager] isReadableFileAtPath:path];
}
- (id)initWithFile:(NSString *)path error:(NSError **)error
{
    return [self initWithURL:path ? [NSURL fileURLWithPath:path] : nil error:error];
}
- (id)initWithURL:(NSURL *)url error:(NSError **)error
{
    if (error) *error = nil;
    if (!url || (url.isFileURL && ![[NSFileManager defaultManager] fileExistsAtPath:url.path])) {
        if (error) *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileNoSuchFileError userInfo:nil];
        [self release];
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:url];
    _player = [[AVPlayer alloc] initWithPlayerItem:item];
    _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    const char *mute = getenv("LP32_MUTE_AUDIO");
    _player.muted = mute && strcmp(mute, "0");
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(lp32_itemDidEnd:)
                                                 name:AVPlayerItemDidPlayToEndTimeNotification object:item];
    if (trace()) fprintf(stderr, "compat32: QTMovie %p opened %s\n", (void *)self, url.absoluteString.UTF8String);
    return self;
}
- (void)lp32_itemDidEnd:(NSNotification *)notification
{
    (void)notification;
    if (trace()) fprintf(stderr, "compat32: QTMovie %p ended\n", (void *)self);
    [[NSNotificationCenter defaultCenter] postNotificationName:@(kQTMovieDidEndNotificationName) object:self];
}
- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_player pause];
    [_player release];
    [super dealloc];
}
- (void)play { [_player play]; }
- (void)stop { [_player pause]; }
- (void)gotoBeginning { [_player seekToTime:kCMTimeZero]; }
- (void)gotoEnd { [_player seekToTime:_player.currentItem.duration]; }
- (float)rate { return _player.rate; }
- (void)setRate:(float)rate { _player.rate = rate; }
- (float)volume { return _player.volume; }
- (void)setVolume:(float)volume { _player.volume = volume; }
- (BOOL)muted { return _player.muted; }
- (void)setMuted:(BOOL)muted
{
    const char *mute = getenv("LP32_MUTE_AUDIO");
    _player.muted = muted || (mute && strcmp(mute, "0"));
}
- (void)setAttribute:(id)value forKey:(NSString *)key { (void)value; (void)key; }
- (id)attributeForKey:(NSString *)key { (void)key; return nil; }
- (BOOL)isPlaying { return _player.rate != 0.f; }
@end

/* QTMovieView: a layer-hosting NSView showing the movie aspect-fit on black.
   Controller, editing and resize-indicator settings only affect QTKit's
   control bar, which this view never shows. */
@interface LP32QTMovieView : NSView
@property(nonatomic, retain) LP32QTMovie *movie;
@property(nonatomic, retain) AVPlayerLayer *playerLayer;
/* QTMovieView's delegate (menu/selection callbacks) is not retained. */
@property(nonatomic, assign) id delegate;
@end

@implementation LP32QTMovieView
- (id)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _playerLayer = [[AVPlayerLayer alloc] init];
    _playerLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    _playerLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    self.layer = _playerLayer;
    self.wantsLayer = YES;
    return self;
}
- (void)dealloc
{
    [_movie release];
    [_playerLayer release];
    [super dealloc];
}
- (void)setMovie:(LP32QTMovie *)movie
{
    if (movie == _movie) return;
    [_movie release];
    _movie = [movie retain];
    _playerLayer.player = [movie isKindOfClass:[LP32QTMovie class]] ? movie.player : nil;
}
- (BOOL)isOpaque { return YES; }
- (void)setControllerVisible:(BOOL)visible { (void)visible; }
- (BOOL)isControllerVisible { return NO; }
- (void)setEditable:(BOOL)editable { (void)editable; }
- (BOOL)isEditable { return NO; }
- (void)setPreservesAspectRatio:(BOOL)preserve
{
    _playerLayer.videoGravity = preserve ? AVLayerVideoGravityResizeAspect : AVLayerVideoGravityResize;
}
- (BOOL)preservesAspectRatio { return [_playerLayer.videoGravity isEqualToString:AVLayerVideoGravityResizeAspect]; }
- (void)setShowsResizeIndicator:(BOOL)shows { (void)shows; }
- (void)setFillColor:(NSColor *)color { _playerLayer.backgroundColor = color.CGColor; }
- (IBAction)play:(id)sender { (void)sender; [_movie play]; }
- (IBAction)pause:(id)sender { (void)sender; [_movie stop]; }
- (IBAction)gotoBeginning:(id)sender { (void)sender; [_movie gotoBeginning]; }
- (IBAction)gotoEnd:(id)sender { (void)sender; [_movie gotoEnd]; }
@end

void *qtkit_bridge32_class(const char *guest_name)
{
    if (!strcmp(guest_name, "QTMovie")) return [LP32QTMovie class];
    if (!strcmp(guest_name, "QTMovieView")) return [LP32QTMovieView class];
    return NULL;
}

static struct guest_qttime guest_time(CMTime time)
{
    struct guest_qttime result = {0, 600, 0};
    if (CMTIME_IS_NUMERIC(time)) {
        result.scale = time.timescale;
        result.value = time.value;
    } else if (CMTIME_IS_INDEFINITE(time)) {
        result.flags = 1; /* kQTTimeIsIndefinite */
    }
    return result;
}

int qtkit_bridge32_stret(void *receiver, const char *selector, void *destination)
{
    id object = (id)receiver;
    if (![object isKindOfClass:[LP32QTMovie class]]) return 0;
    LP32QTMovie *movie = object;
    struct guest_qttime time;
    if (!strcmp(selector, "currentTime")) time = guest_time(movie.player.currentTime);
    else if (!strcmp(selector, "duration")) time = guest_time(movie.player.currentItem.asset.duration);
    else return 0;
    memcpy(destination, &time, sizeof(time));
    return 1;
}

int qtkit_bridge32_message(void *receiver, const char *selector, const uint32_t *arguments, uint64_t *result)
{
    id object = (id)receiver;
    if (![object isKindOfClass:[LP32QTMovie class]] || strcmp(selector, "setCurrentTime:")) return 0;
    struct guest_qttime time;
    memcpy(&time, arguments + 2, sizeof(time));
    if (time.scale > 0)
        [((LP32QTMovie *)object).player seekToTime:CMTimeMake(time.value, time.scale)
                                   toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
    *result = 0;
    return 1;
}
