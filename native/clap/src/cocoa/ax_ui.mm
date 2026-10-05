// ax_ui.mm: palette, text, value formatting and the telemetry decoder.
#include "ax_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace axui {

// ---------------------------------------------------------------- stages
unsigned stageAccent(int stage) {
    switch (stage) {
    case StageAutoEq: return 0xEAC15F;      // gold
    case StageEq: return 0x5CB0E8;          // sky
    case StageBusComp: return 0xB58BFF;     // violet
    case StageLimiter: return 0xFF6B57;     // coral
    case StageBassMono: return 0x7FD46A;    // green
    case StageReverb: return 0x46B6E0;      // blue
    case StageWidener: return 0xCE9BF0;     // lavender
    case StageProgram: return 0xF08A5D;     // warm orange
    default: return 0x566072;
    }
}
NSString* stageName(int stage) {
    switch (stage) {
    case StageAutoEq: return @"AUTO EQ";
    case StageEq: return @"EQ";
    case StageBusComp: return @"BUS COMP";
    case StageLimiter: return @"LIMITER";
    case StageBassMono: return @"BASS MONO";
    case StageReverb: return @"REVERB";
    case StageWidener: return @"WIDENER";
    case StageProgram: return @"PROGRAM EQ";
    default: return @"";
    }
}

// ---------------------------------------------------------------- palette & text
NSColor* uiColor(unsigned rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb>>16)&255)/255. green:((rgb>>8)&255)/255. blue:(rgb&255)/255. alpha:alpha];
}
const unsigned kBg = 0x0B0D10, kBgSunken = 0x070809, kSurface = 0x14171C, kSurfaceRaised = 0x1B1F26, kSurfaceHover = 0x212632;
const unsigned kBorder = 0x232830, kBorderStrong = 0x2E353F;
const unsigned kText = 0xE6EAF0, kTextDim = 0x8A94A3, kTextMute = 0x566072;
const unsigned kAccent = 0x35E0C8, kDanger = 0xFF6B57, kWarn = 0xF3B14E, kOk = 0x4FD08A;

void uiDrawText(NSString* string, NSRect rect, CGFloat size, NSColor* tint, NSFontWeight weight, CGFloat kern, NSTextAlignment alignment) {
    auto* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = alignment; style.lineBreakMode = NSLineBreakByClipping;
    [string drawInRect:rect withAttributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:size weight:weight],
        NSForegroundColorAttributeName: tint, NSKernAttributeName: @(kern), NSParagraphStyleAttributeName: style}];
}
void uiDrawMono(NSString* string, NSRect rect, CGFloat size, NSColor* tint, NSFontWeight weight, NSTextAlignment alignment) {
    auto* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = alignment; style.lineBreakMode = NSLineBreakByClipping;
    [string drawInRect:rect withAttributes:@{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:size weight:weight],
        NSForegroundColorAttributeName: tint, NSParagraphStyleAttributeName: style}];
}
void uiFillRect(NSRect r, NSColor* color) { [color setFill]; NSRectFillUsingOperation(r, NSCompositingOperationSourceOver); }
void uiFillPanel(NSRect r, CGFloat radius, NSColor* fill, NSColor* stroke) {
    auto* path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, .5, .5) xRadius:radius yRadius:radius];
    [fill setFill]; [path fill];
    if (stroke) { [stroke setStroke]; path.lineWidth = 1; [path stroke]; }
}
NSBezierPath* uiPolyline(const std::vector<NSPoint>& points) {
    auto* path = [NSBezierPath bezierPath];
    for (size_t i = 0; i < points.size(); ++i) { if (i == 0) [path moveToPoint:points[i]]; else [path lineToPoint:points[i]]; }
    path.lineJoinStyle = NSLineJoinStyleRound;
    return path;
}
NSString* uiPretty(const std::string& s) {
    NSString* t = [NSString stringWithUTF8String:s.c_str()];
    return [[t stringByReplacingOccurrencesOfString:@"_" withString:@" "] uppercaseString];
}

// ---------------------------------------------------------------- value text
NSString* uiFormat(const Model& model, const std::string& id, double v) {
    const Meta* m = model.find(id);
    if (!m) return [NSString stringWithFormat:@"%.2f", v];
    const std::string& unit = m->unit;
    if (unit == "enum" && !m->options.empty()) {
        const int idx = static_cast<int>(std::clamp<long>(std::lround(v-m->min), 0, static_cast<long>(m->options.size())-1));
        return uiPretty(m->options[static_cast<size_t>(idx)]);
    }
    if (unit == "dB") return [NSString stringWithFormat:@"%@%.1f dB", v >= 0 ? @"+" : @"", v];
    if (unit == "dBFS") return [NSString stringWithFormat:@"%.1f dBFS", v];
    if (unit == "LUFS") return [NSString stringWithFormat:@"%.1f L", v];
    if (unit == "Hz") return v >= 1000 ? [NSString stringWithFormat:v >= 10000 ? @"%.1f kHz" : @"%.2f kHz", v*.001] : [NSString stringWithFormat:@"%.0f Hz", v];
    if (unit == "ms") return [NSString stringWithFormat:@"%.0f ms", v];
    if (unit == "switch") return v >= .5 ? @"ON" : @"OFF";
    if (unit.empty() && m->max <= 1) return [NSString stringWithFormat:@"%.0f%%", v*100];
    return [NSString stringWithFormat:@"%.2f", v];
}

// ---------------------------------------------------------------- telemetry
static void fill(std::vector<float>& out, id array) {
    out.clear();
    if (![array isKindOfClass:NSArray.class]) return;
    for (id v in (NSArray*)array) out.push_back([v isKindOfClass:NSNumber.class] ? [v floatValue] : 0.f);
}
template <size_t N> static bool fillArray(std::array<float, N>& out, id array) {
    if (![array isKindOfClass:NSArray.class]) return false;
    NSArray* a = array;
    for (size_t i = 0; i < N; ++i) out[i] = i < a.count && [a[i] isKindOfClass:NSNumber.class] ? [a[i] floatValue] : 0.f;
    return true;
}
static void shiftIn(std::array<float, kHistory>& hist, float v) {
    for (int i = 0; i+1 < kHistory; ++i) hist[static_cast<size_t>(i)] = hist[static_cast<size_t>(i+1)];
    hist[kHistory-1] = v;
}
static float num(NSDictionary* d, NSString* key, float fallback) {
    id v = d[key]; return [v isKindOfClass:NSNumber.class] ? [v floatValue] : fallback;
}
static void level(NSDictionary* d, Level& out) {
    if (![d isKindOfClass:NSDictionary.class]) return;
    out.lufs_s = num(d, @"lufs_s", -120); out.lufs_m = num(d, @"lufs_m", -120); out.rms = num(d, @"rms", -120); out.peak = num(d, @"peak", -120);
}

std::string decodeTelemetry(const char* js, Model& model) {
    if (!js) return "";
    const char* open = std::strchr(js, '(');
    if (!open) return "";
    const std::string name(js, static_cast<size_t>(open-js));
    if (name.rfind("axon", 0) != 0) return "";
    const char* end = js+std::strlen(js);
    while (end > open && (*(end-1) == ';' || *(end-1) == ' ' || *(end-1) == '\n')) --end;
    if (end <= open+1 || *(end-1) != ')') return "";
    NSData* payload = [NSData dataWithBytes:open+1 length:static_cast<NSUInteger>(end-1-(open+1))];
    Telemetry& t = model.t;
    // axonSetParam("ID",value) is not JSON: handle it by hand.
    if (name == "axonSetParam") {
        const char* q1 = std::strchr(open, '"'); if (!q1) return "";
        const char* q2 = std::strchr(q1+1, '"'); if (!q2) return "";
        const char* comma = std::strchr(q2, ','); if (!comma) return "";
        model.values[std::string(q1+1, static_cast<size_t>(q2-q1-1))] = std::atof(comma+1);
        ++t.generation;
        return name;
    }
    id parsed = [NSJSONSerialization JSONObjectWithData:payload options:0 error:nil];
    if (![parsed isKindOfClass:NSDictionary.class]) return "";
    NSDictionary* d = parsed;
    if (name == "axonSpectrum") {
        t.spectrumOrder.clear();
        if ([d[@"order"] isKindOfClass:NSArray.class]) for (id v in (NSArray*)d[@"order"]) t.spectrumOrder.push_back([v intValue]);
        t.spectrumDb.clear();
        if ([d[@"db"] isKindOfClass:NSArray.class]) for (id row in (NSArray*)d[@"db"]) { std::vector<float> r; fill(r, row); t.spectrumDb.push_back(std::move(r)); }
        t.haveEqBands = fillArray(t.eqBands, d[@"eq"]);
        t.haveEqBins = fillArray(t.eqBins, d[@"eq_bins"]);
    } else if (name == "axonSslCurve") {
        t.sslOn = [d[@"on"] boolValue];
        t.sslSelected = std::clamp(static_cast<int>(std::lround(num(d, @"selected", 0))), 0, 2);
        t.sslHave = {false, false, false};
        if ([d[@"banks"] isKindOfClass:NSArray.class]) {
            NSArray* banks = d[@"banks"];
            for (NSUInteger b = 0; b < 3 && b < banks.count; ++b) t.sslHave[b] = fillArray(t.ssl[b], banks[b]);
        } else t.sslHave[0] = fillArray(t.ssl[0], d[@"bins"]);
    } else if (name == "axonPultecCurve") {
        t.pulOn = [d[@"on"] boolValue] && fillArray(t.pul, d[@"bins"]);
    } else if (name == "axonMeters") {
        t.haveMeters = true;
        level(d[@"in"], t.in); level(d[@"out"], t.out);
    } else if (name == "axonLimiter") {
        t.haveLimiter = true;
        t.limiterActive = d[@"active"] == nil || [d[@"active"] boolValue];
        t.brick = num(d, @"brick", 0); t.ceiling = num(d, @"ceiling", 0);
        fill(t.limF, d[@"f"]); fill(t.limLvl, d[@"lvl"]); fill(t.limGr, d[@"gr"]);
        float band = 0;
        for (float g : t.limGr) band = std::min(band, g);
        shiftIn(t.grHist, t.brick); shiftIn(t.grBandHist, band);
        if (t.grLen < kHistory) ++t.grLen;
    } else if (name == "axonBusComp") {
        t.haveBusComp = true;
        t.bcActive = d[@"active"] == nil || [d[@"active"] boolValue];
        t.bcDist = num(d, @"distortion", -48); t.bcCrest = num(d, @"crest", 0);
        shiftIn(t.bcHist, t.bcDist); shiftIn(t.bcCrestHist, t.bcCrest);
        if (t.bcLen < kHistory) ++t.bcLen;
    } else return "";
    ++t.generation;
    return name;
}

}  // namespace axui
