// AVA OS iOS — ObjC facade over the shared C++ vibroacoustic engine.
// Mic listens to the room (iOS has no system-audio tap); pads play directly.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AVABridge : NSObject
+ (instancetype)shared;

- (BOOL)start;                 // configure session + graph; NO on failure
- (void)stop;
- (BOOL)isRunning;
@property (readonly) NSString *lastError;

// octagon pads (zone 0=HEAD..4=FEET; k = slice 0..7; frac = radial 0..1)
- (void)padDownZone:(int)zone k:(int)k frac:(float)frac;
- (void)padUpZone:(int)zone;
- (void)padAllUp;
- (void)setPatch:(int)p;
- (int)patch;

// engine params
- (void)setIntensity:(float)v;
- (void)setVoidAmt:(float)v;
- (void)setMaster:(float)v;

// How the output channels are wired to the bed:
//   0 = octagon (one channel per body zone on an interface; a plain stereo
//       output gets every zone summed, as a monitor)
//   8 = circle, one channel per transducer on Ch 3-10
//   4 = circle, Ch 1 outer top+left, Ch 2 inner top+left,
//       Ch 3 inner right+bottom, Ch 4 outer right+bottom
//   2 = circle, Ch 1 inner disc, Ch 2 outer ring (works on stereo outputs)
// A circle wiring that needs more channels than the output has falls back to
// the next smaller one; activeWiring reports what is really playing.
- (void)setWiring:(int)mode;
- (int)wiring;
- (int)activeWiring;

// readouts for UI
- (float)meterZone:(int)z;     // 0..1 live zone output
- (float)level;                // input loudness envelope
- (float)bpm;
- (float)rootHz;
- (float)voidNow;
- (int)outputChannels;
- (NSString *)outputName;
@end

NS_ASSUME_NONNULL_END
