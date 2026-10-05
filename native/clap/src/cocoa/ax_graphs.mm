// ax_graphs.mm: the graphs that show a stage's DSP: the EQ curves on a spectrum, the bass-mono crossover, the reverb's
// decay by frequency and the widener's side gain. Their math comes from ../gui_math.hpp, which tests/test_gui_math.cpp
// checks against the real DSP.
#include "ax_ui.h"
#include "../gui_math.hpp"
#include <algorithm>
#include <cmath>

using namespace axui;

namespace {
constexpr double kSampleRate = 48000;
const unsigned kBankColors[3] {0x5CB0E8, 0x35E0C8, 0xF07BC6};

struct Plot {
    NSRect r;
    double minX = 20, maxX = 20000, minY = -12, maxY = 12;
    bool logX = true;
    CGFloat x(double v) const {
        const double t = logX ? std::log(std::max(v, minX)/minX)/std::log(maxX/minX) : (v-minX)/(maxX-minX);
        return r.origin.x+r.size.width*static_cast<CGFloat>(t);
    }
    CGFloat y(double v) const { return r.origin.y+r.size.height*static_cast<CGFloat>(1-(v-minY)/(maxY-minY)); }
};
NSRect plotRect(NSRect bounds) { return NSMakeRect(bounds.origin.x+44, bounds.origin.y+34, bounds.size.width-62, bounds.size.height-64); }
void caption(NSString* text, NSRect bounds) {
    uiDrawText(text.uppercaseString, NSMakeRect(bounds.origin.x+16, bounds.origin.y+11, bounds.size.width-32, 11), 8.5, uiColor(kTextMute), NSFontWeightBold, .9, NSTextAlignmentLeft);
}
// A frequency (log) axis: the grid, its decade labels, and a label for each horizontal mark.
void frequencyGrid(const Plot& p, const std::vector<double>& marks, NSString* (^label)(double), double zeroAt = NAN) {
    uiFillPanel(p.r, 4, uiColor(kBgSunken, .8), nil);
    auto* grid = [NSBezierPath bezierPath];
    for (double v : marks) { [grid moveToPoint:NSMakePoint(p.r.origin.x, p.y(v))]; [grid lineToPoint:NSMakePoint(NSMaxX(p.r), p.y(v))]; }
    for (double f : {30., 40., 50., 60., 70., 80., 90., 100., 200., 300., 400., 500., 600., 700., 800., 900., 1000., 2000., 3000., 4000., 5000., 6000., 7000., 8000., 9000., 10000.}) {
        [grid moveToPoint:NSMakePoint(p.x(f), p.r.origin.y)]; [grid lineToPoint:NSMakePoint(p.x(f), NSMaxY(p.r))];
    }
    grid.lineWidth = .5; [uiColor(0xFFFFFF, .045) setStroke]; [grid stroke];
    if (!std::isnan(zeroAt)) {
        auto* zero = [NSBezierPath bezierPath];
        [zero moveToPoint:NSMakePoint(p.r.origin.x, p.y(zeroAt))]; [zero lineToPoint:NSMakePoint(NSMaxX(p.r), p.y(zeroAt))];
        zero.lineWidth = 1; [uiColor(kTextDim, .45) setStroke]; [zero stroke];
    }
    for (double v : marks) uiDrawText(label(v), NSMakeRect(p.r.origin.x-38, p.y(v)-6, 32, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    for (double f : {100., 1000., 10000.})
        uiDrawText(f >= 1000 ? [NSString stringWithFormat:@"%.0fk", f/1000] : [NSString stringWithFormat:@"%.0f", f], NSMakeRect(p.x(f)-20, NSMaxY(p.r)+5, 40, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentCenter);
}
std::vector<double> logFrequencies(int n) {
    std::vector<double> f(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) f[static_cast<size_t>(i)] = 20*std::pow(1000., i/static_cast<double>(n-1));
    return f;
}
std::vector<double> binFrequencies() {
    std::vector<double> f(kBins);
    for (int i = 0; i < kBins; ++i) f[static_cast<size_t>(i)] = 20*std::pow(1000., i/static_cast<double>(kBins-1));
    return f;
}
NSString* hzText(double hz) { return hz >= 1000 ? [NSString stringWithFormat:@"%.2g kHz", hz*.001] : [NSString stringWithFormat:@"%.0f Hz", hz]; }
double ease(double current, double target, double k) { return current+(target-current)*k; }

// RBJ cookbook biquads, for the Auto EQ's five band gains.
struct Coeffs { double b0, b1, b2, a1, a2; };
Coeffs rbj(int kind, double fc, double q, double gainDb, double sr) {          // 0 low shelf, 1 peaking, 2 high shelf
    const double A = std::pow(10., gainDb/40), w0 = 2*M_PI*fc/sr, cw = std::cos(w0), sw = std::sin(w0), alpha = sw/(2*q);
    double b0, b1, b2, a0, a1, a2;
    if (kind == 1) { b0 = 1+alpha*A; b1 = -2*cw; b2 = 1-alpha*A; a0 = 1+alpha/A; a1 = -2*cw; a2 = 1-alpha/A; }
    else {
        const double s = 2*std::sqrt(A)*alpha;
        if (kind == 0) { b0 = A*((A+1)-(A-1)*cw+s); b1 = 2*A*((A-1)-(A+1)*cw); b2 = A*((A+1)-(A-1)*cw-s); a0 = (A+1)+(A-1)*cw+s; a1 = -2*((A-1)+(A+1)*cw); a2 = (A+1)+(A-1)*cw-s; }
        else { b0 = A*((A+1)+(A-1)*cw+s); b1 = -2*A*((A-1)+(A+1)*cw); b2 = A*((A+1)+(A-1)*cw-s); a0 = (A+1)-(A-1)*cw+s; a1 = 2*((A-1)-(A+1)*cw); a2 = (A+1)-(A-1)*cw-s; }
    }
    return {b0/a0, b1/a0, b2/a0, a1/a0, a2/a0};
}
double responseDb(const Coeffs& c, double hz, double sr) {
    nablafx::BiquadTDF2 q; q.set(c.b0, c.b1, c.b2, c.a1, c.a2);
    return nablafx::guimath::to_db(std::abs(nablafx::guimath::biquad_response(q, hz, sr)));
}
}

// ---------------------------------------------------------------- base
@implementation AXGraph
- (BOOL)isFlipped { return YES; }
- (void)sync { self.needsDisplay = YES; }
- (BOOL)tick { return NO; }
- (double)value:(NSString*)pid { return [_host valueFor:pid]; }
- (const Telemetry&)telemetry { return [_host model]->t; }
@end

@interface AXGraph (Reading)
- (double)value:(NSString*)pid;
- (const Telemetry&)telemetry;
@end

// ---------------------------------------------------------------- the EQ spectrum
@implementation AXSpectrumGraph {
    std::array<float, 5> dGains_;
    std::array<float, kBins> dBins_, dPul_;
    std::array<std::array<float, kBins>, 3> dSsl_;
    BOOL curveMode_;
    NSRect toggleRect_;
}
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    dGains_.fill(0); dBins_.fill(0); dPul_.fill(0); for (auto& b : dSsl_) b.fill(0);
    curveMode_ = [[NSUserDefaults standardUserDefaults] stringForKey:@"AxonEqViewMode"] ? [[[NSUserDefaults standardUserDefaults] stringForKey:@"AxonEqViewMode"] isEqualToString:@"curve"] : NO;
    return self;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)mouseDown:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if ([self telemetry].haveEqBins && NSPointInRect(p, toggleRect_)) {
        curveMode_ = !curveMode_;
        [[NSUserDefaults standardUserDefaults] setObject:curveMode_ ? @"curve" : @"bins" forKey:@"AxonEqViewMode"];
        self.needsDisplay = YES;
    }
}
- (BOOL)tick {
    const Telemetry& t = [self telemetry];
    const double k = .64;                                              // the page eases .35 a frame at 60 fps; this runs at 30
    BOOL moving = NO;
    auto step = [&](float& d, float target, float eps) { d = static_cast<float>(ease(d, target, k)); if (std::abs(d-target) > eps) moving = YES; };
    if (t.haveEqBands) for (size_t i = 0; i < 5; ++i) step(dGains_[i], t.eqBands[i], .01f);
    if (t.haveEqBins) for (size_t i = 0; i < kBins; ++i) step(dBins_[i], t.eqBins[i], .02f);
    for (size_t b = 0; b < 3; ++b) for (size_t i = 0; i < kBins; ++i) step(dSsl_[b][i], t.sslOn && t.sslHave[b] ? t.ssl[b][i] : 0.f, .02f);
    for (size_t i = 0; i < kBins; ++i) step(dPul_[i], t.pulOn ? t.pul[i] : 0.f, .02f);
    if (moving) self.needsDisplay = YES;
    return moving;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const Telemetry& t = [self telemetry];
    Plot p; p.r = plotRect(self.bounds); p.logX = true; p.minY = -12; p.maxY = 12;
    const CGFloat cy = p.y(0);
    frequencyGrid(p, {-8., -4., 4., 8.}, ^NSString*(double d) { return [NSString stringWithFormat:@"%+.0f", d]; }, 0);
    uiDrawText(@"0", NSMakeRect(p.r.origin.x-38, cy-6, 32, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    auto Y = [&](double db) { return p.y(std::clamp(db, -12., 12.)); };

    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:p.r];
    // A faint live picture of the spectrum going into the Auto EQ.
    {
        int pos = -1;
        for (size_t i = 0; i < t.spectrumOrder.size(); ++i) if (t.spectrumOrder[i] == StageAutoEq) pos = static_cast<int>(i);
        if (pos > 0 && static_cast<size_t>(pos-1) < t.spectrumDb.size() && !t.spectrumDb[static_cast<size_t>(pos-1)].empty()) {
            const auto& db = t.spectrumDb[static_cast<size_t>(pos-1)];
            std::vector<NSPoint> pts;
            const size_t n = db.size();
            for (size_t b = 0; b < n; ++b) {
                const double f = 20*std::pow(1000., static_cast<double>(b)/static_cast<double>(n-1));
                const double level = std::clamp<double>(db[b], -90, 0);
                pts.push_back(NSMakePoint(p.x(f), p.r.origin.y+p.r.size.height*static_cast<CGFloat>(1-(level+90)/90)));
            }
            auto* fillPath = uiPolyline(pts);
            [fillPath lineToPoint:NSMakePoint(NSMaxX(p.r), NSMaxY(p.r))]; [fillPath lineToPoint:NSMakePoint(p.r.origin.x, NSMaxY(p.r))]; [fillPath closePath];
            [uiColor(kTextDim, .10) setFill]; [fillPath fill];
        }
    }
    const BOOL haveShape = curveMode_ ? t.haveEqBands : t.haveEqBins;
    const BOOL haveAuto = t.haveEqBins, haveSsl = t.sslOn, havePul = t.pulOn;
    const bool autoFocus = _stage == StageAutoEq, eqFocus = _stage == StageEq, pulFocus = _stage == StageProgram;
    if (haveShape) {
        std::vector<double> xs; std::vector<double> ys;
        if (!curveMode_) {
            const auto freqs = binFrequencies();
            for (int i = 0; i < kBins; ++i) { xs.push_back(p.x(freqs[static_cast<size_t>(i)])); ys.push_back(Y(dBins_[static_cast<size_t>(i)])); }
        } else {
            struct Band { int kind; double fc, q; };
            static const Band bands[5] {{0, 1010, .707}, {1, 110, .707}, {1, 1100, .707}, {1, 7000, .707}, {2, 10000, .707}};
            for (double f : logFrequencies(128)) {
                double db = 0;
                for (int b = 0; b < 5; ++b) db += responseDb(rbj(bands[b].kind, bands[b].fc, bands[b].q, dGains_[static_cast<size_t>(b)], 44100), f, 44100);
                xs.push_back(p.x(f)); ys.push_back(Y(db));
            }
        }
        const size_t N = xs.size();
        auto build = [&]() { auto* path = [NSBezierPath bezierPath]; [path moveToPoint:NSMakePoint(xs[0], cy)]; for (size_t i = 0; i < N; ++i) [path lineToPoint:NSMakePoint(xs[i], ys[i])]; [path lineToPoint:NSMakePoint(xs[N-1], cy)]; [path closePath]; return path; };
        const CGFloat fillAlpha = autoFocus ? 1 : .6;
        [NSGraphicsContext saveGraphicsState];
        [NSBezierPath clipRect:NSMakeRect(p.r.origin.x, p.r.origin.y, p.r.size.width, cy-p.r.origin.y)];
        [[[NSGradient alloc] initWithStartingColor:uiColor(kAccent, .5*fillAlpha) endingColor:uiColor(kAccent, .04*fillAlpha)] drawInBezierPath:build() angle:90];
        [NSGraphicsContext restoreGraphicsState];
        [NSGraphicsContext saveGraphicsState];
        [NSBezierPath clipRect:NSMakeRect(p.r.origin.x, cy, p.r.size.width, NSMaxY(p.r)-cy)];
        [[[NSGradient alloc] initWithStartingColor:uiColor(kDanger, .03*fillAlpha) endingColor:uiColor(kDanger, .46*fillAlpha)] drawInBezierPath:build() angle:90];
        [NSGraphicsContext restoreGraphicsState];
        auto* line = [NSBezierPath bezierPath];
        for (size_t i = 0; i < N; ++i) { if (i == 0) [line moveToPoint:NSMakePoint(xs[i], ys[i])]; else [line lineToPoint:NSMakePoint(xs[i], ys[i])]; }
        line.lineWidth = autoFocus ? 2.4 : 1.75;
        [NSGraphicsContext saveGraphicsState];
        auto* glow = [[NSShadow alloc] init]; glow.shadowColor = uiColor(stageAccent(StageAutoEq), .8); glow.shadowBlurRadius = 5; glow.shadowOffset = NSZeroSize; [glow set];
        [uiColor(stageAccent(StageAutoEq), eqFocus || pulFocus ? .6 : 1) setStroke]; [line stroke];
        [NSGraphicsContext restoreGraphicsState];
        if (!curveMode_) for (size_t i = 0; i < N; ++i) {
            [(ys[i] <= cy ? uiColor(kAccent, .85) : uiColor(kDanger, .85)) setFill];
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(xs[i]-2, ys[i]-2, 4, 4)] fill];
        }
    }
    if (haveSsl || haveAuto || havePul) {
        const auto freqs = binFrequencies();
        auto drawLine = [&](const float* vals, NSColor* color, CGFloat width, std::vector<CGFloat> dash, NSColor* glow) {
            auto* path = [NSBezierPath bezierPath];
            for (int i = 0; i < kBins; ++i) { const NSPoint pt = NSMakePoint(p.x(freqs[static_cast<size_t>(i)]), Y(vals[i])); if (i == 0) [path moveToPoint:pt]; else [path lineToPoint:pt]; }
            path.lineWidth = width;
            if (!dash.empty()) [path setLineDash:dash.data() count:static_cast<NSInteger>(dash.size()) phase:0];
            [NSGraphicsContext saveGraphicsState];
            if (glow) { auto* sh = [[NSShadow alloc] init]; sh.shadowColor = glow; sh.shadowBlurRadius = 6; sh.shadowOffset = NSZeroSize; [sh set]; }
            [color setStroke]; [path stroke];
            [NSGraphicsContext restoreGraphicsState];
        };
        if (haveSsl) {
            const std::vector<CGFloat> dashes[3] {{6, 3}, {3, 3}, {9, 3, 2, 3}};
            for (int b = 0; b < 3; ++b) {
                const BOOL sel = b == t.sslSelected;
                NSColor* color = uiColor(kBankColors[b], eqFocus || sel ? 1 : .55);
                drawLine(dSsl_[static_cast<size_t>(b)].data(), color, sel ? (eqFocus ? 2.8 : 2.4) : 1.35, dashes[b], sel ? uiColor(kBankColors[b], .35) : nil);
            }
        }
        if (havePul) drawLine(dPul_.data(), uiColor(stageAccent(StageProgram), pulFocus ? 1 : .7), pulFocus ? 2.6 : 2, {}, uiColor(stageAccent(StageProgram), .35));
        if (haveSsl || haveAuto) {
            std::array<float, kBins> total;
            for (size_t i = 0; i < kBins; ++i) total[i] = (haveAuto ? dBins_[i] : 0.f)+(haveSsl ? dSsl_[0][i] : 0.f);
            drawLine(total.data(), uiColor(kText, .9), 2, {}, uiColor(kText, .3));
        }
    }
    [NSGraphicsContext restoreGraphicsState];
    // Legend, top left of the plot.
    if (haveSsl || haveAuto || havePul) {
        struct Entry { unsigned rgb; NSString* label; };
        std::vector<Entry> legend {{kText, @"AUTO+ST"}, {stageAccent(StageAutoEq), @"AUTO EQ"}, {kBankColors[0], @"STEREO"}, {kBankColors[1], @"MID"}, {kBankColors[2], @"SIDE"}};
        if (havePul) legend.push_back({stageAccent(StageProgram), @"PROGRAM"});
        CGFloat x = p.r.origin.x+8;
        for (const auto& e : legend) {
            uiFillRect(NSMakeRect(x, p.r.origin.y+9, 10, 3), uiColor(e.rgb));
            uiDrawText(e.label, NSMakeRect(x+14, p.r.origin.y+4, 60, 12), 8, uiColor(kTextDim), NSFontWeightSemibold, .6, NSTextAlignmentLeft);
            x += 14+[e.label sizeWithAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:8 weight:NSFontWeightSemibold], NSKernAttributeName: @.6}].width+10;
        }
    }
    // The view switch.
    toggleRect_ = NSZeroRect;
    if (t.haveEqBins) {
        toggleRect_ = NSMakeRect(NSMaxX(self.bounds)-16-112, 8, 112, 18);
        uiFillPanel(toggleRect_, 9, uiColor(kSurface), uiColor(kBorderStrong));
        const NSRect left = NSMakeRect(toggleRect_.origin.x, toggleRect_.origin.y, 56, 18), right = NSMakeRect(toggleRect_.origin.x+56, toggleRect_.origin.y, 56, 18);
        uiDrawText(@"CURVE", NSMakeRect(left.origin.x, left.origin.y+4, left.size.width, 11), 8, curveMode_ ? uiColor(kAccent) : uiColor(kTextMute), NSFontWeightBold, .8, NSTextAlignmentCenter);
        uiDrawText(@"BINS", NSMakeRect(right.origin.x, right.origin.y+4, right.size.width, 11), 8, curveMode_ ? uiColor(kTextMute) : uiColor(kAccent), NSFontWeightBold, .8, NSTextAlignmentCenter);
    }
    if (!(haveShape || haveSsl || haveAuto || havePul))
        uiDrawText(@"WAITING FOR AUDIO", NSMakeRect(p.r.origin.x, p.r.origin.y+p.r.size.height/2-8, p.r.size.width, 16), 10, uiColor(kTextMute), NSFontWeightBold, 2, NSTextAlignmentCenter);
}
@end

// ---------------------------------------------------------------- bass mono
@implementation AXBassMonoGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const double cutoff = [self value:@"BMF"];
    const BOOL on = [self value:@"BMI"] >= .5;
    NSColor* tint = uiColor(stageAccent(StageBassMono), on ? 1 : .42);
    Plot p; p.r = plotRect(self.bounds); p.minY = -48; p.maxY = 6;
    frequencyGrid(p, {0., -12., -24., -36., -48.}, ^NSString*(double d) { return [NSString stringWithFormat:@"%+.0f", d]; });
    std::vector<NSPoint> side;
    for (double f : logFrequencies(400)) side.push_back(NSMakePoint(p.x(f), p.y(std::clamp(nablafx::guimath::bass_mono_side_db(f, cutoff, kSampleRate), p.minY, p.maxY))));
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:p.r];
    // The mid, and with it the mono sum, is not touched at any frequency.
    auto* mid = [NSBezierPath bezierPath];
    [mid moveToPoint:NSMakePoint(p.r.origin.x, p.y(0))]; [mid lineToPoint:NSMakePoint(NSMaxX(p.r), p.y(0))];
    mid.lineWidth = 2; [uiColor(kTextDim, .75) setStroke]; [mid stroke];
    auto* fill = uiPolyline(side);
    [fill lineToPoint:NSMakePoint(NSMaxX(p.r), NSMaxY(p.r))]; [fill lineToPoint:NSMakePoint(p.r.origin.x, NSMaxY(p.r))]; [fill closePath];
    [uiColor(stageAccent(StageBassMono), on ? .14 : .06) setFill]; [fill fill];
    auto* stroke = uiPolyline(side);
    stroke.lineWidth = 2.4; [tint setStroke]; [stroke stroke];
    auto* mark = [NSBezierPath bezierPath];
    [mark moveToPoint:NSMakePoint(p.x(cutoff), p.r.origin.y)]; [mark lineToPoint:NSMakePoint(p.x(cutoff), NSMaxY(p.r))];
    const CGFloat dash[2] {4, 4}; [mark setLineDash:dash count:2 phase:0];
    mark.lineWidth = 1; [[tint colorWithAlphaComponent:.55] setStroke]; [mark stroke];
    [NSGraphicsContext restoreGraphicsState];
    uiDrawText(hzText(cutoff), NSMakeRect(std::min(p.x(cutoff)+6, NSMaxX(p.r)-80), p.r.origin.y+6, 80, 12), 9, tint, NSFontWeightBold, .6, NSTextAlignmentLeft);
    uiDrawText(@"MID · THE MONO SUM, NEVER TOUCHED", NSMakeRect(NSMaxX(p.r)-300, p.y(0)+5, 292, 11), 8, uiColor(kTextDim), NSFontWeightBold, .8, NSTextAlignmentRight);
    uiDrawText(@"SIDE · MONO BELOW THE CUTOFF", NSMakeRect(NSMaxX(p.r)-300, p.y(-30), 292, 11), 8, [tint colorWithAlphaComponent:.9], NSFontWeightBold, .8, NSTextAlignmentRight);
    caption([NSString stringWithFormat:@"Bass mono · the side channel through a 24 dB/oct high-pass at %@%@", hzText(cutoff), on ? @"" : @" · stage off"], self.bounds);
}
@end

// ---------------------------------------------------------------- reverb
@implementation AXReverbGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const double mix = [self value:@"RVB_MIX"], size = [self value:@"RVB_SIZE"], damp = [self value:@"RVB_DAMP"], lowcut = [self value:@"RVB_LOWCUT"], width = [self value:@"RVB_WIDTH"];
    const BOOL on = mix > .001;
    NSColor* tint = uiColor(stageAccent(StageReverb), on ? 1 : .42);
    const double rt60 = nablafx::Reverb::rt60_seconds(size);
    Plot p; p.r = plotRect(self.bounds); p.minY = 0; p.maxY = 2.4;
    std::vector<double> marks;
    for (double t = 0; t <= 2.0001; t += .5) marks.push_back(t);
    frequencyGrid(p, marks, ^NSString*(double t) { return [NSString stringWithFormat:@"%.1f s", t]; });
    const auto freqs = logFrequencies(260);
    std::vector<NSPoint> line;
    for (double f : freqs) line.push_back(NSMakePoint(p.x(f), p.y(nablafx::Reverb::rt60_at(size, damp, f, kSampleRate))));
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:p.r];
    // Below the low cut nothing is sent into the reverb: the bass stays dry.
    uiFillRect(NSMakeRect(p.r.origin.x, p.r.origin.y, p.x(lowcut)-p.r.origin.x, p.r.size.height), uiColor(kBorder, .45));
    auto* fill = uiPolyline(line);
    [fill lineToPoint:NSMakePoint(p.x(freqs.back()), NSMaxY(p.r))]; [fill lineToPoint:NSMakePoint(p.x(freqs.front()), NSMaxY(p.r))]; [fill closePath];
    [uiColor(stageAccent(StageReverb), on ? .16 : .07) setFill]; [fill fill];
    auto* stroke = uiPolyline(line);
    stroke.lineWidth = 2.4; [tint setStroke]; [stroke stroke];
    auto* size_ = [NSBezierPath bezierPath];
    [size_ moveToPoint:NSMakePoint(p.r.origin.x, p.y(rt60))]; [size_ lineToPoint:NSMakePoint(NSMaxX(p.r), p.y(rt60))];
    const CGFloat dash[2] {4, 4}; [size_ setLineDash:dash count:2 phase:0];
    size_.lineWidth = 1; [[tint colorWithAlphaComponent:.5] setStroke]; [size_ stroke];
    [NSGraphicsContext restoreGraphicsState];
    uiDrawText([NSString stringWithFormat:@"RT60 %.2f s", rt60], NSMakeRect(p.x(lowcut)+8, p.y(rt60)-16, 160, 12), 9, tint, NSFontWeightBold, .6, NSTextAlignmentLeft);
    uiDrawText([NSString stringWithFormat:@"NOT SENT BELOW %@", hzText(lowcut).uppercaseString], NSMakeRect(p.r.origin.x+8, NSMaxY(p.r)-18, 200, 11), 8, uiColor(kTextDim), NSFontWeightBold, .7, NSTextAlignmentLeft);
    uiDrawText([NSString stringWithFormat:@"DAMPING %@ · PRE-DELAY %.0f MS · WIDTH %.0f%%", hzText(damp).uppercaseString, nablafx::Reverb::predelay_ms(size), width*100], NSMakeRect(NSMaxX(p.r)-420, p.r.origin.y+8, 412, 11), 8, uiColor(kTextDim), NSFontWeightBold, .7, NSTextAlignmentRight);
    caption([NSString stringWithFormat:@"Reverb · how long the tail lasts at each frequency, to −60 dB%@", on ? @"" : @" · mix is 0%: off"], self.bounds);
}
@end

// ---------------------------------------------------------------- widener
@implementation AXWidenerGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    uiFillPanel(self.bounds, 12, uiColor(kSurface), uiColor(kBorder));
    const double width = [self value:@"WID_AMT"], low = [self value:@"WID_FREQ"], air = [self value:@"WID_AIR"];
    const BOOL on = [self value:@"WID_ON"] >= .5;
    NSColor* tint = uiColor(stageAccent(StageWidener), on ? 1 : .42);
    const CGFloat side = self.bounds.size.height-64;
    NSRect image = NSMakeRect(44, 34, side, side);
    uiFillPanel(image, 4, uiColor(kBgSunken, .8), nil);
    const NSPoint c = NSMakePoint(NSMidX(image), NSMidY(image));
    auto* axes = [NSBezierPath bezierPath];
    [axes moveToPoint:NSMakePoint(image.origin.x, c.y)]; [axes lineToPoint:NSMakePoint(NSMaxX(image), c.y)];
    [axes moveToPoint:NSMakePoint(c.x, image.origin.y)]; [axes lineToPoint:NSMakePoint(c.x, NSMaxY(image))];
    axes.lineWidth = .5; [uiColor(0xFFFFFF, .06) setStroke]; [axes stroke];
    uiDrawText(@"M", NSMakeRect(c.x+5, image.origin.y+4, 14, 12), 8.5, uiColor(kTextMute), NSFontWeightBold, 0, NSTextAlignmentLeft);
    uiDrawText(@"S", NSMakeRect(NSMaxX(image)-18, c.y+4, 14, 12), 8.5, uiColor(kTextMute), NSFontWeightBold, 0, NSTextAlignmentRight);
    // The image of a source that has both mid and side in it, at a high frequency: the mid is as it was, the side is scaled by the stage.
    const double highGain = std::pow(10., nablafx::guimath::widener_side_db(10000, width, low, air, kSampleRate)/20);
    const CGFloat mid = side*.2, wide = mid*static_cast<CGFloat>(std::clamp(highGain, .02, 3.));
    auto* shape = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-wide, c.y-mid, 2*wide, 2*mid)];
    [uiColor(stageAccent(StageWidener), on ? .2 : .08) setFill]; [shape fill];
    auto* ghost = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-mid, c.y-mid, 2*mid, 2*mid)];
    const CGFloat dash[2] {3, 3}; [ghost setLineDash:dash count:2 phase:0];
    ghost.lineWidth = 1; [uiColor(kTextDim, .5) setStroke]; [ghost stroke];
    shape.lineWidth = 2.2; [tint setStroke]; [shape stroke];
    // The side gain by frequency.
    NSRect area = NSMakeRect(44+side+54, 34, self.bounds.size.width-(44+side+54)-18, side);
    Plot p; p.r = area; p.minY = -12; p.maxY = 12;
    frequencyGrid(p, {-12., -6., 6., 12.}, ^NSString*(double d) { return [NSString stringWithFormat:@"%+.0f", d]; }, 0);
    uiDrawText(@"0", NSMakeRect(p.r.origin.x-38, p.y(0)-6, 32, 12), 8.5, uiColor(kTextMute), NSFontWeightMedium, 0, NSTextAlignmentRight);
    std::vector<NSPoint> line;
    for (double f : logFrequencies(400)) line.push_back(NSMakePoint(p.x(f), p.y(std::clamp(nablafx::guimath::widener_side_db(f, width, low, air, kSampleRate), p.minY-3, p.maxY+3))));
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:p.r];
    auto* fill = uiPolyline(line);
    [fill lineToPoint:NSMakePoint(NSMaxX(p.r), p.y(0))]; [fill lineToPoint:NSMakePoint(p.r.origin.x, p.y(0))]; [fill closePath];
    [uiColor(stageAccent(StageWidener), on ? .16 : .07) setFill]; [fill fill];
    auto* stroke = uiPolyline(line);
    stroke.lineWidth = 2.4; [tint setStroke]; [stroke stroke];
    for (double f : {low, nablafx::Widener::kAirHz}) {
        auto* mark = [NSBezierPath bezierPath];
        [mark moveToPoint:NSMakePoint(p.x(f), p.r.origin.y)]; [mark lineToPoint:NSMakePoint(p.x(f), NSMaxY(p.r))];
        [mark setLineDash:dash count:2 phase:0];
        mark.lineWidth = 1; [[tint colorWithAlphaComponent:.45] setStroke]; [mark stroke];
    }
    [NSGraphicsContext restoreGraphicsState];
    uiDrawText([NSString stringWithFormat:@"LOW %@", hzText(low).uppercaseString], NSMakeRect(std::min(p.x(low)+6, NSMaxX(p.r)-90), NSMaxY(p.r)-18, 90, 11), 8, [tint colorWithAlphaComponent:.9], NSFontWeightBold, .6, NSTextAlignmentLeft);
    uiDrawText(@"AIR 6 KHZ", NSMakeRect(std::min(p.x(nablafx::Widener::kAirHz)+6, NSMaxX(p.r)-70), NSMaxY(p.r)-32, 70, 11), 8, [tint colorWithAlphaComponent:.9], NSFontWeightBold, .6, NSTextAlignmentLeft);
    uiDrawText(@"IMAGE AT 10 KHZ · MID UP, SIDE ACROSS", NSMakeRect(44, 11, 320, 11), 8.5, uiColor(kTextMute), NSFontWeightBold, .9, NSTextAlignmentLeft);
    uiDrawText([NSString stringWithFormat:@"SIDE GAIN BY FREQUENCY · THE MONO SUM IS NEVER CHANGED%@", on ? @"" : @" · STAGE OFF"], NSMakeRect(area.origin.x, 11, 460, 11), 8.5, uiColor(kTextMute), NSFontWeightBold, .9, NSTextAlignmentLeft);
}
@end
