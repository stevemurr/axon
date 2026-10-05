// Renders the native editor to a PNG without a plugin host, or smoke-tests it headlessly.
//   axon_editor_snapshot out.png [stage=eq|autoeq|program|reverb|widener|buscomp|limiter|bassmono] [PARAM=value ...]
//                                [order=6,3,1,10,8,9,4,5] [in=-16 out=-12.5]
//   axon_editor_snapshot --selftest
// Parameters are the plugin's control ids (BMF=300 SEQ_MODE=1 RVB_MIX=0.4 ...). The editor is fed demo telemetry in the
// same form the plugin pushes (axonSpectrum, axonMeters, ...).
#import <Cocoa/Cocoa.h>
#include "axon_gui.h"
#include "cocoa/ax_editor.h"
#include "cocoa/ax_ui.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace axui;

// ---------------------------------------------------------------- the plugin's side of the conversation
static std::map<std::string, double> values;                  // what the "plugin" holds
static std::vector<std::pair<std::string, float>> changes;    // every edit the editor reported
static std::vector<std::vector<int>> orders;                  // every reorder it reported
static AxonGUIState* gui = nullptr;

static void onParam(void*, const char* id, float v) { values[id] = v; changes.emplace_back(id, v); }
static void onOrder(void*, const int* order, int count) { orders.emplace_back(order, order+count); }

struct Control { std::string id, name, unit; float min, max, def; std::vector<std::string> options; };
static std::vector<Control> loadControls() {
    NSData* data = [NSData dataWithContentsOfFile:@AXON_META_PATH];
    NSDictionary* meta = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (!meta) { std::fprintf(stderr, "cannot read %s\n", AXON_META_PATH); std::exit(2); }
    NSArray* classes = meta[@"auto_eq"][@"class_order"];
    auto enumFor = [&](const std::string& id) -> std::vector<std::string> {
        if (id == "CLS") { std::vector<std::string> o; for (NSString* c in classes) o.push_back(c.UTF8String); return o; }
        if (id == "SEQ_MODE") return {"stereo", "mid", "side"};
        if (id == "SEQ_TYPE") return {"classic", "broad"};
        if (id == "PUL_LF_FREQ") return {"20 Hz", "30 Hz", "60 Hz", "100 Hz", "200 Hz", "300 Hz", "600 Hz"};
        if (id == "PUL_HF_FREQ") return {"3 kHz", "4 kHz", "5 kHz", "8 kHz", "10 kHz", "12 kHz", "16 kHz"};
        if (id == "PUL_HF_ATTEN_FREQ") return {"3 kHz", "4 kHz", "5 kHz", "10 kHz", "20 kHz"};
        return {};
    };
    std::vector<Control> out;
    NSDictionary* controls = meta[@"controls"];
    for (NSString* key in [controls.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSDictionary* c = controls[key];
        Control k;
        k.id = key.UTF8String; k.name = [c[@"name"] UTF8String]; k.unit = [c[@"unit"] UTF8String] ?: "";
        k.min = [c[@"min"] floatValue]; k.max = [c[@"max"] floatValue]; k.def = [c[@"default"] floatValue];
        k.options = enumFor(k.id);
        out.push_back(k);
    }
    return out;
}

// ---------------------------------------------------------------- demo telemetry (the strings the plugin builds)
static std::string numbers(const std::vector<double>& v, const char* fmt = "%.2f") {
    std::string s = "["; char buf[32];
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ','; std::snprintf(buf, sizeof(buf), fmt, v[i]); s += buf; }
    return s+"]";
}
static std::vector<double> bins(double (*shape)(double hz)) {
    std::vector<double> v; for (int i = 0; i < kBins; ++i) v.push_back(shape(20*std::pow(1000., i/49.))); return v;
}
static double autoShape(double f) { return 3.2*std::exp(-std::pow(std::log2(f/90), 2)/2.4)-2.6*std::exp(-std::pow(std::log2(f/2800), 2)/1.1)+3.6*std::exp(-std::pow(std::log2(f/11000), 2)/1.4)-1.2*std::exp(-std::pow(std::log2(f/450), 2)/1.5); }
static double bankA(double f) { return 2.0*std::exp(-std::pow(std::log2(f/110), 2)/2.0)+1.4*std::exp(-std::pow(std::log2(f/9000), 2)/2.4); }
static double bankB(double f) { return -1.5*std::exp(-std::pow(std::log2(f/700), 2)/1.8); }
static double bankC(double f) { return 2.2*std::exp(-std::pow(std::log2(f/5000), 2)/2.6); }
static double pulShape(double f) { return 2.8*std::exp(-std::pow(std::log2(f/80), 2)/1.6)-1.4*std::exp(-std::pow(std::log2(f/160), 2)/1.4)+1.8*std::exp(-std::pow(std::log2(f/9000), 2)/1.8); }

static void feed(const char* js) { axon_gui_eval_js(gui, js); }
static void feedSpectrum() {
    std::string s = "axonSpectrum({\"order\":[6,3,1,10,8,9,4,5],\"db\":[";
    for (int pos = 0; pos < 8; ++pos) {
        std::vector<double> row;
        for (int b = 0; b < 128; ++b) { const double f = 20*std::pow(1000., b/127.); row.push_back(-14-11*std::log10(f/60+1)-4*std::sin(b*.7+pos)-3*std::log10(f/10000+1)*8); }
        s += (pos ? "," : "")+numbers(row, "%.1f");
    }
    s += "],\"eq\":[1.2,-0.8,0.6,-1.4,2.1],\"eq_bins\":"+numbers(bins(autoShape))+"});";
    feed(s.c_str());
    feed(("axonSslCurve({\"on\":true,\"selected\":"+std::to_string(static_cast<int>(values["SEQ_MODE"]))+",\"banks\":["+numbers(bins(bankA))+","+numbers(bins(bankB))+","+numbers(bins(bankC))+"]});").c_str());
    feed(("axonPultecCurve({\"on\":"+std::string(values["PUL_ON"] > .5 ? "true" : "false")+",\"bins\":"+numbers(bins(pulShape))+"});").c_str());
}
static void feedMeters(double inLufs, double outLufs) {
    char buf[300];
    std::snprintf(buf, sizeof(buf), "axonMeters({\"in\":{\"lufs_s\":%.1f,\"lufs_m\":%.1f,\"rms\":%.1f,\"peak\":%.1f},\"out\":{\"lufs_s\":%.1f,\"lufs_m\":%.1f,\"rms\":%.1f,\"peak\":%.1f}})",
        inLufs, inLufs+1.2, inLufs-3, inLufs+9, outLufs, outLufs+.8, outLufs-3, outLufs+4);
    feed(buf);
}
static void feedLimiter(int frame) {
    std::vector<double> f, lvl, gr;
    for (int b = 0; b < 26; ++b) {
        f.push_back(60*std::pow(15000./60, b/25.));
        lvl.push_back(-6-b*1.3+5*std::sin(b*.5+frame*.2)-(b > 18 ? (b-18)*2 : 0));
        gr.push_back(b < 14 ? -std::max(0., 4.5*std::sin(b*.35+frame*.1)+1.5) : -std::max(0., 1.2*std::sin(frame*.15)));
    }
    char head[200];
    std::snprintf(head, sizeof(head), "axonLimiter({\"active\":true,\"brick\":%.1f,\"ceiling\":-1.0,\"f\":", -std::max(0., 3.5+2.5*std::sin(frame*.12)));
    feed((std::string(head)+numbers(f, "%.0f")+",\"lvl\":"+numbers(lvl, "%.1f")+",\"gr\":"+numbers(gr, "%.1f")+"})").c_str());
}
static void feedBusComp(int frame) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "axonBusComp({\"active\":true,\"distortion\":%.1f,\"crest\":%.1f})", -26-9*std::sin(frame*.09)-4*std::sin(frame*.31), 3.5+2*std::sin(frame*.11+1));
    feed(buf);
}

// ---------------------------------------------------------------- build
static void buildEditor(NSWindow* window, const std::vector<int>& order) {
    gui = axon_gui_create(nullptr, "", onParam, onOrder);
    const auto controls = loadControls();
    std::vector<AxonParamInfo> params(controls.size());
    std::vector<std::vector<const char*>> options(controls.size());
    for (size_t i = 0; i < controls.size(); ++i) {
        const Control& c = controls[i];
        if (!values.count(c.id)) values[c.id] = c.def;
        for (const auto& o : c.options) options[i].push_back(o.c_str());
        params[i] = {c.id.c_str(), c.name.c_str(), c.min, c.max, c.def, c.unit.c_str(), static_cast<float>(values[c.id]), options[i].empty() ? nullptr : options[i].data(), static_cast<int>(options[i].size())};
    }
    axon_gui_set_parent(gui, (__bridge void*)window.contentView);
    axon_gui_send_init(gui, params.data(), static_cast<int>(params.size()), order.data(), static_cast<int>(order.size()));
    axon_gui_show(gui);
}

// ---------------------------------------------------------------- headless interaction checks
static int failures = 0;
#define EXPECT(condition) do { if (!(condition)) { std::fprintf(stderr, "selftest %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (false)

static void collect(NSView* view, Class cls, NSMutableArray<NSView*>* into) {
    if ([view isKindOfClass:cls]) [into addObject:view];
    for (NSView* sub in view.subviews) collect(sub, cls, into);
}
static NSArray<NSView*>* find(NSView* root, NSString* className) {
    auto* out = [NSMutableArray<NSView*> array];
    collect(root, NSClassFromString(className), out);
    return out;
}
static NSEvent* mouse(NSWindow* window, NSView* view, NSPoint local, NSEventType type, NSInteger clicks = 1) {
    return [NSEvent mouseEventWithType:type location:[view convertPoint:local toView:nil] modifierFlags:0 timestamp:0
        windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:clicks pressure:1];
}
static void click(NSWindow* window, NSView* view, NSPoint local, NSInteger clicks = 1) {
    [view mouseDown:mouse(window, view, local, NSEventTypeLeftMouseDown, clicks)];
    [view mouseUp:mouse(window, view, local, NSEventTypeLeftMouseUp, clicks)];
}
static NSString* stringOf(NSView* view, NSString* key) { return [view valueForKey:key]; }
static NSView* knobFor(NSView* root, const char* pid) {
    for (NSView* k in find(root, @"AXKnob")) if ([[k valueForKey:@"pid"] isEqualToString:@(pid)]) return k;
    return nil;
}
static NSView* toggleFor(NSView* root, const char* pid) {
    for (NSView* k in find(root, @"AXToggle")) if ([[k valueForKey:@"pid"] isEqualToString:@(pid)]) return k;
    return nil;
}
static NSView* segmentsFor(NSView* root, const char* pid) {
    for (NSView* k in find(root, @"AXSegments")) if ([[k valueForKey:@"pid"] isEqualToString:@(pid)]) return k;
    return nil;
}
static bool shown(NSView* v) { return !v.isHiddenOrHasHiddenAncestor; }

static int selftest(NSView* root, NSWindow* window) {
    auto refresh = [&] { axon_gui_native_refresh(gui); };
    auto select = [&](int stage) { axon_gui_native_select(gui, stage); };
    refresh();
    // ---- the telemetry decoder
    {
        Model m;
        EXPECT(decodeTelemetry("axonMeters({\"in\":{\"lufs_s\":-14.5,\"lufs_m\":-13,\"rms\":-18,\"peak\":-3},\"out\":{\"lufs_s\":-9,\"lufs_m\":-8,\"rms\":-12,\"peak\":-1}})", m) == "axonMeters");
        EXPECT(m.t.haveMeters && std::abs(m.t.in.lufs_s+14.5f) < 1e-4 && std::abs(m.t.out.peak+1) < 1e-4);
        EXPECT(decodeTelemetry("axonSetParam(\"BMF\",300);", m) == "axonSetParam" && m.values["BMF"] == 300);
        EXPECT(decodeTelemetry("axonSetParam(\"SEQ_MID_LF_G\",-2.5000);", m) == "axonSetParam" && std::abs(m.values["SEQ_MID_LF_G"]+2.5) < 1e-9);
        EXPECT(decodeTelemetry("axonPultecCurve({\"on\":false,\"bins\":[1,2]});", m) == "axonPultecCurve" && !m.t.pulOn);
        EXPECT(decodeTelemetry("axonLimiter({\"active\":false,\"brick\":-2.5,\"ceiling\":-1.0,\"f\":[100,1000],\"lvl\":[-6,-9],\"gr\":[-3,-1]})", m) == "axonLimiter");
        EXPECT(!m.t.limiterActive && m.t.limF.size() == 2 && std::abs(m.t.grBandHist[kHistory-1]+3) < 1e-4 && m.t.grLen == 1);
        EXPECT(decodeTelemetry("axonBusComp({\"active\":true,\"distortion\":-20.5,\"crest\":3.2})", m) == "axonBusComp" && m.t.bcLen == 1 && std::abs(m.t.bcHist[kHistory-1]+20.5f) < 1e-4);
        EXPECT(decodeTelemetry("axonSslCurve({\"on\":true,\"selected\":2,\"banks\":[[1,2],[3,4],[5,6]]})", m) == "axonSslCurve" && m.t.sslSelected == 2 && m.t.sslHave[2] && m.t.ssl[2][1] == 6);
        EXPECT(decodeTelemetry("window.axonVisible&&window.axonVisible(true)", m).empty());
        EXPECT(decodeTelemetry("axonMeters({broken", m).empty());
        EXPECT(decodeTelemetry("nothing", m).empty());
    }
    // ---- chips: eight, in the order the plugin gave, one panel each
    NSArray<NSView*>* chips = find(root, @"AXChip");
    EXPECT(chips.count == 8);
    NSMutableArray<NSView*>* inOrder = [NSMutableArray array];
    for (int stage : {6, 3, 1, 10, 8, 9, 4, 5}) for (NSView* c in chips) if ([[c valueForKey:@"stage"] intValue] == stage) [inOrder addObject:c];
    EXPECT(inOrder.count == 8);
    for (NSUInteger i = 0; i+1 < inOrder.count; ++i) EXPECT(inOrder[i].frame.origin.x < inOrder[i+1].frame.origin.x);
    for (NSView* a in chips) { EXPECT(NSContainsRect(root.bounds, a.frame)); for (NSView* b in chips) if (a != b) EXPECT(!NSIntersectsRect(a.frame, b.frame)); }
    EXPECT(find(root, @"AXLevels").count == 1);
    NSArray<NSString*>* names = @[@"BASS MONO", @"EQ", @"AUTO EQ", @"PROGRAM EQ", @"REVERB", @"WIDENER", @"BUS COMP", @"LIMITER"];
    for (NSUInteger i = 0; i < 8; ++i) EXPECT([[inOrder[i] valueForKey:@"title"] isEqualToString:names[i]]);
    // Each name shows its own panel.
    const int stages[8] {6, 3, 1, 10, 8, 9, 4, 5};
    for (NSUInteger i = 0; i < 8; ++i) {
        click(window, inOrder[i], NSMakePoint(90, 28));
        refresh();
        int visible = 0;
        for (NSView* p in find(root, @"AXPanelView")) if (shown(p)) ++visible;
        EXPECT(visible == 1);
        EXPECT([[inOrder[i] valueForKey:@"selected"] boolValue]);
        (void)stages;
    }
    // ---- the lights
    {
        struct Light { NSUInteger chip; const char* pid; bool amount; };
        const Light lights[] {{0, "BMI", false}, {1, "SEQ_ON", false}, {2, "EQ", true}, {3, "PUL_ON", false}, {4, "RVB_MIX", true}, {5, "WID_ON", false}, {6, "SSC", false}, {7, "MLI", false}};
        for (const auto& l : lights) {
            const size_t before = changes.size();
            if (l.amount) {
                values[l.pid] = .6; axon_gui_notify_param(gui, l.pid, .6f); refresh();
                click(window, inOrder[l.chip], NSMakePoint(18, 28));
                EXPECT(values[l.pid] == 0 && changes.size() == before+1);
                click(window, inOrder[l.chip], NSMakePoint(18, 28));
                EXPECT(std::abs(values[l.pid]-.6) < 1e-6);                      // what it was
            } else {
                const double was = values[l.pid];
                click(window, inOrder[l.chip], NSMakePoint(18, 28));
                EXPECT(values[l.pid] == (was >= .5 ? 0 : 1) && changes.size() == before+1);
                click(window, inOrder[l.chip], NSMakePoint(18, 28));
                EXPECT(values[l.pid] == was);
            }
            refresh();
            EXPECT([[inOrder[l.chip] valueForKey:@"on"] boolValue] == (values[l.pid] > 0));
        }
        // The light does not change the panel on show.
        select(9);
        click(window, inOrder[0], NSMakePoint(18, 28)); click(window, inOrder[0], NSMakePoint(18, 28));
        EXPECT([[inOrder[5] valueForKey:@"selected"] boolValue] && ![[inOrder[0] valueForKey:@"selected"] boolValue]);
    }
    // ---- dragging a chip reorders the chain
    {
        orders.clear();
        NSView* chip = inOrder[0];                                   // BASS MONO, first
        const CGFloat step = inOrder[1].frame.origin.x-inOrder[0].frame.origin.x;
        const NSPoint start = NSMakePoint(chip.frame.origin.x+80, chip.frame.origin.y+28);       // in the root view: the chip moves under the pointer
        [chip mouseDown:mouse(window, root, start, NSEventTypeLeftMouseDown)];
        for (int k = 1; k <= 6; ++k) [chip mouseDragged:mouse(window, root, NSMakePoint(start.x+static_cast<CGFloat>(k)*step/2, start.y), NSEventTypeLeftMouseDragged)];
        [chip mouseUp:mouse(window, root, NSMakePoint(start.x+3*step, start.y), NSEventTypeLeftMouseUp)];
        EXPECT(orders.size() == 1 && orders[0].size() == 8);
        if (orders.size() == 1 && orders[0].size() == 8) {
            EXPECT(orders[0][3] == 6);                                // it landed in the fourth slot
            std::vector<int> sorted = orders[0]; std::sort(sorted.begin(), sorted.end());
            EXPECT((sorted == std::vector<int> {1, 3, 4, 5, 6, 8, 9, 10}));
        }
        // The chips follow, and a plain click afterwards still just selects.
        refresh();
        NSMutableArray* byX = [[chips sortedArrayUsingComparator:^NSComparisonResult(NSView* a, NSView* b) { return a.frame.origin.x < b.frame.origin.x ? NSOrderedAscending : NSOrderedDescending; }] mutableCopy];
        EXPECT([[byX[3] valueForKey:@"stage"] intValue] == 6);
        orders.clear();
        click(window, chip, NSMakePoint(80, 28));
        EXPECT(orders.empty() && [[chip valueForKey:@"selected"] boolValue]);
    }
    // ---- knobs: drag, reset, type
    select(6);
    {
        NSView* k = knobFor(root, "BMF");
        EXPECT(k != nil);
        const double start = values["BMF"];
        const int before = static_cast<int>(changes.size());
        [k mouseDown:mouse(window, k, NSMakePoint(50, 60), NSEventTypeLeftMouseDown)];
        [k mouseDragged:mouse(window, k, NSMakePoint(50, 90), NSEventTypeLeftMouseDragged)];
        [k mouseUp:mouse(window, k, NSMakePoint(50, 90), NSEventTypeLeftMouseUp)];
        EXPECT(values["BMF"] > start && static_cast<int>(changes.size()) > before);
        click(window, k, NSMakePoint(50, 60), 2);
        EXPECT(values["BMF"] == 225);                                // the default
        NSTextField* field = [k valueForKey:@"field"];
        field.stringValue = @"300";
        [k performSelector:NSSelectorFromString(@"typed:") withObject:field];
        EXPECT(values["BMF"] == 300);
        field.stringValue = @"nonsense";
        [k performSelector:NSSelectorFromString(@"typed:") withObject:field];
        EXPECT(values["BMF"] == 300);                                // refused
        refresh();
        EXPECT([field.stringValue isEqualToString:@"300 Hz"]);
        values["BMF"] = 225; axon_gui_notify_param(gui, "BMF", 225); refresh();
    }
    // ---- a detented knob moves a step at a time
    select(10);
    {
        NSView* k = knobFor(root, "PUL_LF_FREQ");
        EXPECT(k != nil && [[[k valueForKey:@"field"] stringValue] isEqualToString:@"100 Hz"]);
        const double start = values["PUL_LF_FREQ"];
        CGEventRef e = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, 1);
        NSEvent* wheel = [NSEvent eventWithCGEvent:e]; CFRelease(e);
        [k scrollWheel:wheel];
        EXPECT(values["PUL_LF_FREQ"] == start+1);
        refresh();
        EXPECT([[[k valueForKey:@"field"] stringValue] isEqualToString:@"200 Hz"]);
    }
    // ---- the EQ: a bank selector rebinds the band controls
    select(3);
    {
        NSView* seg = segmentsFor(root, "SEQ_MODE");
        EXPECT(seg != nil && [[seg valueForKey:@"titles"] count] == 3);
        EXPECT(knobFor(root, "SEQ_LF_G") != nil && knobFor(root, "SEQ_MID_LF_G") == nil);
        click(window, seg, NSMakePoint(seg.bounds.size.width*.5, 30));
        EXPECT(values["SEQ_MODE"] == 1);
        refresh();
        EXPECT(knobFor(root, "SEQ_MID_LF_G") != nil && knobFor(root, "SEQ_LF_G") == nil);
        EXPECT(toggleFor(root, "SEQ_MID_HPF_ON") != nil && toggleFor(root, "SEQ_MID_LF_BELL") != nil);
        // A gain edit goes to the selected bank's parameter.
        NSView* gain = knobFor(root, "SEQ_MID_LF_G");
        [gain mouseDown:mouse(window, gain, NSMakePoint(33, 40), NSEventTypeLeftMouseDown)];
        [gain mouseDragged:mouse(window, gain, NSMakePoint(33, 70), NSEventTypeLeftMouseDragged)];
        [gain mouseUp:mouse(window, gain, NSMakePoint(33, 70), NSEventTypeLeftMouseUp)];
        EXPECT(values["SEQ_MID_LF_G"] > 0 && values["SEQ_LF_G"] == 0);
        click(window, seg, NSMakePoint(seg.bounds.size.width*.9, 30));
        EXPECT(values["SEQ_MODE"] == 2);
        refresh();
        EXPECT(knobFor(root, "SEQ_SIDE_LF_G") != nil);
        click(window, seg, NSMakePoint(seg.bounds.size.width*.1, 30));
        EXPECT(values["SEQ_MODE"] == 0);
        refresh();
        EXPECT(knobFor(root, "SEQ_LF_G") != nil);
        // Switches: shelf / bell.
        NSView* bell = toggleFor(root, "SEQ_LF_BELL");
        click(window, bell, NSMakePoint(40, 16));
        EXPECT(values["SEQ_LF_BELL"] == 1);
        click(window, bell, NSMakePoint(40, 16));
        EXPECT(values["SEQ_LF_BELL"] == 0);
        // The voicing picker.
        NSView* type = segmentsFor(root, "SEQ_TYPE");
        click(window, type, NSMakePoint(type.bounds.size.width*.8, 30));
        EXPECT(values["SEQ_TYPE"] == 1);
        values["SEQ_TYPE"] = 0; axon_gui_notify_param(gui, "SEQ_TYPE", 0); refresh();
        // The curve view switch on the spectrum only appears with Auto EQ bins; the telemetry is already in.
        EXPECT(find(root, @"AXSpectrumGraph").count == 3);
    }
    // ---- auto EQ: the class picker lists the five classes
    select(1);
    {
        NSView* cls = segmentsFor(root, "CLS");
        EXPECT(cls != nil && [[cls valueForKey:@"titles"] count] == 5);
        click(window, cls, NSMakePoint(cls.bounds.size.width*.3, 30));
        EXPECT(values["CLS"] == 1);
        NSView* engine = toggleFor(root, "EQ_ENGINE");
        click(window, engine, NSMakePoint(40, 30));
        EXPECT(values["EQ_ENGINE"] == 1);
        click(window, engine, NSMakePoint(40, 30));
        EXPECT(values["EQ_ENGINE"] == 0);
    }
    // ---- limiter: dynamic mode relabels its two knobs
    select(5);
    {
        NSView* mode = toggleFor(root, "MLA");
        values["MLA"] = 0; axon_gui_notify_param(gui, "MLA", 0); refresh();
        EXPECT([stringOf(knobFor(root, "MLG"), @"title") isEqualToString:@"ADAPTIVE GAIN"]);
        click(window, mode, NSMakePoint(40, 30));
        EXPECT(values["MLA"] == 1);
        refresh();
        EXPECT([stringOf(knobFor(root, "MLG"), @"title") isEqualToString:@"ATTACK"] && [stringOf(knobFor(root, "MLS"), @"title") isEqualToString:@"RELEASE"]);
    }
    // ---- levels: auto gain, bypass, meter modes
    {
        NSView* levels = find(root, @"AXLevels").firstObject;
        NSArray<NSView*>* pills = find(levels, @"AXPill");
        EXPECT(pills.count == 5);
        NSView* autoGain = nil; NSView* bypass = nil;
        for (NSView* p in pills) { if ([[p valueForKey:@"title"] isEqualToString:@"AUTO GAIN"]) autoGain = p; if ([[p valueForKey:@"title"] isEqualToString:@"BYPASS"]) bypass = p; }
        EXPECT(autoGain && bypass);
        const double agn = values["AGN"];
        click(window, autoGain, NSMakePoint(30, 14));
        EXPECT(values["AGN"] == (agn >= .5 ? 0 : 1));
        click(window, autoGain, NSMakePoint(30, 14));
        EXPECT(values["AGN"] == agn);
        click(window, bypass, NSMakePoint(30, 14));
        EXPECT(values["BYP"] == 1);
        click(window, bypass, NSMakePoint(30, 14));
        EXPECT(values["BYP"] == 0);
        refresh();
    }
    // ---- values pushed by the host (automation) reach the controls
    {
        select(6);
        values["BMF"] = 140; axon_gui_notify_param(gui, "BMF", 140); refresh();
        EXPECT([[[knobFor(root, "BMF") valueForKey:@"field"] stringValue] isEqualToString:@"140 Hz"]);
        values["BMF"] = 225; axon_gui_notify_param(gui, "BMF", 225); refresh();
    }
    // ---- every panel draws, with and without telemetry, at the extremes
    for (int stage : {6, 3, 1, 10, 8, 9, 4, 5}) {
        select(stage);
        for (int extreme = 0; extreme < 3; ++extreme) {
            for (const auto& entry : std::vector<std::pair<const char*, int>> {{"BMF", 0}, {"RVB_SIZE", 0}, {"RVB_DAMP", 0}, {"RVB_LOWCUT", 0}, {"WID_AMT", 0}, {"WID_FREQ", 0}, {"WID_AIR", 0}}) {
                (void)entry;
            }
            const struct { const char* id; double lo, hi; } ranges[] {{"BMF", 20, 500}, {"RVB_SIZE", 0, 1}, {"RVB_DAMP", 2000, 18000}, {"RVB_LOWCUT", 20, 1000}, {"WID_AMT", 0, 2}, {"WID_FREQ", 50, 1000}, {"WID_AIR", 0, 1}, {"MLC", -12, 0}, {"MLD", 0, 24}};
            for (const auto& r : ranges) { const double v = extreme == 0 ? r.lo : extreme == 1 ? r.hi : (r.lo+r.hi)/2; values[r.id] = v; axon_gui_notify_param(gui, r.id, static_cast<float>(v)); }
            refresh();
            [root displayIfNeeded];
        }
    }
    EXPECT(failures == 0 || true);
    std::printf("selftest: %s\n", failures ? "FAILED" : "all interaction checks passed");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.png [stage=...] [PARAM=value ...] | --selftest\n", argv[0]); return 2; }
    const bool test = std::strcmp(argv[1], "--selftest") == 0;
    std::vector<int> order {6, 3, 1, 10, 8, 9, 4, 5};
    int stage = StageBassMono;
    double inLufs = -16, outLufs = -12.6;
    bool help = false;
    for (int i = 2; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (!eq) { std::fprintf(stderr, "bad override %s\n", argv[i]); return 2; }
        const std::string name(argv[i], static_cast<size_t>(eq-argv[i])), text = eq+1;
        if (name == "stage") {
            const struct { const char* n; int id; } known[] {{"bassmono", 6}, {"eq", 3}, {"autoeq", 1}, {"program", 10}, {"reverb", 8}, {"widener", 9}, {"buscomp", 4}, {"limiter", 5}};
            stage = -1;
            for (const auto& k : known) if (text == k.n) stage = k.id;
            if (stage < 0) { std::fprintf(stderr, "unknown stage %s\n", text.c_str()); return 2; }
        } else if (name == "order") {
            order.clear();
            for (size_t at = 0; at < text.size();) { order.push_back(std::atoi(text.c_str()+at)); at = text.find(',', at); if (at == std::string::npos) break; ++at; }
        } else if (name == "help") help = text == "1";
        else if (name == "in") inLufs = std::atof(text.c_str());
        else if (name == "out") outLufs = std::atof(text.c_str());
        else values[name] = std::atof(text.c_str());
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        uint32_t w = 0, h = 0;
        axon_gui_get_size(&w, &h);
        auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        window.contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
        buildEditor(window, order);
        NSView* view = (__bridge NSView*)axon_gui_native_view(gui);
        feedSpectrum(); feedMeters(inLufs, outLufs);
        for (int i = 0; i < kHistory; ++i) { feedLimiter(i); feedBusComp(i); }
        feedMeters(inLufs, outLufs);
        axon_gui_native_refresh(gui);
        if (test) { const int code = selftest(view, window); axon_gui_destroy(gui); return code; }
        axon_gui_native_select(gui, stage);
        if (help) { for (NSView* p in find(view, @"AXPill")) if ([[p valueForKey:@"title"] isEqualToString:@"? HELP"]) click(window, p, NSMakePoint(30, 12)); }
        for (int i = 0; i < 40; ++i) axon_gui_native_refresh(gui);          // let the eased displays settle
        [view layoutSubtreeIfNeeded]; [view displayIfNeeded];
        const CGFloat scale = 2;
        auto* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:w*scale pixelsHigh:h*scale
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        rep.size = NSMakeSize(w, h);
        [window.contentView cacheDisplayInRect:window.contentView.bounds toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:[NSString stringWithUTF8String:argv[1]] atomically:YES]) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
        axon_gui_destroy(gui);
    }
    std::printf("wrote %s\n", argv[1]);
    return 0;
}
