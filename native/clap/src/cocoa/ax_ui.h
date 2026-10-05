// ax_ui.h
// The native macOS editor's shared pieces: the model (parameters + telemetry the
// plugin pushes), the palette and drawing helpers, the host protocol every widget
// talks to, and the widgets and graphs themselves. Objective-C++, ARC.
//
// Layout of the editor (axon_gui_cocoa.mm): a header, a row of stage chips (the
// signal path, reorderable, each with a light that switches it), one panel per
// stage with a graph drawn from that stage's DSP and its controls, and a levels
// column on the right.
#pragma once
#import <AppKit/AppKit.h>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace axui {

// ---------------------------------------------------------------- stages
// StageIDs, as the plugin numbers them (processor_order holds these).
enum StageId { StageAutoEq = 1, StageEq = 3, StageBusComp = 4, StageLimiter = 5, StageBassMono = 6, StageReverb = 8, StageWidener = 9, StageProgram = 10 };
unsigned stageAccent(int stage);
NSString* stageName(int stage);

// ---------------------------------------------------------------- model
struct Meta {
    std::string name, unit;
    double min = 0, max = 1, def = 0;
    std::vector<std::string> options;      // enum labels, one per integer value from min to max
};
struct Level { float lufs_s = -120, lufs_m = -120, rms = -120, peak = -120; };

// Everything the plugin pushes to the page as JavaScript calls (axonSpectrum, axonSslCurve, ...), decoded.
constexpr int kBins = 50;                  // the EQ curves are published at 50 log-spaced bins, 20 Hz to 20 kHz
constexpr int kHistory = 180;
struct Telemetry {
    // axonSpectrum
    std::vector<int> spectrumOrder;
    std::vector<std::vector<float>> spectrumDb;    // [position in the chain][128 log-spaced bins], dB
    bool haveEqBands = false; std::array<float, 5> eqBands {};
    bool haveEqBins = false; std::array<float, kBins> eqBins {};
    // axonSslCurve
    bool sslOn = false; int sslSelected = 0;
    std::array<std::array<float, kBins>, 3> ssl {};
    std::array<bool, 3> sslHave {};
    // axonPultecCurve
    bool pulOn = false; std::array<float, kBins> pul {};
    // axonMeters
    bool haveMeters = false; Level in, out;
    // axonLimiter
    bool haveLimiter = false, limiterActive = true; float brick = 0, ceiling = 0;
    std::vector<float> limF, limLvl, limGr;
    // axonBusComp
    bool haveBusComp = false; bool bcActive = true; float bcDist = -48, bcCrest = 0;
    // The two scrolling strips (gain reduction, distortion): newest last.
    std::array<float, kHistory> grHist {}, grBandHist {}, bcHist {}, bcCrestHist {};
    int grLen = 0, bcLen = 0;
    unsigned generation = 0;               // bumps on every change, so views know to repaint
};

struct Model {
    std::unordered_map<std::string, Meta> meta;
    std::unordered_map<std::string, double> values;
    std::vector<int> order;
    Telemetry t;
    double value(const std::string& id) const {
        auto v = values.find(id); if (v != values.end()) return v->second;
        auto m = meta.find(id); return m != meta.end() ? m->second.def : 0;
    }
    const Meta* find(const std::string& id) const { auto m = meta.find(id); return m == meta.end() ? nullptr : &m->second; }
};

// Decode one of the plugin's telemetry calls: `axonName({...});`. Returns the name ("" if it is not one).
std::string decodeTelemetry(const char* js, Model& model);

// ---------------------------------------------------------------- palette & text
NSColor* uiColor(unsigned rgb, CGFloat alpha = 1);
extern const unsigned kBg, kBgSunken, kSurface, kSurfaceRaised, kSurfaceHover, kBorder, kBorderStrong;
extern const unsigned kText, kTextDim, kTextMute, kAccent, kDanger, kWarn, kOk;
void uiDrawText(NSString* string, NSRect rect, CGFloat size, NSColor* tint, NSFontWeight weight, CGFloat kern, NSTextAlignment alignment);
void uiDrawMono(NSString* string, NSRect rect, CGFloat size, NSColor* tint, NSFontWeight weight, NSTextAlignment alignment);
void uiFillPanel(NSRect r, CGFloat radius, NSColor* fill, NSColor* stroke);
void uiFillRect(NSRect r, NSColor* color);                     // blends; NSRectFill ignores alpha
NSBezierPath* uiPolyline(const std::vector<NSPoint>& points);
NSString* uiPretty(const std::string& s);                      // "full_mix" -> "FULL MIX"
// Values as the controls show them: "225 Hz", "+3.0 dB", "30%", ... A stage can override one parameter's text.
NSString* uiFormat(const Model& model, const std::string& id, double value);

// ---------------------------------------------------------------- host protocol
}  // namespace axui

@protocol AXHost <NSObject>
- (axui::Model*)model;
- (double)valueFor:(NSString*)pid;
- (void)setParam:(NSString*)pid value:(double)value;           // an edit by the user: remember it and tell the plugin
- (NSString*)textFor:(NSString*)pid value:(double)value;       // the stage's own text for it, or the default
- (BOOL)parse:(NSString*)text for:(NSString*)pid into:(double*)value;
@end

// ---------------------------------------------------------------- widgets
// A plain container whose coordinates run from the top.
@interface AXFlipped : NSView
@end

typedef NS_ENUM(NSInteger, AXKnobSize) { AXKnobBig, AXKnobCompact, AXKnobMini };

@interface AXKnob : NSView <NSTextFieldDelegate>
@property (weak) id<AXHost> host;
@property (nonatomic, copy) NSString* pid;
@property (nonatomic, copy) NSString* title;
@property (nonatomic) NSColor* tint;
@property (nonatomic) AXKnobSize size;
@property (nonatomic) double step;                 // detents (a rotary switch), 0 for continuous
@property (nonatomic) BOOL dimmed;
@property (nonatomic, readonly) NSTextField* field;
- (void)sync;
@end

@interface AXToggle : NSView
@property (weak) id<AXHost> host;
@property (nonatomic, copy) NSString* pid;
@property (nonatomic, copy) NSString* caption;      // small text above
@property (nonatomic) NSArray<NSString*>* labels;   // off, on
@property (nonatomic) NSColor* tint;
@property (nonatomic) BOOL on;
@property (nonatomic) BOOL big;
- (void)sync;
@end

// A row of buttons for an enum: one selected. Colours may differ per option (the EQ banks).
@interface AXSegments : NSView
@property (weak) id<AXHost> host;
@property (nonatomic, copy) NSString* pid;
@property (nonatomic) NSArray<NSString*>* titles;
@property (nonatomic) NSArray<NSColor*>* colors;
@property (nonatomic) NSColor* tint;
@property (nonatomic) NSInteger selected;
@property (nonatomic, copy) NSString* caption;
- (void)sync;
@end

// One stage in the signal path: a light that switches it, its name and a live readout; drag to reorder.
@interface AXChip : NSView
@property (nonatomic) int stage;
@property (nonatomic, copy) NSString* title;
@property (nonatomic, copy) NSString* subtitle;
@property (nonatomic) NSColor* tint;
@property (nonatomic) BOOL on, selected;
@property (copy) void (^onSelect)(void);
@property (copy) void (^onToggle)(void);
@property (copy) void (^onDragBegan)(AXChip*);
@property (copy) void (^onDragMoved)(AXChip*, CGFloat);   // the chip's x in its superview
@property (copy) void (^onDragEnded)(AXChip*);
@end

// A small labelled button in the header (HELP, METER MODES, AUTO GAIN, BYPASS ...).
@interface AXPill : NSView
@property (nonatomic, copy) NSString* title;
@property (nonatomic) NSColor* tint;
@property (nonatomic) BOOL on;
@property (nonatomic) BOOL sticky;                 // keeps its on/off look; otherwise a plain button
@property (copy) void (^onClick)(void);
@end

// ---------------------------------------------------------------- graphs
@interface AXGraph : NSView
@property (weak) id<AXHost> host;
- (void)sync;                                      // pull the latest values and telemetry, redraw if they changed
- (BOOL)tick;                                      // ease toward the targets one frame; YES while still moving
@end
// EQ, Auto EQ and Program EQ: the correction curves on one spectrum.
@interface AXSpectrumGraph : AXGraph
@property (nonatomic) int stage;                   // which stage is shown: it draws its own curve boldest
@end
@interface AXBassMonoGraph : AXGraph
@end
@interface AXReverbGraph : AXGraph
@end
@interface AXWidenerGraph : AXGraph
@end
@interface AXBusCompGraph : AXGraph
@end
@interface AXLimiterGraph : AXGraph
@end
// The levels column: IN and OUT bars with LUFS, RMS or PEAK, their readouts, Auto Gain and Bypass.
@interface AXLevels : AXGraph
@property (copy) void (^onAutoGain)(void);
@property (copy) void (^onBypass)(void);
@end
