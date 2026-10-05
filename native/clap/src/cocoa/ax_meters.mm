// ax_meters.mm: the dynamics stages' graphs (bus comp, limiter) and the levels column.
#include "ax_ui.h"
#include <algorithm>
#include <cmath>

using namespace axui;

namespace {
double ease(double current, double target, double k) { return current+(target-current)*k; }

// teal (idle) -> amber -> red (heavy gain reduction), the limiter's "how hard is it working" colour.
NSColor* grColor(double grDb, CGFloat alpha) {
    const double a = std::clamp(-grDb/9, 0., 1.);
    const double h = (168-a*158)/360., s = (55+a*25)/100., l = (52+a*6)/100.;
    // HSL -> RGB
    const double c = (1-std::abs(2*l-1))*s, hp = h*6, x = c*(1-std::abs(std::fmod(hp, 2)-1)), m = l-c/2;
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; } else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    return [NSColor colorWithSRGBRed:r+m green:g+m blue:b+m alpha:alpha];
}
void caption(NSString* text, NSRect bounds) {
    uiDrawText(text.uppercaseString, NSMakeRect(bounds.origin.x+16, bounds.origin.y+11, bounds.size.width-32, 11), 8.5, uiColor(kTextMute), NSFontWeightBold, .9, NSTextAlignmentLeft);
}
// A scrolling strip: newest on the right. `top` and `bottom` are the values at the top and bottom of the plot.
struct Strip {
    NSRect r; double top, bottom;
    CGFloat y(double v) const { return r.origin.y+r.size.height*static_cast<CGFloat>((top-std::clamp(v, std::min(top, bottom), std::max(top, bottom)))/(top-bottom)); }
    CGFloat x(int k, int len) const { return NSMaxX(r)-static_cast<CGFloat>(len-1-k)/(kHistory-1)*r.size.width; }
};
void stripGrid(const Strip& s, const std::vector<double>& marks, NSString* heading) {
    uiFillPanel(NSInsetRect(s.r, -2, -2), 4, uiColor(kBgSunken, .8), nil);
    auto* grid = [NSBezierPath bezierPath];
    for (double v : marks) { [grid moveToPoint:NSMakePoint(s.r.origin.x, s.y(v))]; [grid lineToPoint:NSMakePoint(NSMaxX(s.r), s.y(v))]; }
    grid.lineWidth = .5; [uiColor(0xFFFFFF, .05) setStroke]; [grid stroke];
    for (double v : marks) uiDrawText([NSString stringWithFormat:@"%.0f", v], NSMakeRect(s.r.origin.x-36, s.y(v)-6, 30, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    uiDrawText(heading, NSMakeRect(s.r.origin.x+4, s.r.origin.y+3, 300, 11), 8, uiColor(kTextDim), NSFontWeightBold, .8, NSTextAlignmentLeft);
}
}

// ---------------------------------------------------------------- bus comp
@implementation AXBusCompGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const Telemetry& t = [self.host model]->t;
    const BOOL on = [self.host valueFor:@"SSC"] >= .5;
    const unsigned accent = stageAccent(StageBusComp);
    NSRect plot = NSMakeRect(46, 36, self.bounds.size.width-46-48, self.bounds.size.height-36-34);
    Strip s {plot, 0, -48};
    stripGrid(s, {0, -12, -24, -36, -48}, @"DISTORTION (1−COHERENCE) → TIME");
    const Strip crest {plot, 12, 0};
    const int len = t.bcLen, first = kHistory-len;
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:plot];
    if (len > 1) {
        auto* area = [NSBezierPath bezierPath];
        [area moveToPoint:NSMakePoint(s.x(0, len), NSMaxY(plot))];
        for (int k = 0; k < len; ++k) [area lineToPoint:NSMakePoint(s.x(k, len), s.y(t.bcHist[static_cast<size_t>(first+k)]))];
        [area lineToPoint:NSMakePoint(s.x(len-1, len), NSMaxY(plot))]; [area closePath];
        [uiColor(accent, on ? .22 : .1) setFill]; [area fill];
        auto* comp = [NSBezierPath bezierPath];
        for (int k = 0; k < len; ++k) { const NSPoint pt = NSMakePoint(crest.x(k, len), NSMaxY(plot)-(NSMaxY(plot)-crest.y(t.bcCrestHist[static_cast<size_t>(first+k)]))); if (k == 0) [comp moveToPoint:pt]; else [comp lineToPoint:pt]; }
        comp.lineWidth = 1; [uiColor(stageAccent(StageEq), .45) setStroke]; [comp stroke];
        auto* line = [NSBezierPath bezierPath];
        for (int k = 0; k < len; ++k) { const NSPoint pt = NSMakePoint(s.x(k, len), s.y(t.bcHist[static_cast<size_t>(first+k)])); if (k == 0) [line moveToPoint:pt]; else [line lineToPoint:pt]; }
        line.lineWidth = 1.8; [uiColor(accent, on ? .95 : .45) setStroke]; [line stroke];
    } else {
        uiDrawText(@"WAITING FOR AUDIO", NSMakeRect(plot.origin.x, plot.origin.y+plot.size.height/2-8, plot.size.width, 16), 10, uiColor(kTextMute), NSFontWeightBold, 2, NSTextAlignmentCenter);
    }
    [NSGraphicsContext restoreGraphicsState];
    const double cur = len ? t.bcHist[kHistory-1] : -48;
    uiDrawText([NSString stringWithFormat:@"%.1f", cur], NSMakeRect(NSMaxX(plot)+5, s.y(std::max(-48., cur))-6, 40, 12), 9, uiColor(kTextDim), NSFontWeightSemibold, 0, NSTextAlignmentLeft);
    uiDrawText(@"DISTORT", NSMakeRect(NSMaxX(plot)-130, NSMaxY(plot)+6, 60, 11), 8, uiColor(accent, .95), NSFontWeightBold, .6, NSTextAlignmentRight);
    uiDrawText(@"COMP", NSMakeRect(NSMaxX(plot)-50, NSMaxY(plot)+6, 50, 11), 8, uiColor(stageAccent(StageEq), .8), NSFontWeightBold, .6, NSTextAlignmentRight);
    caption([NSString stringWithFormat:@"Bus comp · how much the model colours the program, and how much crest it takes out%@", on ? @"" : @" · stage off"], self.bounds);
}
@end

// ---------------------------------------------------------------- limiter
@implementation AXLimiterGraph {
    std::vector<float> dLvl_, dGr_;
}
- (BOOL)tick {
    const Telemetry& t = [self.host model]->t;
    if (t.limLvl.empty()) return NO;
    const size_t n = t.limLvl.size();
    if (dLvl_.size() != n) { dLvl_ = t.limLvl; dGr_ = t.limGr.size() == n ? t.limGr : std::vector<float>(n, 0.f); }
    BOOL moving = NO;
    for (size_t i = 0; i < n; ++i) {
        const float tl = t.limLvl[i], tg = i < t.limGr.size() ? t.limGr[i] : 0.f;
        dLvl_[i] = static_cast<float>(ease(dLvl_[i], tl, tl > dLvl_[i] ? .74 : .33));
        dGr_[i] = static_cast<float>(ease(dGr_[i], tg, tg < dGr_[i] ? .8 : .26));
        if (std::abs(dLvl_[i]-tl) > .05 || std::abs(dGr_[i]-tg) > .05) moving = YES;
    }
    if (moving) self.needsDisplay = YES;
    return moving;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const Telemetry& t = [self.host model]->t;
    const BOOL active = t.limiterActive;
    const unsigned accent = stageAccent(StageLimiter);
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    // ---- gain reduction over time
    const CGFloat stripH = std::min<CGFloat>(110, H*.34);
    NSRect grPlot = NSMakeRect(46, 36, W-46-48, stripH);
    Strip s {grPlot, 0, -12};
    stripGrid(s, {0, -3, -6, -12}, @"GAIN REDUCTION → TIME");
    const int len = t.grLen, first = kHistory-len;
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:grPlot];
    if (len > 1) {
        auto* area = [NSBezierPath bezierPath];
        [area moveToPoint:NSMakePoint(s.x(0, len), grPlot.origin.y)];
        for (int k = 0; k < len; ++k) [area lineToPoint:NSMakePoint(s.x(k, len), s.y(t.grHist[static_cast<size_t>(first+k)]))];
        [area lineToPoint:NSMakePoint(s.x(len-1, len), grPlot.origin.y)]; [area closePath];
        [uiColor(kDanger, .2) setFill]; [area fill];
        auto* band = [NSBezierPath bezierPath];
        for (int k = 0; k < len; ++k) { const NSPoint pt = NSMakePoint(s.x(k, len), s.y(t.grBandHist[static_cast<size_t>(first+k)])); if (k == 0) [band moveToPoint:pt]; else [band lineToPoint:pt]; }
        band.lineWidth = 1; [uiColor(kAccent, .45) setStroke]; [band stroke];
        auto* peak = [NSBezierPath bezierPath];
        for (int k = 0; k < len; ++k) { const NSPoint pt = NSMakePoint(s.x(k, len), s.y(t.grHist[static_cast<size_t>(first+k)])); if (k == 0) [peak moveToPoint:pt]; else [peak lineToPoint:pt]; }
        peak.lineWidth = 1.8; [uiColor(kDanger, .95) setStroke]; [peak stroke];
    }
    [NSGraphicsContext restoreGraphicsState];
    const double cur = len ? t.grHist[kHistory-1] : 0;
    uiDrawText(cur <= -.05 ? [NSString stringWithFormat:@"%.1f", cur] : @"0.0", NSMakeRect(NSMaxX(grPlot)+5, s.y(std::max(-12., cur))-6, 40, 12), 9, uiColor(kTextDim), NSFontWeightSemibold, 0, NSTextAlignmentLeft);
    uiDrawText(@"PEAK", NSMakeRect(NSMaxX(grPlot)-80, NSMaxY(grPlot)+5, 36, 11), 8, uiColor(kDanger, .95), NSFontWeightBold, .6, NSTextAlignmentRight);
    uiDrawText(@"BAND", NSMakeRect(NSMaxX(grPlot)-40, NSMaxY(grPlot)+5, 40, 11), 8, uiColor(kAccent, .8), NSFontWeightBold, .6, NSTextAlignmentRight);
    // ---- band levels with the gain reduction on each
    NSRect plot = NSMakeRect(46, NSMaxY(grPlot)+42, W-46-18, H-(NSMaxY(grPlot)+42)-30);
    uiFillPanel(NSInsetRect(plot, -2, -2), 4, uiColor(kBgSunken, .8), nil);
    const double top = 6, bottom = -60;
    auto Y = [&](double db) { return plot.origin.y+plot.size.height*static_cast<CGFloat>((top-std::clamp(db, bottom, top))/(top-bottom)); };
    auto* grid = [NSBezierPath bezierPath];
    for (double db = top; db >= bottom; db -= 12) { [grid moveToPoint:NSMakePoint(plot.origin.x, Y(db))]; [grid lineToPoint:NSMakePoint(NSMaxX(plot), Y(db))]; }
    grid.lineWidth = .5; [uiColor(0xFFFFFF, .05) setStroke]; [grid stroke];
    for (double db = top; db >= bottom; db -= 12) uiDrawText([NSString stringWithFormat:@"%.0f", db], NSMakeRect(plot.origin.x-36, Y(db)-6, 30, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    if (!dLvl_.empty()) {
        const size_t N = dLvl_.size();
        const CGFloat slot = plot.size.width/static_cast<CGFloat>(N), barW = slot*.74;
        if (t.ceiling != 0 || t.haveLimiter) {
            const CGFloat yc = Y(t.ceiling);
            auto* line = [NSBezierPath bezierPath];
            [line moveToPoint:NSMakePoint(plot.origin.x, yc)]; [line lineToPoint:NSMakePoint(NSMaxX(plot), yc)];
            const CGFloat dash[2] {4, 3}; [line setLineDash:dash count:2 phase:0];
            line.lineWidth = 1; [uiColor(kAccent, .55) setStroke]; [line stroke];
            uiDrawText(@"CEIL", NSMakeRect(NSMaxX(plot)-34, yc-6 < plot.origin.y ? yc+3 : yc-13, 30, 11), 8, uiColor(kAccent, .8), NSFontWeightBold, .6, NSTextAlignmentRight);
        }
        [NSGraphicsContext saveGraphicsState];
        [NSBezierPath clipRect:plot];
        for (size_t b = 0; b < N; ++b) {
            const double lvl = dLvl_[b], gr = dGr_[b];
            const CGFloat x = plot.origin.x+static_cast<CGFloat>(b)*slot+(slot-barW)/2, yLvl = Y(lvl), yOut = Y(lvl+gr);
            if (NSMaxY(plot)-yLvl > 0) uiFillRect(NSMakeRect(x, yLvl, barW, NSMaxY(plot)-yLvl), grColor(gr, active ? .92 : .28));
            if (gr < -.3 && yOut-yLvl > .5) uiFillRect(NSMakeRect(x, yLvl, barW, std::min<CGFloat>(yOut-yLvl, 3)), grColor(gr, active ? 1 : .3));
        }
        [NSGraphicsContext restoreGraphicsState];
        if (t.limF.size() == N) for (double tf : {100., 1000., 10000.}) {
            size_t bi = 0; double bd = 1e9;
            for (size_t b = 0; b < N; ++b) { const double d = std::abs(t.limF[b]-tf); if (d < bd) { bd = d; bi = b; } }
            uiDrawText(tf >= 1000 ? [NSString stringWithFormat:@"%.0fk", tf/1000] : [NSString stringWithFormat:@"%.0f", tf], NSMakeRect(plot.origin.x+static_cast<CGFloat>(bi)*slot+slot/2-20, NSMaxY(plot)+5, 40, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentCenter);
        }
    }
    if (!active) uiDrawText(@"LIMITER OFF", NSMakeRect(plot.origin.x, plot.origin.y+plot.size.height/2-8, plot.size.width, 16), 11, uiColor(kTextDim), NSFontWeightBold, 2.4, NSTextAlignmentCenter);
    uiDrawText(@"BAND LEVEL · IDLE → LIMITING", NSMakeRect(plot.origin.x+4, plot.origin.y+4, 300, 11), 8, uiColor(kTextDim), NSFontWeightBold, .8, NSTextAlignmentLeft);
    uiDrawText(@"■", NSMakeRect(plot.origin.x+176, plot.origin.y+3, 10, 11), 8, grColor(0, 1), NSFontWeightBold, 0, NSTextAlignmentLeft);
    uiDrawText(@"■", NSMakeRect(plot.origin.x+186, plot.origin.y+3, 10, 11), 8, grColor(-9, 1), NSFontWeightBold, 0, NSTextAlignmentLeft);
    caption(@"Limiter · gain reduction over time, and the level and gain reduction in each of 26 bands", self.bounds);
    (void)accent;
}
@end

// ---------------------------------------------------------------- levels column
@implementation AXLevels {
    AXPill* mode_[3];
    AXPill* auto_;
    AXPill* bypass_;
    NSInteger modeIndex_;
    double dispIn_, dispOut_;
}
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    dispIn_ = dispOut_ = -120;
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"AxonMeterMode"];
    modeIndex_ = [saved isEqualToString:@"rms"] ? 1 : [saved isEqualToString:@"peak"] ? 2 : 0;
    NSString* names[3] {@"LUFS", @"RMS", @"PEAK"};
    const CGFloat w = (frame.size.width-24-8)/3;
    __weak AXLevels* weak = self;
    for (int i = 0; i < 3; ++i) {
        mode_[i] = [[AXPill alloc] initWithFrame:NSMakeRect(12+i*(w+4), 30, w, 22)];
        mode_[i].title = names[i]; mode_[i].sticky = YES; mode_[i].on = i == modeIndex_;
        mode_[i].onClick = ^{ [weak setMode:i]; };
        [self addSubview:mode_[i]];
    }
    auto_ = [[AXPill alloc] initWithFrame:NSMakeRect(12, frame.size.height-44, (frame.size.width-24-6)/2, 28)];
    auto_.title = @"AUTO GAIN"; auto_.sticky = YES; auto_.tint = uiColor(kAccent); auto_.toolTip = @"Match output loudness to input for fair A/B";
    auto_.onClick = ^{ AXLevels* s = weak; if (s) [s.host setParam:@"AGN" value:[s.host valueFor:@"AGN"] >= .5 ? 0 : 1]; };
    [self addSubview:auto_];
    bypass_ = [[AXPill alloc] initWithFrame:NSMakeRect(12+(frame.size.width-24-6)/2+6, frame.size.height-44, (frame.size.width-24-6)/2, 28)];
    bypass_.title = @"BYPASS"; bypass_.sticky = YES; bypass_.tint = uiColor(kDanger); bypass_.toolTip = @"Audition the raw input (level-aligned, no DAW bypass)";
    bypass_.onClick = ^{ AXLevels* s = weak; if (s) [s.host setParam:@"BYP" value:[s.host valueFor:@"BYP"] >= .5 ? 0 : 1]; };
    [self addSubview:bypass_];
    return self;
}
- (void)setMode:(NSInteger)mode {
    modeIndex_ = mode;
    [[NSUserDefaults standardUserDefaults] setObject:mode == 1 ? @"rms" : mode == 2 ? @"peak" : @"lufs" forKey:@"AxonMeterMode"];
    for (int i = 0; i < 3; ++i) mode_[i].on = i == mode;
    self.needsDisplay = YES;
}
- (void)sync {
    auto_.on = [self.host valueFor:@"AGN"] >= .5;
    bypass_.on = [self.host valueFor:@"BYP"] >= .5;
    self.needsDisplay = YES;
}
- (float)field:(const Level&)l { return modeIndex_ == 0 ? l.lufs_s : modeIndex_ == 1 ? l.rms : l.peak; }
- (BOOL)tick {
    const Telemetry& t = [self.host model]->t;
    const double lo = modeIndex_ == 0 ? -36 : -48;
    const double tIn = t.haveMeters ? [self field:t.in] : lo, tOut = t.haveMeters ? [self field:t.out] : lo;
    dispIn_ = ease(dispIn_, tIn, .64); dispOut_ = ease(dispOut_, tOut, .64);
    const BOOL moving = std::abs(dispIn_-tIn) > .05 || std::abs(dispOut_-tOut) > .05;
    if (moving) self.needsDisplay = YES;
    return moving;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    uiFillPanel(b, 12, uiColor(kSurface), uiColor(kBorder));
    uiDrawText(@"LEVELS", NSMakeRect(0, 11, b.size.width, 12), 9, uiColor(kTextDim), NSFontWeightHeavy, 2.4, NSTextAlignmentCenter);
    const double mn = modeIndex_ == 0 ? -36 : -48, mx = 0, step = 6;
    const CGFloat barTop = 66, barBot = b.size.height-146, barH = barBot-barTop, areaL = 28, areaR = b.size.width-10, areaW = areaR-areaL, gap = 10, barW = (areaW-gap)/2;
    auto yFor = [&](double db) { return barTop+(mx-std::clamp(db, mn, mx))/(mx-mn)*barH; };
    for (double db = mx; db >= mn; db -= step) {
        const CGFloat y = yFor(db);
        uiFillRect(NSMakeRect(areaL, y, areaW, 1), uiColor(0xFFFFFF, .06));
        uiDrawText([NSString stringWithFormat:@"%.0f", db], NSMakeRect(2, y-6, areaL-6, 12), 9, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    }
    if (modeIndex_ == 0) {                                          // the streaming-loudness target zone
        const CGFloat yHi = yFor(-11), yLo = yFor(-14);
        uiFillRect(NSMakeRect(areaL, yHi, areaW, yLo-yHi), uiColor(kOk, .16));
        auto* edges = [NSBezierPath bezierPath];
        [edges moveToPoint:NSMakePoint(areaL, yHi)]; [edges lineToPoint:NSMakePoint(areaR, yHi)];
        [edges moveToPoint:NSMakePoint(areaL, yLo)]; [edges lineToPoint:NSMakePoint(areaR, yLo)];
        const CGFloat dash[2] {4, 3}; [edges setLineDash:dash count:2 phase:0];
        edges.lineWidth = 1; [uiColor(kOk, .5) setStroke]; [edges stroke];
    }
    auto fillColor = [&](double db) {
        if (modeIndex_ == 0) { if (db >= -14 && db <= -11) return uiColor(kOk); if (db > -11) return uiColor(kDanger); return uiColor(kOk, .45); }
        if (db >= -1) return uiColor(kDanger); if (db >= -6) return uiColor(kWarn); return uiColor(kAccent);
    };
    const double values[2] {dispIn_, dispOut_};
    NSString* names[2] {@"IN", @"OUT"};
    for (int i = 0; i < 2; ++i) {
        const CGFloat x = areaL+i*(barW+gap);
        uiFillRect(NSMakeRect(x, barTop, barW, barH), uiColor(0xFFFFFF, .05));
        if (values[i] > mn) { const CGFloat y = yFor(values[i]); uiFillRect(NSMakeRect(x, y, barW, barBot-y), fillColor(values[i])); }
        auto* frame = [NSBezierPath bezierPathWithRect:NSMakeRect(x+.5, barTop+.5, barW-1, barH-1)];
        frame.lineWidth = 1; [uiColor(kBorder) setStroke]; [frame stroke];
        uiDrawText(names[i], NSMakeRect(x, barBot+6, barW, 14), 12, uiColor(kTextDim), NSFontWeightBold, 0, NSTextAlignmentCenter);
        // readout
        const NSRect box = NSMakeRect(x-2, barBot+26, barW+4, 36);
        uiFillPanel(box, 8, uiColor(kSurface), uiColor(kBorder));
        const double v = values[i];
        const BOOL inZone = modeIndex_ == 0 && v >= -14 && v <= -11;
        uiDrawText(names[i], NSMakeRect(box.origin.x, box.origin.y+4, box.size.width, 9), 7, uiColor(kTextMute), NSFontWeightBold, 1.2, NSTextAlignmentCenter);
        uiDrawMono((v <= mn+.01 || v <= -119) ? @"–∞" : [NSString stringWithFormat:@"%.1f", v], NSMakeRect(box.origin.x, box.origin.y+14, box.size.width, 18), 14, inZone ? uiColor(kOk) : uiColor(kText), NSFontWeightBold, NSTextAlignmentCenter);
    }
}
@end
