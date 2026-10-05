// axon_gui_cocoa.mm
// The native macOS editor: the same C ABI as the WebView backends (axon_gui.h) and the same telemetry the plugin
// already pushes (axonSpectrum, axonMeters, ...), drawn with Cocoa instead of a web page.
//
// A header, a row of stage chips (the signal path: drag to reorder, the light switches the stage), one panel per stage
// with a graph drawn from that stage's DSP and its controls, and a levels column on the right.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "axon_gui.h"
#include "cocoa/ax_ui.h"
#include "cocoa/ax_editor.h"

using namespace axui;

// ---------------------------------------------------------------- layout
static const CGFloat kMargin = 24, kHeaderH = 84, kChipY = 92, kChipH = 56, kPanelY = 164, kPanelW = 868, kPanelH = 500, kLevelsW = 172;
static const CGFloat kWindowW = kMargin+kPanelW+16+kLevelsW+kMargin, kWindowH = kPanelY+kPanelH+kMargin;
static const CGFloat kBig = 100, kBigH = 108, kChipW = 118;

// What a stage's light switches. Some stages have a switch, some an amount (a mix, a correction); for those the light puts
// the amount at 0 and brings back what it was.
struct StageDef { int id; const char* on; bool amount; };
static const StageDef kStages[] {
    {StageBassMono, "BMI", false}, {StageEq, "SEQ_ON", false}, {StageAutoEq, "EQ", true}, {StageProgram, "PUL_ON", false},
    {StageReverb, "RVB_MIX", true}, {StageWidener, "WID_ON", false}, {StageBusComp, "SSC", false}, {StageLimiter, "MLI", false}};
static const StageDef* stageDef(int id) { for (const auto& s : kStages) if (s.id == id) return &s; return nullptr; }
static NSString* ns(const char* s) { return [NSString stringWithUTF8String:s]; }

// ---------------------------------------------------------------- a panel that draws its own labels
@interface AXPanelView : AXFlipped
@property (nonatomic) NSMutableArray<NSDictionary*>* items;
- (void)label:(NSString*)text at:(NSRect)rect size:(CGFloat)size color:(NSColor*)color kern:(CGFloat)kern align:(NSTextAlignment)align;
- (void)rule:(NSRect)rect color:(NSColor*)color;
- (void)paragraph:(NSString*)text at:(NSRect)rect color:(NSColor*)color;
@end
@implementation AXPanelView
- (instancetype)initWithFrame:(NSRect)frame { self = [super initWithFrame:frame]; if (self) _items = [NSMutableArray array]; return self; }
- (void)label:(NSString*)text at:(NSRect)rect size:(CGFloat)size color:(NSColor*)color kern:(CGFloat)kern align:(NSTextAlignment)align {
    [_items addObject:@{@"kind": @"label", @"text": text, @"rect": [NSValue valueWithRect:rect], @"size": @(size), @"color": color, @"kern": @(kern), @"align": @(align)}];
}
- (void)rule:(NSRect)rect color:(NSColor*)color { [_items addObject:@{@"kind": @"rule", @"rect": [NSValue valueWithRect:rect], @"color": color}]; }
- (void)paragraph:(NSString*)text at:(NSRect)rect color:(NSColor*)color { [_items addObject:@{@"kind": @"paragraph", @"text": text, @"rect": [NSValue valueWithRect:rect], @"color": color}]; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    for (NSDictionary* item in _items) {
        const NSRect rect = [item[@"rect"] rectValue];
        NSString* kind = item[@"kind"];
        if ([kind isEqualToString:@"label"]) uiDrawText(item[@"text"], rect, [item[@"size"] doubleValue], item[@"color"], NSFontWeightBold, [item[@"kern"] doubleValue], static_cast<NSTextAlignment>([item[@"align"] integerValue]));
        else if ([kind isEqualToString:@"rule"]) uiFillRect(rect, item[@"color"]);
        else {
            auto* style = [[NSMutableParagraphStyle alloc] init];
            style.lineBreakMode = NSLineBreakByWordWrapping; style.lineSpacing = 2;
            [item[@"text"] drawInRect:rect withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightRegular], NSForegroundColorAttributeName: item[@"color"], NSParagraphStyleAttributeName: style}];
        }
    }
}
@end

// ---------------------------------------------------------------- the help overlay
// Over a stage's panel: a scrim with the documented controls cut out, the stage's summary, and a card per topic with a line to
// what it is about. The controls underneath stay live.
@interface AXHelpView : NSView
@property (nonatomic) NSRect panelRect;                     // where the panel is, in this view
@property (nonatomic, copy) NSString* name;
@property (nonatomic, copy) NSString* summary;
@property (nonatomic) NSColor* tint;
@property (nonatomic) NSArray<NSDictionary*>* topics;       // title, body, rects (in the panel's coordinates)
@end
@implementation AXHelpView
- (BOOL)isFlipped { return YES; }
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
- (NSRect)place:(NSValue*)v { NSRect r = v.rectValue; r.origin.x += _panelRect.origin.x; r.origin.y += _panelRect.origin.y; return r; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = _panelRect;
    NSColor* tint = _tint ? _tint : uiColor(kAccent);
    // The scrim over the panel, with the targets left clear.
    auto* scrim = [NSBezierPath bezierPathWithRoundedRect:b xRadius:12 yRadius:12];
    for (NSDictionary* t in _topics) for (NSValue* v in t[@"rects"]) {
        if (v.rectValue.size.height > 250) continue;                 // the graph itself: it stays under the scrim
        const NSRect r = NSInsetRect([self place:v], -4, -4);
        if (NSContainsRect(b, r)) [scrim appendBezierPath:[NSBezierPath bezierPathWithRoundedRect:r xRadius:8 yRadius:8]];
    }
    scrim.windingRule = NSWindingRuleEvenOdd;
    [uiColor(kBg, .9) setFill]; [scrim fill];
    for (NSDictionary* t in _topics) for (NSValue* v in t[@"rects"]) {
        if (v.rectValue.size.height > 250) continue;
        auto* ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([self place:v], -4, -4) xRadius:8 yRadius:8];
        ring.lineWidth = 1; [[tint colorWithAlphaComponent:.6] setStroke]; [ring stroke];
    }
    // The overview.
    const CGFloat ox = b.origin.x+24, oy = b.origin.y;
    uiDrawText(@"MODULE GUIDE", NSMakeRect(ox, oy+14, 300, 12), 9, uiColor(kTextMute), NSFontWeightBold, 2.4, NSTextAlignmentLeft);
    uiDrawText(@"Choose another module above to keep exploring · controls stay live", NSMakeRect(b.origin.x+b.size.width-24-460, oy+14, 460, 12), 9, uiColor(kTextMute), NSFontWeightMedium, .6, NSTextAlignmentRight);
    uiDrawText(_name ? _name : @"", NSMakeRect(ox, oy+28, 400, 26), 20, tint, NSFontWeightHeavy, 2, NSTextAlignmentLeft);
    auto* style = [[NSMutableParagraphStyle alloc] init];
    style.lineBreakMode = NSLineBreakByWordWrapping; style.lineSpacing = 3;
    [_summary drawInRect:NSMakeRect(ox, oy+58, 640, 40) withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5 weight:NSFontWeightRegular], NSForegroundColorAttributeName: uiColor(kText), NSParagraphStyleAttributeName: style}];
    // The topic cards, in a row, each with a line to its targets.
    const NSUInteger n = _topics.count;
    if (!n) return;
    const CGFloat gap = 12, cardW = (b.size.width-48-(static_cast<CGFloat>(n)-1)*gap)/static_cast<CGFloat>(n), cardY = oy+106, cardH = 100;
    for (NSUInteger i = 0; i < n; ++i) {
        NSDictionary* t = _topics[i];
        const NSRect card = NSMakeRect(ox+static_cast<CGFloat>(i)*(cardW+gap), cardY, cardW, cardH);
        uiFillPanel(card, 10, uiColor(kSurfaceRaised, .97), [tint colorWithAlphaComponent:.4]);
        uiFillRect(NSMakeRect(card.origin.x+12, card.origin.y, 28, 2), tint);
        uiDrawText([t[@"title"] uppercaseString], NSMakeRect(card.origin.x+12, card.origin.y+12, cardW-24, 12), 9.5, tint, NSFontWeightHeavy, 1.4, NSTextAlignmentLeft);
        auto* body = [[NSMutableParagraphStyle alloc] init];
        body.lineBreakMode = NSLineBreakByWordWrapping; body.lineSpacing = 2;
        [t[@"body"] drawInRect:NSMakeRect(card.origin.x+12, card.origin.y+30, cardW-24, cardH-36) withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightRegular], NSForegroundColorAttributeName: uiColor(kTextDim), NSParagraphStyleAttributeName: body}];
        for (NSValue* v in t[@"rects"]) {
            if (v.rectValue.size.height > 250) continue;
            const NSRect r = [self place:v];
            const BOOL above = NSMaxY(r) < card.origin.y;
            const CGFloat sx = NSMidX(card), sy = above ? card.origin.y : NSMaxY(card);
            const CGFloat tx = NSMidX(r), ty = above ? NSMaxY(r)+5 : r.origin.y-5;
            const CGFloat bend = above ? std::max(ty+7, sy-12) : std::min(ty-7, sy+12);
            auto* path = [NSBezierPath bezierPath];
            [path moveToPoint:NSMakePoint(sx, sy)]; [path lineToPoint:NSMakePoint(sx, bend)]; [path lineToPoint:NSMakePoint(tx, bend)]; [path lineToPoint:NSMakePoint(tx, ty)];
            path.lineWidth = 1; [[tint colorWithAlphaComponent:.7] setStroke]; [path stroke];
            [tint setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(tx-2.5, ty-2.5, 5, 5)] fill];
        }
    }
}
@end

// ---------------------------------------------------------------- the editor
@interface AXEditor : NSView <AXHost> {
    @public
    void* plugin_;
    void (*onParam_)(void*, const char*, float);
    void (*onOrder_)(void*, const int*, int);
    Model model_;
    NSTimer* timer_;
    BOOL built_;
    NSMutableArray<AXChip*>* chips_;                 // in chain order
    NSMutableDictionary<NSNumber*, AXPanelView*>* panels_;
    NSMutableDictionary<NSNumber*, AXChip*>* chipOf_;
    NSMutableArray<AXKnob*>* knobs_;
    NSMutableArray<AXToggle*>* toggles_;
    NSMutableArray<AXSegments*>* segments_;
    NSMutableArray<AXGraph*>* graphs_;
    AXLevels* levels_;
    AXPill* help_;
    int selected_;
    std::unordered_map<std::string, double> stash_;  // what a light brings back
    // The EQ panel's bank-dependent controls.
    NSMutableArray<NSDictionary*>* bankBound_;
    AXPanelView* eqPanel_;
    AXPanelView* limiterPanel_;
    AXKnob* limiterKnobs_[2];
    AXToggle* limiterMode_;
    CGFloat dragSlot_;
    NSInteger dragFrom_;
    AXHelpView* helpView_;
    BOOL helpOpen_;
}
- (instancetype)initWithPlugin:(void*)plugin onParam:(void (*)(void*, const char*, float))onParam onOrder:(void (*)(void*, const int*, int))onOrder;
- (void)applyInit:(const AxonParamInfo*)params count:(int)n order:(const int*)order orderCount:(int)orderCount;
- (void)evalScript:(const char*)js;
- (void)notifyParam:(NSString*)pid value:(double)value;
- (void)refresh;
- (void)start;
- (void)stop;
- (void)select:(int)stage;
@end

@implementation AXEditor
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (instancetype)initWithPlugin:(void*)plugin onParam:(void (*)(void*, const char*, float))onParam onOrder:(void (*)(void*, const int*, int))onOrder {
    self = [super initWithFrame:NSMakeRect(0, 0, kWindowW, kWindowH)];
    if (!self) return nil;
    plugin_ = plugin; onParam_ = onParam; onOrder_ = onOrder;
    self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.wantsLayer = YES;
    chips_ = [NSMutableArray array]; panels_ = [NSMutableDictionary dictionary]; chipOf_ = [NSMutableDictionary dictionary];
    knobs_ = [NSMutableArray array]; toggles_ = [NSMutableArray array]; segments_ = [NSMutableArray array]; graphs_ = [NSMutableArray array];
    bankBound_ = [NSMutableArray array];
    selected_ = StageBassMono;
    return self;
}

// ---- host protocol
- (Model*)model { return &model_; }
- (double)valueFor:(NSString*)pid { return model_.value(pid.UTF8String); }
- (void)setParam:(NSString*)pid value:(double)value {
    const Meta* m = model_.find(pid.UTF8String);
    if (m) value = std::clamp(value, m->min, m->max);
    model_.values[pid.UTF8String] = value;
    if (onParam_) onParam_(plugin_, pid.UTF8String, static_cast<float>(value));
    ++model_.t.generation;
}
- (NSString*)textFor:(NSString*)pid value:(double)v {
    static NSArray* low = @[@"20 Hz", @"30 Hz", @"60 Hz", @"100 Hz", @"200 Hz", @"300 Hz", @"600 Hz"];
    static NSArray* high = @[@"3 kHz", @"4 kHz", @"5 kHz", @"8 kHz", @"10 kHz", @"12 kHz", @"16 kHz"];
    static NSArray* atten = @[@"3 kHz", @"4 kHz", @"5 kHz", @"10 kHz", @"20 kHz"];
    auto pick = [&](NSArray* names) { return names[static_cast<NSUInteger>(std::clamp<long>(std::lround(v), 0, static_cast<long>(names.count)-1))]; };
    if ([pid isEqualToString:@"PUL_LF_FREQ"]) return pick(low);
    if ([pid isEqualToString:@"PUL_HF_FREQ"]) return pick(high);
    if ([pid isEqualToString:@"PUL_HF_ATTEN_FREQ"]) return pick(atten);
    if ([pid hasPrefix:@"PUL_"] && ![pid hasSuffix:@"_ON"]) return [NSString stringWithFormat:@"%.1f", v];
    return uiFormat(model_, pid.UTF8String, v);
}
- (BOOL)parse:(NSString*)text for:(NSString*)pid into:(double*)out {
    const Meta* m = model_.find(pid.UTF8String);
    if (!m || m->unit == "enum" || m->unit == "switch") return NO;
    NSString* t = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].lowercaseString;
    const char* s = t.UTF8String;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s || !std::isfinite(v)) return NO;
    while (*end == ' ') ++end;
    const std::string unit(end);
    if (unit == "k" || unit == "khz") v *= 1000;
    else if (unit == "%") v = m->unit.empty() && m->max <= 1 ? v/100 : v;
    else if (unit == "s") v *= 1000;
    else if (!unit.empty() && unit != "hz" && unit != "db" && unit != "dbfs" && unit != "ms" && unit != "l") return NO;
    if (unit.empty() && m->unit.empty() && m->max <= 1 && v > 1) v /= 100;     // "30" for a percentage
    *out = std::clamp(v, m->min, m->max);
    return YES;
}

// ---- building blocks
- (AXKnob*)knob:(NSString*)pid title:(NSString*)title tint:(NSColor*)tint size:(AXKnobSize)size frame:(NSRect)frame in:(NSView*)parent {
    auto* k = [[AXKnob alloc] initWithFrame:frame];
    k.host = self; k.pid = pid; k.title = title; k.tint = tint; k.size = size;
    [parent addSubview:k]; [knobs_ addObject:k];
    return k;
}
- (AXToggle*)toggle:(NSString*)pid caption:(NSString*)caption labels:(NSArray*)labels tint:(NSColor*)tint frame:(NSRect)frame in:(NSView*)parent {
    auto* t = [[AXToggle alloc] initWithFrame:frame];
    t.host = self; t.pid = pid; t.caption = caption; t.labels = labels; t.tint = tint;
    [parent addSubview:t]; [toggles_ addObject:t];
    return t;
}
- (AXSegments*)segments:(NSString*)pid titles:(NSArray*)titles colors:(NSArray*)colors tint:(NSColor*)tint caption:(NSString*)caption frame:(NSRect)frame in:(NSView*)parent {
    auto* s = [[AXSegments alloc] initWithFrame:frame];
    s.host = self; s.pid = pid; s.titles = titles; s.colors = colors; s.tint = tint; s.caption = caption;
    [parent addSubview:s]; [segments_ addObject:s];
    return s;
}
- (NSArray<NSString*>*)optionsOf:(const char*)pid {
    const Meta* m = model_.find(pid);
    NSMutableArray* out = [NSMutableArray array];
    if (m) for (const auto& o : m->options) [out addObject:uiPretty(o)];
    return out;
}
- (AXGraph*)graph:(AXGraph*)g frame:(NSRect)frame in:(NSView*)parent {
    g.frame = frame; g.host = self;
    [parent addSubview:g]; [graphs_ addObject:g];
    return g;
}
// n big knobs evenly spread across the panel, starting at y.
- (void)knobRow:(NSArray<NSArray<NSString*>*>*)specs tint:(NSColor*)tint y:(CGFloat)y in:(NSView*)parent {
    const CGFloat n = static_cast<CGFloat>(specs.count), gap = (kPanelW-n*kBig)/(n+1);
    for (NSUInteger i = 0; i < specs.count; ++i)
        [self knob:specs[i][0] title:specs[i][1] tint:tint size:AXKnobBig frame:NSMakeRect(gap+static_cast<CGFloat>(i)*(kBig+gap), y, kBig, kBigH) in:parent];
}
- (AXPanelView*)panelFor:(int)stage {
    auto* p = [[AXPanelView alloc] initWithFrame:NSMakeRect(kMargin, kPanelY, kPanelW, kPanelH)];
    p.hidden = stage != selected_;
    panels_[@(stage)] = p;
    [self addSubview:p];
    return p;
}

// ---- the panels
- (void)buildBassMono {
    AXPanelView* p = [self panelFor:StageBassMono];
    NSColor* tint = uiColor(stageAccent(StageBassMono));
    [self graph:[AXBassMonoGraph new] frame:NSMakeRect(0, 0, kPanelW, 330) in:p];
    [self knob:@"BMF" title:@"FREQUENCY" tint:tint size:AXKnobBig frame:NSMakeRect(60, 352, kBig, kBigH) in:p];
    [p paragraph:@"Tightens the low end by removing stereo side information below a crossover, while the mid, and so the mono sum, is left exactly as it was. Lows below the frequency become mono; width above it is kept. The light on the chip turns the stage on and off." at:NSMakeRect(250, 366, 560, 80) color:uiColor(kTextDim)];
}
- (void)buildAutoEq {
    AXPanelView* p = [self panelFor:StageAutoEq];
    NSColor* tint = uiColor(stageAccent(StageAutoEq));
    auto* g = static_cast<AXSpectrumGraph*>([self graph:[AXSpectrumGraph new] frame:NSMakeRect(0, 0, kPanelW, 244) in:p]);
    g.stage = StageAutoEq;
    NSArray* classes = [self optionsOf:"CLS"];
    [self segments:@"CLS" titles:classes colors:nil tint:tint caption:@"TARGET" frame:NSMakeRect(0, 262, 440, 44) in:p];
    [p paragraph:@"Listens to the program and shapes a class-aware correction curve; the spectrum above shows it live. Target picks the family whose tonal goal guides the curve." at:NSMakeRect(470, 262, 398, 60) color:uiColor(kTextDim)];
    [self knobRow:@[@[@"EQ", @"AMOUNT"], @[@"EQR", @"RANGE"], @[@"EQB", @"BOOST"], @[@"EQS", @"SPEED"]] tint:tint y:352 in:p];
    // Move the four knobs left to leave room for the mode switches.
    for (NSUInteger i = 0; i < 4; ++i) { AXKnob* k = knobs_[knobs_.count-4+i]; k.frame = NSMakeRect(8+static_cast<CGFloat>(i)*118, 352, kBig, kBigH); }
    [self toggle:@"EQ_ENGINE" caption:@"ENGINE" labels:@[@"NEURAL", @"ADAPTIVE"] tint:tint frame:NSMakeRect(500, 370, 112, 44) in:p];
    [self toggle:@"EQ_RENDER" caption:@"RENDERER" labels:@[@"STFT", @"IIR"] tint:tint frame:NSMakeRect(620, 370, 112, 44) in:p];
    [self toggle:@"EQ_FREEZE" caption:@"CURVE" labels:@[@"LIVE", @"HOLD"] tint:tint frame:NSMakeRect(740, 370, 112, 44) in:p];
}
- (void)buildEq {
    AXPanelView* p = [self panelFor:StageEq];
    eqPanel_ = p;
    NSColor* tint = uiColor(0x5CB0E8);
    auto* g = static_cast<AXSpectrumGraph*>([self graph:[AXSpectrumGraph new] frame:NSMakeRect(0, 0, kPanelW, 200) in:p]);
    g.stage = StageEq;
    NSArray* bankColors = @[uiColor(0x5CB0E8), uiColor(0x35E0C8), uiColor(0xF07BC6)];
    [self segments:@"SEQ_MODE" titles:@[@"STEREO", @"MID", @"SIDE"] colors:bankColors tint:tint caption:@"EDIT BANK" frame:NSMakeRect(0, 212, 270, 44) in:p];
    [self segments:@"SEQ_TYPE" titles:[self optionsOf:"SEQ_TYPE"] colors:nil tint:tint caption:@"VOICING · ALL BANKS" frame:NSMakeRect(290, 212, 200, 44) in:p];
    [self toggle:@"SEQ_CAL" caption:@"ST SOLVE" labels:@[@"IDLE", @"CALIBRATE"] tint:tint frame:NSMakeRect(kPanelW-2*128-8, 212, 128, 44) in:p];
    [self toggle:@"SEQ_RESET" caption:@" " labels:@[@"IDLE", @"RESET"] tint:uiColor(kDanger) frame:NSMakeRect(kPanelW-128, 212, 128, 44) in:p];
    struct Col { const char* title; std::vector<std::pair<const char*, const char*>> items; };   // suffix, label
    const std::vector<Col> cols {
        {"HPF", {{"HPF_ON", "T:ON"}, {"HPF_F", "FREQ"}}},
        {"LF", {{"LF_BELL", "T:SHAPE"}, {"LF_F", "FREQ"}, {"LF_G", "GAIN"}}},
        {"LMF", {{"LMF_Q", "Q"}, {"LMF_F", "FREQ"}, {"LMF_G", "GAIN"}}},
        {"HMF", {{"HMF_Q", "Q"}, {"HMF_F", "FREQ"}, {"HMF_G", "GAIN"}}},
        {"HF", {{"HF_BELL", "T:SHAPE"}, {"HF_F", "FREQ"}, {"HF_G", "GAIN"}}},
        {"LPF", {{"LPF_ON", "T:ON"}, {"LPF_F", "FREQ"}}},
        {"COLOUR", {{"DRIVE", "COLOUR"}, {"@SEQ_AUTO", "ASSIST"}, {"@SEQ_SPLIT", "SPLIT"}}}};
    const CGFloat colW = 112, gap = (kPanelW-static_cast<CGFloat>(cols.size())*colW)/static_cast<CGFloat>(cols.size()-1), top = 286;
    for (size_t c = 0; c < cols.size(); ++c) {
        const CGFloat x = static_cast<CGFloat>(c)*(colW+gap);
        [p label:ns(cols[c].title) at:NSMakeRect(x, 268, colW, 12) size:8.5 color:uiColor(kTextMute) kern:1.6 align:NSTextAlignmentCenter];
        if (c > 0) [p rule:NSMakeRect(x-gap/2, 270, 1, 220) color:uiColor(kBorder)];
        for (size_t i = 0; i < cols[c].items.size(); ++i) {
            const std::string suffix = cols[c].items[i].first, label = cols[c].items[i].second;
            const CGFloat y = top+static_cast<CGFloat>(i)*70;
            NSString* pid = [NSString stringWithFormat:@"SEQ_%s", suffix.c_str()];
            if (suffix[0] == '@') pid = ns(suffix.c_str()+1);
            if (label.rfind("T:", 0) == 0) {
                AXToggle* t = [self toggle:pid caption:nil labels:suffix.find("BELL") != std::string::npos ? @[@"SHELF", @"BELL"] : @[@"OFF", @"ON"] tint:tint frame:NSMakeRect(x+10, y+4, colW-20, 28) in:p];
                if (suffix[0] != '@') [bankBound_ addObject:@{@"view": t, @"suffix": ns(suffix.c_str())}];
            } else {
                AXKnob* k = [self knob:pid title:ns(label.c_str()) tint:tint size:AXKnobMini frame:NSMakeRect(x+(colW-66)/2, y, 66, 66) in:p];
                if (suffix[0] != '@') [bankBound_ addObject:@{@"view": k, @"suffix": ns(suffix.c_str())}];
            }
        }
    }
    // The stage's own switch, as a reminder of what the chip's light does.
    [self bindEqBank];
}
- (int)bank { return std::clamp(static_cast<int>(std::lround(model_.value("SEQ_MODE"))), 0, 2); }
- (void)bindEqBank {
    static NSString* const prefixes[3] {@"SEQ_", @"SEQ_MID_", @"SEQ_SIDE_"};
    static const unsigned colors[3] {0x5CB0E8, 0x35E0C8, 0xF07BC6};
    NSString* prefix = prefixes[[self bank]];
    NSColor* tint = uiColor(colors[[self bank]]);
    for (NSDictionary* d in bankBound_) {
        id view = d[@"view"];
        NSString* pid = [prefix stringByAppendingString:d[@"suffix"]];
        if ([view isKindOfClass:AXKnob.class]) { ((AXKnob*)view).pid = pid; ((AXKnob*)view).tint = tint; }
        else { ((AXToggle*)view).pid = pid; ((AXToggle*)view).tint = tint; }
    }
}
- (void)buildProgram {
    AXPanelView* p = [self panelFor:StageProgram];
    NSColor* tint = uiColor(stageAccent(StageProgram));
    auto* g = static_cast<AXSpectrumGraph*>([self graph:[AXSpectrumGraph new] frame:NSMakeRect(0, 0, kPanelW, 248) in:p]);
    g.stage = StageProgram;
    const struct { NSString* title; std::vector<std::array<NSString*, 3>> items; CGFloat x; } groups[] {
        {@"LOW", {{@"PUL_LF_FREQ", @"FREQUENCY", @"1"}, {@"PUL_LF_BOOST", @"BOOST", @"0"}, {@"PUL_LF_ATTEN", @"ATTEN", @"0"}}, 0},
        {@"HIGH BOOST", {{@"PUL_HF_FREQ", @"FREQUENCY", @"1"}, {@"PUL_HF_BW", @"BANDWIDTH", @"0"}, {@"PUL_HF_BOOST", @"BOOST", @"0"}}, 322},
        {@"HIGH ATTEN", {{@"PUL_HF_ATTEN_FREQ", @"SELECT", @"1"}, {@"PUL_HF_ATTEN", @"ATTEN", @"0"}}, 644}};
    for (const auto& group : groups) {
        [p label:group.title at:NSMakeRect(group.x, 266, 296, 12) size:8.5 color:uiColor(kTextMute) kern:1.6 align:NSTextAlignmentLeft];
        [p rule:NSMakeRect(group.x, 282, group.items.size()*104-8, 1) color:uiColor(kBorder)];
        for (size_t i = 0; i < group.items.size(); ++i) {
            AXKnob* k = [self knob:group.items[i][0] title:group.items[i][1] tint:tint size:AXKnobBig frame:NSMakeRect(group.x+static_cast<CGFloat>(i)*104, 296, 96, kBigH) in:p];
            if ([group.items[i][2] isEqualToString:@"1"]) k.step = 1;
        }
    }
    [p paragraph:@"A passive tube program EQ for broad colour and low-end sculpting, with the classic panel: stepped frequencies and 0–10 dials. Low boost and atten share one frequency and interact: turn both up for deep extension below it with a tightening dip above. Off is a bit-exact bypass." at:NSMakeRect(0, 424, kPanelW, 60) color:uiColor(kTextDim)];
}
- (void)buildReverb {
    AXPanelView* p = [self panelFor:StageReverb];
    NSColor* tint = uiColor(stageAccent(StageReverb));
    [self graph:[AXReverbGraph new] frame:NSMakeRect(0, 0, kPanelW, 330) in:p];
    [self knobRow:@[@[@"RVB_MIX", @"MIX"], @[@"RVB_SIZE", @"SIZE"], @[@"RVB_WIDTH", @"WIDTH"], @[@"RVB_DAMP", @"DAMPING"], @[@"RVB_LOWCUT", @"LOW CUT"]] tint:tint y:352 in:p];
}
- (void)buildWidener {
    AXPanelView* p = [self panelFor:StageWidener];
    NSColor* tint = uiColor(stageAccent(StageWidener));
    [self graph:[AXWidenerGraph new] frame:NSMakeRect(0, 0, kPanelW, 330) in:p];
    const CGFloat y = 352;
    [self knob:@"WID_AMT" title:@"AMOUNT" tint:tint size:AXKnobBig frame:NSMakeRect(24, y, kBig, kBigH) in:p];
    [self knob:@"WID_FREQ" title:@"LOW" tint:tint size:AXKnobBig frame:NSMakeRect(148, y, kBig, kBigH) in:p];
    [self knob:@"WID_AIR" title:@"AIR" tint:tint size:AXKnobBig frame:NSMakeRect(272, y, kBig, kBigH) in:p];
    [p paragraph:@"Expands stereo side information above a protected low-frequency crossover, keeping the centre focused and the mono sum unchanged. Amount is the side gain above Low; Air adds extra lift above 6 kHz." at:NSMakeRect(420, 366, 440, 80) color:uiColor(kTextDim)];
}
- (void)buildBusComp {
    AXPanelView* p = [self panelFor:StageBusComp];
    NSColor* tint = uiColor(stageAccent(StageBusComp));
    [self graph:[AXBusCompGraph new] frame:NSMakeRect(0, 0, kPanelW, 330) in:p];
    [self knob:@"SSC_IN" title:@"INPUT" tint:tint size:AXKnobBig frame:NSMakeRect(60, 352, kBig, kBigH) in:p];
    [p paragraph:@"The learned glue and movement of an SSL-style mix-bus compressor. Input drives the model: more of it means more compression, colour and movement. The bright trace estimates the nonlinear character; the faint one is how much crest the compressor takes out." at:NSMakeRect(250, 366, 560, 80) color:uiColor(kTextDim)];
}
- (void)buildLimiter {
    AXPanelView* p = [self panelFor:StageLimiter];
    limiterPanel_ = p;
    NSColor* tint = uiColor(stageAccent(StageLimiter));
    [self graph:[AXLimiterGraph new] frame:NSMakeRect(0, 0, kPanelW, 330) in:p];
    [self knob:@"MLD" title:@"DRIVE" tint:tint size:AXKnobBig frame:NSMakeRect(24, 352, kBig, kBigH) in:p];
    [self knob:@"MLC" title:@"CEILING" tint:tint size:AXKnobBig frame:NSMakeRect(148, 352, kBig, kBigH) in:p];
    limiterMode_ = [self toggle:@"MLA" caption:@"PEAK CONTROL" labels:@[@"EVEN", @"DYNAMIC"] tint:tint frame:NSMakeRect(336, 352, 130, 44) in:p];
    limiterKnobs_[0] = [self knob:@"MLG" title:@"ADAPTIVE GAIN" tint:tint size:AXKnobBig frame:NSMakeRect(500, 352, kBig, kBigH) in:p];
    limiterKnobs_[1] = [self knob:@"MLS" title:@"ADAPTIVE SPEED" tint:tint size:AXKnobBig frame:NSMakeRect(624, 352, kBig, kBigH) in:p];
    [p paragraph:@"26-band perceptual limiter that spreads gain reduction across the spectrum, then a lookahead peak safety stage. Dynamic mode lets attack and release follow the program." at:NSMakeRect(24, 466, 820, 30) color:uiColor(kTextDim)];
}

// ---- the whole editor, built once the plugin has said what its parameters are
- (void)build {
    if (built_) return;
    built_ = YES;
    [self buildBassMono]; [self buildEq]; [self buildAutoEq]; [self buildProgram]; [self buildReverb]; [self buildWidener]; [self buildBusComp]; [self buildLimiter];
    __weak AXEditor* weak = self;
    levels_ = [[AXLevels alloc] initWithFrame:NSMakeRect(kMargin+kPanelW+16, kPanelY, kLevelsW, kPanelH)];
    levels_.host = self;
    [self addSubview:levels_]; [graphs_ addObject:levels_];
    help_ = [[AXPill alloc] initWithFrame:NSMakeRect(kWindowW-kMargin-84, 28, 84, 26)];
    help_.title = @"? HELP"; help_.sticky = YES; help_.toolTip = @"Show module help (?)";
    help_.onClick = ^{ AXEditor* e = weak; if (e) [e setHelpOpen:!e->helpOpen_]; };
    [self addSubview:help_];
    // The chips, one per stage, in the order the plugin gave.
    for (const auto& def : kStages) {
        auto* chip = [[AXChip alloc] initWithFrame:NSMakeRect(0, kChipY, kChipW, kChipH)];
        chip.stage = def.id; chip.title = stageName(def.id); chip.tint = uiColor(stageAccent(def.id));
        const int stage = def.id;
        chip.onSelect = ^{ [weak select:stage]; };
        chip.onToggle = ^{ [weak toggleStage:stage]; };
        chip.onDragBegan = ^(AXChip* c) { [weak dragBegan:c]; };
        chip.onDragMoved = ^(AXChip* c, CGFloat x) { [weak drag:c to:x]; };
        chip.onDragEnded = ^(AXChip* c) { [weak dragEnded:c]; };
        chipOf_[@(def.id)] = chip;
        [self addSubview:chip];
    }
    [self applyOrder];
    [self select:selected_];
}
- (void)applyInit:(const AxonParamInfo*)params count:(int)n order:(const int*)order orderCount:(int)orderCount {
    model_.meta.clear();
    for (int i = 0; i < n; ++i) {
        const AxonParamInfo& p = params[i];
        Meta m; m.name = p.name ? p.name : ""; m.unit = p.unit ? p.unit : "";
        m.min = p.min; m.max = p.max; m.def = p.def;
        for (int k = 0; k < p.n_enum_options; ++k) m.options.push_back(p.enum_options && p.enum_options[k] ? p.enum_options[k] : "");
        model_.meta[p.id] = std::move(m);
        model_.values[p.id] = p.current_value;
    }
    model_.order.clear();
    for (int i = 0; i < orderCount; ++i) if (stageDef(order[i])) model_.order.push_back(order[i]);
    for (const auto& d : kStages) if (std::find(model_.order.begin(), model_.order.end(), d.id) == model_.order.end()) model_.order.push_back(d.id);
    [self build];
    [self applyOrder];
    [self bindEqBank];
    [self refresh];
}

// ---- help
// Rectangles are in the panel's coordinates; a chip's light is in the editor's, converted when the card is laid out.
static NSValue* R(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { return [NSValue valueWithRect:NSMakeRect(x, y, w, h)]; }
- (NSValue*)lightOf:(int)stage {
    AXChip* c = chipOf_[@(stage)];
    const NSRect f = c.frame;
    return R(f.origin.x-kMargin+4, f.origin.y-kPanelY+kChipH/2-12, 28, 24);      // the chip's light, in the panel's coordinates (above it)
}
- (NSDictionary*)helpFor:(int)stage {
    NSMutableArray* topics = [NSMutableArray array];
    auto add = [&](NSString* title, NSString* body, NSArray<NSValue*>* rects) { [topics addObject:@{@"title": title, @"body": body, @"rects": rects}]; };
    NSValue* light = [self lightOf:stage];
    NSString* summary = @"";
    const CGFloat G = kBig;
    switch (stage) {
    case StageBassMono:
        summary = @"Tightens the low end by removing stereo side information below a crossover while leaving the mono sum unchanged.";
        add(@"Enable", @"Switch the low-frequency mono fold on or off. The mid signal always remains untouched.", @[light]);
        add(@"Frequency", @"Sets the crossover: lows below it become mono while stereo width above it is preserved.", @[R(60, 352, G, kBigH)]);
        add(@"Curve", @"What the side channel is left with. The mid, and with it the mono sum, is the flat line: never changed.", @[R(0, 0, kPanelW, 330)]);
        break;
    case StageEq: {
        summary = @"Three simultaneous zero-latency EQ banks: independent Stereo, Mid, and Side tone shaping, with Auto EQ assist on Stereo.";
        const CGFloat col = 126;
        add(@"Bank", @"Which bank the controls edit. All three run together; each has its own curve colour.", @[R(0, 212, 270, 44)]);
        add(@"Type", @"Voicing for all banks. Classic is the textbook cascade; Broad is fitted to measured console curves.", @[R(290, 212, 200, 44)]);
        add(@"Tone bands", @"LF and HF switch between shelf and bell; LMF and HMF have variable Q.", @[R(col, 276, 4*col-14, 214)]);
        add(@"Filters", @"High-pass for rumble, low-pass for excess top end, each with a switch.", @[R(0, 276, 112, 214), R(5*col, 276, 112, 214)]);
        add(@"Stereo assist", @"Share Auto EQ's correction with the Stereo bank, then calibrate or reset it.", @[R(kPanelW-264, 212, 264, 44), R(6*col, 276, 112, 214)]);
        break; }
    case StageAutoEq:
        summary = @"Listens to the program and continuously shapes a class-aware correction curve. The spectrum shows the result in real time.";
        add(@"Target", @"Choose the source family whose learned or measured tonal target should guide the curve.", @[R(0, 262, 440, 44)]);
        add(@"Amount", @"Blend the correction, constrain its range, and decide how much upward boost is allowed.", @[R(8, 352, 3*118-18, kBigH)]);
        add(@"Speed", @"Sets how quickly the rendered curve follows new tonal information. Slower values feel steadier.", @[R(8+3*118, 352, G, kBigH)]);
        add(@"Mode", @"Pick Neural or Adaptive analysis, STFT or low-latency IIR rendering, and freeze a useful curve.", @[R(500, 370, 352, 44)]);
        break;
    case StageProgram:
        summary = @"A passive tube program EQ for broad colour and low-end sculpting, with the classic panel: stepped frequencies and 0–10 dials.";
        add(@"Low", @"Boost and Atten share one frequency. Use one for a broad shelf, or turn both up for deep extension below the frequency with a tightening dip above.", @[R(0, 296, 3*104-8, kBigH)]);
        add(@"High boost", @"A resonant high lift at the selected frequency. Bandwidth runs from sharp (0) to broad (10).", @[R(322, 296, 3*104-8, kBigH)]);
        add(@"High atten", @"A separate high shelf cut for smoothing the top end, at its own stepped frequency.", @[R(644, 296, 2*104-8, kBigH)]);
        add(@"Main", @"Engage the stage with the light on its chip. Off is a bit-exact bypass.", @[light]);
        break;
    case StageReverb: {
        summary = @"Adds a decorrelated mastering ambience for depth and cohesion without moving the dry signal or its transients.";
        const CGFloat gap = (kPanelW-5*kBig)/6;
        auto knobAt = [&](int i) { return R(gap+static_cast<CGFloat>(i)*(kBig+gap), 352, G, kBigH); };
        add(@"Mix", @"Blends the reverb return with the dry program. Small moves are usually enough on a master.", @[knobAt(0)]);
        add(@"Space", @"Size changes decay and density; Width controls how broadly the ambience opens around the mix.", @[knobAt(1), knobAt(2)]);
        add(@"Tone", @"Damping softens the tail above its cutoff; Low Cut keeps bass energy out of the wet path.", @[knobAt(3), knobAt(4)]);
        add(@"Decay", @"How long the tail lasts at each frequency. The shaded part is not sent into the reverb at all.", @[R(0, 0, kPanelW, 330)]);
        break; }
    case StageWidener:
        summary = @"Expands stereo side information above a protected low-frequency crossover, preserving centre focus and mono compatibility.";
        add(@"Width", @"Set side gain with Amount, and keep everything below Low anchored in the centre.", @[R(24, 352, G, kBigH), R(148, 352, G, kBigH)]);
        add(@"Air", @"Adds extra high-frequency side lift for openness without widening the protected low end.", @[R(272, 352, G, kBigH)]);
        add(@"Side gain", @"The gain the stage puts on the side channel at each frequency. The mono sum is never changed.", @[R(0, 0, kPanelW, 330)]);
        break;
    case StageBusComp:
        summary = @"Applies the learned glue and movement of an SSL-style mix-bus compressor, with a live view of its program-dependent character.";
        add(@"Comp", @"Drive the model's input. More input produces more compression, colour, and movement.", @[R(60, 352, G, kBigH)]);
        add(@"Distortion", @"The bright trace estimates nonlinear character; higher on the graph means more model coloration.", @[R(0, 0, kPanelW, 330)]);
        add(@"Crest reduction", @"The faint companion trace shows how strongly the compressor is reducing transient crest.", @[R(0, 0, kPanelW, 330)]);
        break;
    case StageLimiter:
        summary = @"A 26-band perceptual limiter that redistributes gain reduction across the spectrum, followed by a lookahead peak safety stage.";
        add(@"Output", @"Drive pushes level into the limiter; Ceiling sets the highest permitted output sample.", @[R(24, 352, G, kBigH), R(148, 352, G, kBigH)]);
        add(@"Adaptive", @"Choose even or dynamic peak control, then shape spectral adaptation and its timing.", @[R(336, 352, 130, 44), R(500, 352, G, kBigH), R(624, 352, G, kBigH)]);
        add(@"Meters", @"The upper strip shows gain reduction over time; the band display reveals where limiting occurs.", @[R(0, 0, kPanelW, 330)]);
        break;
    }
    return @{@"summary": summary, @"topics": topics};
}
- (void)setHelpOpen:(BOOL)open {
    helpOpen_ = open;
    help_.on = open;
    help_.title = open ? @"DONE" : @"? HELP";
    [self updateHelp];
}
- (void)updateHelp {
    if (!helpOpen_) { helpView_.hidden = YES; return; }
    if (!helpView_) {
        helpView_ = [[AXHelpView alloc] initWithFrame:NSMakeRect(0, kHeaderH, kWindowW, kWindowH-kHeaderH)];
        helpView_.panelRect = NSMakeRect(kMargin, kPanelY-kHeaderH, kPanelW, kPanelH);
        [self addSubview:helpView_];
    }
    NSDictionary* h = [self helpFor:selected_];
    helpView_.name = stageName(selected_);
    helpView_.summary = h[@"summary"];
    helpView_.tint = uiColor(stageAccent(selected_));
    helpView_.topics = h[@"topics"];
    helpView_.hidden = NO;
    [helpView_.superview addSubview:helpView_ positioned:NSWindowAbove relativeTo:nil];
    helpView_.needsDisplay = YES;
}
- (void)keyDown:(NSEvent*)event {
    NSString* c = event.charactersIgnoringModifiers;
    if (event.keyCode == 53 && helpOpen_) { [self setHelpOpen:NO]; return; }
    if ([c isEqualToString:@"?"] && !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))) { [self setHelpOpen:!helpOpen_]; return; }
    [super keyDown:event];
}

// ---- the chip row
- (CGFloat)chipGap { return (kPanelW+16+kLevelsW-8*kChipW)/7; }
- (CGFloat)slotX:(NSUInteger)i { return kMargin+static_cast<CGFloat>(i)*(kChipW+[self chipGap]); }
- (void)applyOrder {
    [chips_ removeAllObjects];
    for (int stage : model_.order) { AXChip* c = chipOf_[@(stage)]; if (c) [chips_ addObject:c]; }
    for (NSUInteger i = 0; i < chips_.count; ++i) chips_[i].frame = NSMakeRect([self slotX:i], kChipY, kChipW, kChipH);
    self.needsDisplay = YES;
}
- (void)dragBegan:(AXChip*)chip {
    dragFrom_ = static_cast<NSInteger>([chips_ indexOfObject:chip]);
    [chip removeFromSuperview]; [self addSubview:chip];                 // on top
}
- (void)drag:(AXChip*)chip to:(CGFloat)x {
    x = std::clamp(x, kMargin, [self slotX:chips_.count-1]);
    NSRect f = chip.frame; f.origin.x = x; chip.frame = f;
    const NSInteger target = std::clamp<NSInteger>(static_cast<NSInteger>(std::lround((x-kMargin)/(kChipW+[self chipGap]))), 0, static_cast<NSInteger>(chips_.count)-1);
    NSMutableArray* order = [chips_ mutableCopy];
    [order removeObject:chip]; [order insertObject:chip atIndex:static_cast<NSUInteger>(target)];
    for (NSUInteger i = 0; i < order.count; ++i) if (order[i] != chip) ((AXChip*)order[i]).frame = NSMakeRect([self slotX:i], kChipY, kChipW, kChipH);
    dragSlot_ = target;
}
- (void)dragEnded:(AXChip*)chip {
    NSMutableArray* order = [chips_ mutableCopy];
    [order removeObject:chip]; [order insertObject:chip atIndex:static_cast<NSUInteger>(dragSlot_)];
    chips_ = order;
    std::vector<int> stages;
    model_.order.clear();
    for (AXChip* c in chips_) { model_.order.push_back(c.stage); stages.push_back(c.stage); }
    for (NSUInteger i = 0; i < chips_.count; ++i) chips_[i].frame = NSMakeRect([self slotX:i], kChipY, kChipW, kChipH);
    if (onOrder_) onOrder_(plugin_, stages.data(), static_cast<int>(stages.size()));
    self.needsDisplay = YES;
}
- (void)select:(int)stage {
    selected_ = stage;
    for (NSNumber* key in panels_) panels_[key].hidden = key.intValue != stage;
    for (AXChip* c in chips_) c.selected = c.stage == stage;
    [self updateHelp];
    [self refresh];
    self.needsDisplay = YES;
}
- (BOOL)stageOn:(const StageDef*)def { return model_.value(def->on) > 0; }
- (void)toggleStage:(int)stage {
    const StageDef* def = stageDef(stage);
    if (!def) return;
    const double now = model_.value(def->on);
    if (!def->amount) { [self setParam:ns(def->on) value:now >= .5 ? 0 : 1]; }
    else if (now > 0) { stash_[def->on] = now; [self setParam:ns(def->on) value:0]; }
    else {
        const Meta* m = model_.find(def->on);
        auto it = stash_.find(def->on);
        [self setParam:ns(def->on) value:it != stash_.end() ? it->second : (m && m->def > 0 ? m->def : 1)];
    }
    [self refresh];
}
- (NSString*)subtitleFor:(int)stage {
    auto v = [&](const char* id) { return model_.value(id); };
    switch (stage) {
    case StageBassMono: return uiFormat(model_, "BMF", v("BMF")).uppercaseString;
    case StageEq: { const Meta* m = model_.find("SEQ_TYPE"); const int i = static_cast<int>(std::lround(v("SEQ_TYPE"))); return m && i >= 0 && i < static_cast<int>(m->options.size()) ? uiPretty(m->options[static_cast<size_t>(i)]) : @"EQ"; }
    case StageAutoEq: return uiFormat(model_, "CLS", v("CLS"));
    case StageProgram: return [NSString stringWithFormat:@"%@ · %@", [self textFor:@"PUL_LF_FREQ" value:v("PUL_LF_FREQ")], [self textFor:@"PUL_HF_FREQ" value:v("PUL_HF_FREQ")]].uppercaseString;
    case StageReverb: return [NSString stringWithFormat:@"MIX %.0f%%", v("RVB_MIX")*100];
    case StageWidener: return [NSString stringWithFormat:@"AMOUNT %.2f", v("WID_AMT")];
    case StageBusComp: return [NSString stringWithFormat:@"INPUT %+.1f", v("SSC_IN")];
    case StageLimiter: { const Telemetry& t = model_.t; return t.haveLimiter && t.limiterActive && t.brick < -.05 ? [NSString stringWithFormat:@"GR %.1f dB", t.brick] : [NSString stringWithFormat:@"CEIL %.1f", v("MLC")]; }
    }
    return @"";
}

// ---- scripts from the plugin
- (void)notifyParam:(NSString*)pid value:(double)value {
    model_.values[pid.UTF8String] = value;
    ++model_.t.generation;
    if (built_) [self refresh];
}
- (void)evalScript:(const char*)js {
    if (!js) return;
    if (std::strstr(js, "axonVisible(true)")) { [self start]; return; }
    if (std::strstr(js, "axonVisible(false)")) { [self stop]; return; }
    const std::string name = decodeTelemetry(js, model_);
    if (name == "axonSetParam" && built_) [self refresh];
}

// ---- refresh
- (void)refresh {
    if (!built_) return;
    [self bindEqBank];
    // The limiter's second row of knobs is attack and release in dynamic mode.
    const BOOL dynamic = model_.value("MLA") >= .5;
    limiterKnobs_[0].title = dynamic ? @"ATTACK" : @"ADAPTIVE GAIN";
    limiterKnobs_[1].title = dynamic ? @"RELEASE" : @"ADAPTIVE SPEED";
    for (AXKnob* k in knobs_) if (!k.superview.hidden) [k sync];
    for (AXToggle* t in toggles_) if (!t.superview.hidden) [t sync];
    for (AXSegments* s in segments_) if (!s.superview.hidden) [s sync];
    for (AXChip* c in chips_) {
        const StageDef* def = stageDef(c.stage);
        c.on = def && [self stageOn:def];
        c.subtitle = [self subtitleFor:c.stage];
        const BOOL dim = !c.on;
        (void)dim;
    }
    for (AXGraph* g in graphs_) if (g == levels_ || !g.superview.hidden) { [g sync]; [g tick]; }
    const BOOL accent = NO; (void)accent;
}
- (void)start {
    if (timer_) return;
    __weak AXEditor* weak = self;
    timer_ = [NSTimer timerWithTimeInterval:1./30 repeats:YES block:^(NSTimer*) { [weak refresh]; }];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
}
- (void)stop { [timer_ invalidate]; timer_ = nil; }
- (void)dealloc { [timer_ invalidate]; }

// ---- drawing: the header and the arrows between the chips
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [uiColor(kBg) setFill]; NSRectFill(self.bounds);
    [[[NSGradient alloc] initWithStartingColor:uiColor(0x0E1116) endingColor:uiColor(kBg)] drawInRect:NSMakeRect(0, 0, self.bounds.size.width, kHeaderH) angle:90];
    // AXON, the X in the accent.
    NSMutableAttributedString* logo = [[NSMutableAttributedString alloc] initWithString:@"AXON"];
    NSDictionary* base = @{NSFontAttributeName: [NSFont systemFontOfSize:26 weight:NSFontWeightHeavy], NSKernAttributeName: @6, NSForegroundColorAttributeName: uiColor(kText)};
    [logo setAttributes:base range:NSMakeRange(0, 4)];
    [logo addAttribute:NSForegroundColorAttributeName value:uiColor(kAccent) range:NSMakeRange(1, 1)];
    [logo drawAtPoint:NSMakePoint(kMargin, 18)];
    uiDrawText(@"ADAPTIVE NEURAL MASTERING CHAIN", NSMakeRect(kMargin+2, 54, 360, 12), 8.5, uiColor(kTextMute), NSFontWeightSemibold, 2, NSTextAlignmentLeft);
    uiFillRect(NSMakeRect(0, kHeaderH-1, self.bounds.size.width, 1), uiColor(kBorder));
    const CGFloat gap = [self chipGap];
    for (int i = 0; i < 7; ++i)
        uiDrawText(@"›", NSMakeRect(kMargin+(i+1)*kChipW+i*gap, kChipY+13, gap, 28), 20, uiColor(kTextMute), NSFontWeightLight, 0, NSTextAlignmentCenter);
}
@end

// ---------------------------------------------------------------- C ABI
struct AxonGUIState {
    __strong AXEditor* editor = nil;
    __strong NSView* container = nil;
};

AxonGUIState* axon_gui_create(void* plugin_ptr, const char* resources_dir,
                              void (*on_param_change)(void*, const char*, float),
                              void (*on_order_change)(void*, const int*, int)) {
    (void)resources_dir;                          // everything is drawn; there is no page to load
    if (!NSThread.isMainThread) {
        __block AxonGUIState* result = nullptr;
        dispatch_sync(dispatch_get_main_queue(), ^{ result = axon_gui_create(plugin_ptr, resources_dir, on_param_change, on_order_change); });
        return result;
    }
    auto* state = new AxonGUIState();
    state->editor = [[AXEditor alloc] initWithPlugin:plugin_ptr onParam:on_param_change onOrder:on_order_change];
    state->container = state->editor;
    return state;
}
void axon_gui_destroy(AxonGUIState* gui) {
    if (!gui) return;
    auto block = ^{
        [gui->editor stop];
        [gui->editor removeFromSuperview];
        gui->editor = nil; gui->container = nil;
    };
    if (NSThread.isMainThread) block(); else dispatch_sync(dispatch_get_main_queue(), block);
    delete gui;
}
bool axon_gui_set_parent(AxonGUIState* gui, void* ns_view_ptr) {
    if (!gui || !ns_view_ptr) return false;
    auto block = ^bool{
        NSView* parent = (__bridge NSView*)ns_view_ptr;
        if (!parent) return false;
        gui->editor.frame = NSMakeRect(0, 0, kWindowW, kWindowH);
        [parent addSubview:gui->editor];
        return true;
    };
    if (NSThread.isMainThread) return block();
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{ result = block(); });
    return result;
}
void axon_gui_show(AxonGUIState* gui) {
    if (!gui) return;
    auto block = ^{ gui->editor.hidden = NO; [gui->editor start]; };
    if (NSThread.isMainThread) block(); else dispatch_async(dispatch_get_main_queue(), block);
}
void axon_gui_hide(AxonGUIState* gui) {
    if (!gui) return;
    auto block = ^{ gui->editor.hidden = YES; [gui->editor stop]; };
    if (NSThread.isMainThread) block(); else dispatch_async(dispatch_get_main_queue(), block);
}
void axon_gui_get_size(uint32_t* w, uint32_t* h) {
    if (w) *w = static_cast<uint32_t>(kWindowW);
    if (h) *h = static_cast<uint32_t>(kWindowH);
}
void axon_gui_send_init(AxonGUIState* gui, const AxonParamInfo* params, int n_params, const int* order, int order_count) {
    if (!gui) return;
    // The strings behind `params` are only valid for this call: the editor copies what it needs.
    auto block = ^{ [gui->editor applyInit:params count:n_params order:order orderCount:order_count]; };
    if (NSThread.isMainThread) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}
void axon_gui_eval_js(AxonGUIState* gui, const char* js) {
    if (!gui || !js || !NSThread.isMainThread) return;
    [gui->editor evalScript:js];
}
void axon_gui_notify_param(AxonGUIState* gui, const char* param_id, float value) {
    if (!gui || !param_id) return;
    NSString* pid = [NSString stringWithUTF8String:param_id];
    auto block = ^{ [gui->editor notifyParam:pid value:value]; };
    if (NSThread.isMainThread) block(); else dispatch_async(dispatch_get_main_queue(), block);
}
void* axon_gui_native_view(AxonGUIState* gui) { return gui ? (__bridge void*)gui->editor : nullptr; }
void axon_gui_native_refresh(AxonGUIState* gui) { if (gui) [gui->editor refresh]; }
void axon_gui_native_select(AxonGUIState* gui, int stage) { if (gui) [gui->editor select:stage]; }
