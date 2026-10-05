// ax_widgets.mm: the editor's controls (knob, toggle, segmented enum, chip, pill).
#include "ax_ui.h"
#include <algorithm>
#include <cmath>

using namespace axui;

@implementation AXFlipped
- (BOOL)isFlipped { return YES; }
@end

// A borderless field draws its text from the top; this centres it vertically.
@interface AXCenteredCell : NSTextFieldCell
@end
@implementation AXCenteredCell
- (NSRect)drawingRectForBounds:(NSRect)rect {
    NSRect r = [super drawingRectForBounds:rect];
    const CGFloat height = [self cellSizeForBounds:rect].height;
    if (height < r.size.height) { r.origin.y += (r.size.height-height)/2; r.size.height = height; }
    return r;
}
- (NSRect)titleRectForBounds:(NSRect)rect { return [self drawingRectForBounds:rect]; }
@end

// ---------------------------------------------------------------- knob
@implementation AXKnob {
    double pos_, cont_, lastY_;
    BOOL interacting_;
}
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _field = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _field.cell = [[AXCenteredCell alloc] initTextCell:@""];
    _field.editable = YES; _field.selectable = YES;
    _field.textColor = uiColor(kText); _field.backgroundColor = NSColor.clearColor;
    _field.drawsBackground = NO; _field.bordered = NO; _field.bezeled = NO;
    _field.alignment = NSTextAlignmentCenter; _field.focusRingType = NSFocusRingTypeNone;
    _field.delegate = self; _field.target = self; _field.action = @selector(typed:);
    [self addSubview:_field];
    _tint = uiColor(kAccent);
    cont_ = -1;
    [self setSize:AXKnobBig];
    return self;
}
- (void)setSize:(AXKnobSize)size {
    _size = size;
    const NSRect b = self.bounds;
    switch (size) {
    case AXKnobBig: _field.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium]; _field.frame = NSMakeRect(4, 10, b.size.width-8, 20); break;
    case AXKnobCompact: _field.font = [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightMedium]; _field.frame = NSMakeRect(0, 2, b.size.width, 16); break;
    case AXKnobMini: _field.font = [NSFont monospacedDigitSystemFontOfSize:9.5 weight:NSFontWeightMedium]; _field.frame = NSMakeRect(0, -1, b.size.width, 14); break;
    }
    self.needsDisplay = YES;
}
- (void)setFrame:(NSRect)frame { [super setFrame:frame]; [self setSize:_size]; }
- (void)setTitle:(NSString*)title { if (![_title isEqualToString:title]) { _title = [title copy]; self.needsDisplay = YES; } }
- (void)setTint:(NSColor*)tint { if (![_tint isEqual:tint]) { _tint = tint; self.needsDisplay = YES; } }
- (void)setDimmed:(BOOL)dimmed { if (_dimmed != dimmed) { _dimmed = dimmed; self.alphaValue = dimmed ? .42 : 1; } }
- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setPid:(NSString*)pid { if (![_pid isEqualToString:pid]) { _pid = [pid copy]; cont_ = -1; } }
- (const Meta*)meta { return _pid ? [_host model]->find(_pid.UTF8String) : nullptr; }
- (double)normalize:(double)v {
    const Meta* m = [self meta]; if (!m || m->max == m->min) return 0;
    return std::clamp((v-m->min)/(m->max-m->min), 0., 1.);
}
- (double)snap:(double)v {
    const Meta* m = [self meta]; if (!m) return v;
    v = std::clamp(v, m->min, m->max);
    return _step > 0 ? m->min+std::round((v-m->min)/_step)*_step : v;
}
- (void)sync {
    if (!_pid || !_host) return;
    const double value = [_host valueFor:_pid];
    const double pos = [self normalize:value];
    if (!interacting_ && cont_ < 0) cont_ = pos;
    NSString* text = [_host textFor:_pid value:value];
    if (pos != pos_ || ![_field.stringValue isEqualToString:text]) {
        pos_ = pos;
        if (!_field.currentEditor) _field.stringValue = text;
        self.needsDisplay = YES;
    }
    _field.editable = _step == 0;
    NSString* how = _step > 0 ? @"Drag or scroll to choose; double-click resets." : @"Drag, scroll, or type a value. Shift-drag for fine control; double-click resets.";
    if (![self.toolTip isEqualToString:how]) self.toolTip = _field.toolTip = how;
    [_field setAccessibilityLabel:[NSString stringWithFormat:@"%@ value", _title]];
}
- (void)apply:(double)position {
    const Meta* m = [self meta]; if (!m) return;
    cont_ = std::clamp(position, 0., 1.);
    const double value = [self snap:m->min+cont_*(m->max-m->min)];
    if (value != [_host valueFor:_pid]) [_host setParam:_pid value:value];
    [self sync];
}
- (void)mouseDown:(NSEvent*)event {
    const Meta* m = [self meta]; if (!m) return;
    if (event.clickCount >= 2) { [_host setParam:_pid value:m->def]; cont_ = -1; [self sync]; return; }
    [self.window makeFirstResponder:nil];
    interacting_ = YES; lastY_ = event.locationInWindow.y;
    cont_ = [self normalize:[_host valueFor:_pid]];
}
- (void)mouseDragged:(NSEvent*)event {
    if (!interacting_) return;
    const double y = event.locationInWindow.y;
    const double perPixel = (event.modifierFlags & NSEventModifierFlagShift) ? 1./800 : 1./200;
    [self apply:cont_+(y-lastY_)*perPixel];
    lastY_ = y;
}
- (void)mouseUp:(NSEvent*)event { (void)event; interacting_ = NO; [self sync]; }
- (void)scrollWheel:(NSEvent*)event {
    const Meta* m = [self meta]; if (!m) return;
    const double delta = event.scrollingDeltaY*(event.hasPreciseScrollingDeltas ? .004 : .02);
    if (delta == 0) return;
    if (_step > 0) {
        const double current = [self snap:[_host valueFor:_pid]];
        const double next = [self snap:current+(delta > 0 ? _step : -_step)];
        if (next != current) { [_host setParam:_pid value:next]; [self sync]; }
        return;
    }
    cont_ = [self normalize:[_host valueFor:_pid]];
    [self apply:cont_+delta];
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilitySliderRole; }
- (NSString*)accessibilityLabel { return _title; }
- (id)accessibilityValue { return [_host textFor:_pid value:[_host valueFor:_pid]]; }
- (BOOL)accessibilityPerformIncrement { cont_ = pos_; [self apply:pos_+.05]; return YES; }
- (BOOL)accessibilityPerformDecrement { cont_ = pos_; [self apply:pos_-.05]; return YES; }
- (void)typed:(NSTextField*)sender {
    double value;
    if (_step == 0 && [_host parse:sender.stringValue for:_pid into:&value]) [_host setParam:_pid value:value];
    [self.window makeFirstResponder:nil]; [self sync];
}
- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if ([notification.userInfo[@"NSTextMovement"] integerValue] != NSReturnTextMovement) [self typed:_field];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    CGFloat radius, line, titleSize, titleKern, titleY, centerFromTop;
    switch (_size) {
    case AXKnobBig: radius = 29; line = 4; titleSize = 10; titleKern = 1.6; titleY = b.size.height-15; centerFromTop = 52; break;
    case AXKnobCompact: radius = 18; line = 3.2; titleSize = 8.5; titleKern = 1.2; titleY = b.size.height-13; centerFromTop = 38; break;
    default: radius = 16; line = 3; titleSize = 8; titleKern = 1; titleY = b.size.height-11; centerFromTop = 29; break;
    }
    uiDrawText(_title, NSMakeRect(0, titleY, b.size.width, 12), titleSize, uiColor(kTextDim), NSFontWeightSemibold, titleKern, NSTextAlignmentCenter);
    const NSPoint c = NSMakePoint(b.size.width/2, b.size.height-centerFromTop);
    auto* track = [NSBezierPath bezierPath];
    [track appendBezierPathWithArcWithCenter:c radius:radius startAngle:225 endAngle:-45 clockwise:YES];
    track.lineWidth = line; track.lineCapStyle = NSLineCapStyleRound;
    [uiColor(kBorder) setStroke]; [track stroke];
    // A parameter that runs either side of zero (a gain) grows its arc from the middle.
    const Meta* m = [self meta];
    const BOOL bipolar = m && m->min < 0 && m->max > 0;
    const double from = bipolar ? (0-m->min)/(m->max-m->min) : 0;
    if (std::abs(pos_-from) > .004) {
        auto* arc = [NSBezierPath bezierPath];
        [arc appendBezierPathWithArcWithCenter:c radius:radius startAngle:225-270*from endAngle:225-270*pos_ clockwise:pos_ > from];
        arc.lineWidth = line; arc.lineCapStyle = NSLineCapStyleRound;
        [NSGraphicsContext saveGraphicsState];
        auto* glow = [[NSShadow alloc] init];
        glow.shadowColor = [_tint colorWithAlphaComponent:.45]; glow.shadowBlurRadius = _size == AXKnobBig ? 8 : 5; glow.shadowOffset = NSZeroSize;
        [glow set]; [_tint setStroke]; [arc stroke];
        [NSGraphicsContext restoreGraphicsState];
    }
    const CGFloat body = radius-(_size == AXKnobBig ? 8 : _size == AXKnobCompact ? 5.5 : 5);
    auto* disc = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-body, c.y-body, 2*body, 2*body)];
    [[[NSGradient alloc] initWithStartingColor:uiColor(kSurfaceHover) endingColor:uiColor(kBgSunken)] drawInBezierPath:disc angle:-90];
    [uiColor(kBorderStrong) setStroke]; disc.lineWidth = 1; [disc stroke];
    const double angle = (225-270*pos_)*M_PI/180;
    const CGFloat inner = _size == AXKnobBig ? 5 : 3, outer = body-(_size == AXKnobBig ? 3 : 2);
    auto* tick = [NSBezierPath bezierPath];
    [tick moveToPoint:NSMakePoint(c.x+std::cos(angle)*inner, c.y+std::sin(angle)*inner)];
    [tick lineToPoint:NSMakePoint(c.x+std::cos(angle)*outer, c.y+std::sin(angle)*outer)];
    tick.lineWidth = _size == AXKnobBig ? 2.5 : 2; tick.lineCapStyle = NSLineCapStyleRound;
    [_tint setStroke]; [tick stroke];
}
@end

// ---------------------------------------------------------------- toggle
@implementation AXToggle
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setOn:(BOOL)on { if (_on != on) { _on = on; self.needsDisplay = YES; } }
- (void)setLabels:(NSArray<NSString*>*)labels { _labels = labels; self.needsDisplay = YES; }
- (void)setCaption:(NSString*)caption { if (![_caption isEqualToString:caption]) { _caption = [caption copy]; self.needsDisplay = YES; } }
- (void)sync { if (_pid && _host) self.on = [_host valueFor:_pid] >= .5; }
- (void)flip { [_host setParam:_pid value:_on ? 0 : 1]; self.on = !_on; }
- (void)mouseDown:(NSEvent*)event { (void)event; [self.window makeFirstResponder:nil]; [self flip]; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityCheckBoxRole; }
- (NSString*)accessibilityLabel { return _caption.length ? _caption : (_labels.count > 1 ? _labels[1] : @"switch"); }
- (id)accessibilityValue { return @(_on); }
- (BOOL)accessibilityPerformPress { [self flip]; return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    if (_caption.length) {
        uiDrawText(_caption, NSMakeRect(0, 0, b.size.width, 12), 8.5, uiColor(kTextDim), NSFontWeightSemibold, 1.2, NSTextAlignmentCenter);
        b.origin.y += 14; b.size.height -= 14;
    }
    const NSRect r = NSInsetRect(b, 1.5, 1.5);
    auto* path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:_big ? 10 : 7 yRadius:_big ? 10 : 7];
    [NSGraphicsContext saveGraphicsState];
    if (_on) { auto* glow = [[NSShadow alloc] init]; glow.shadowColor = [_tint colorWithAlphaComponent:.35]; glow.shadowBlurRadius = 8; glow.shadowOffset = NSZeroSize; [glow set]; }
    [(_on ? [_tint colorWithAlphaComponent:.14] : uiColor(kSurface)) setFill]; [path fill];
    [NSGraphicsContext restoreGraphicsState];
    [(_on ? _tint : uiColor(kBorder)) setStroke]; path.lineWidth = 1; [path stroke];
    NSString* text = _labels.count > 1 ? _labels[_on ? 1 : 0] : (_on ? @"ON" : @"OFF");
    const CGFloat size = _big ? 12 : 9.5;
    uiDrawText(text, NSMakeRect(r.origin.x, r.origin.y+(r.size.height-size-3)/2+.5, r.size.width, size+3), size, _on ? _tint : uiColor(kTextDim), NSFontWeightBold, 1.2, NSTextAlignmentCenter);
}
@end

// ---------------------------------------------------------------- segments
@implementation AXSegments
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setSelected:(NSInteger)selected { if (_selected != selected) { _selected = selected; self.needsDisplay = YES; } }
- (void)setTitles:(NSArray<NSString*>*)titles { _titles = titles; self.needsDisplay = YES; }
- (void)sync { if (_pid && _host) self.selected = static_cast<NSInteger>(std::lround([_host valueFor:_pid])); }
- (NSRect)rectFor:(NSInteger)i {
    NSRect b = self.bounds;
    if (_caption.length) { b.origin.y += 14; b.size.height -= 14; }
    const CGFloat w = b.size.width/static_cast<CGFloat>(std::max<NSUInteger>(1, _titles.count));
    return NSMakeRect(b.origin.x+w*static_cast<CGFloat>(i), b.origin.y, w, b.size.height);
}
- (void)mouseDown:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    [self.window makeFirstResponder:nil];
    for (NSInteger i = 0; i < static_cast<NSInteger>(_titles.count); ++i)
        if (NSPointInRect(p, [self rectFor:i])) {
            if (i != _selected) { [_host setParam:_pid value:static_cast<double>(i)]; self.selected = i; }
            return;
        }
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityRadioGroupRole; }
- (NSString*)accessibilityLabel { return _caption.length ? _caption : _pid; }
- (id)accessibilityValue { return _selected >= 0 && _selected < static_cast<NSInteger>(_titles.count) ? _titles[static_cast<NSUInteger>(_selected)] : @""; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (_caption.length) uiDrawText(_caption, NSMakeRect(0, 0, self.bounds.size.width, 12), 8.5, uiColor(kTextDim), NSFontWeightSemibold, 1.2, NSTextAlignmentLeft);
    for (NSInteger i = 0; i < static_cast<NSInteger>(_titles.count); ++i) {
        NSRect r = NSInsetRect([self rectFor:i], 1.5, 1);
        NSColor* tint = i < static_cast<NSInteger>(_colors.count) ? _colors[static_cast<NSUInteger>(i)] : _tint;
        const BOOL on = i == _selected;
        auto* path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6];
        [NSGraphicsContext saveGraphicsState];
        if (on) { auto* glow = [[NSShadow alloc] init]; glow.shadowColor = [tint colorWithAlphaComponent:.3]; glow.shadowBlurRadius = 7; glow.shadowOffset = NSZeroSize; [glow set]; }
        [(on ? [tint colorWithAlphaComponent:.14] : uiColor(kSurface)) setFill]; [path fill];
        [NSGraphicsContext restoreGraphicsState];
        [(on ? tint : uiColor(kBorder)) setStroke]; path.lineWidth = 1; [path stroke];
        uiDrawText(_titles[static_cast<NSUInteger>(i)], NSMakeRect(r.origin.x, r.origin.y+(r.size.height-12)/2+.5, r.size.width, 13), 9, on ? tint : uiColor(kTextDim), NSFontWeightBold, 1.1, NSTextAlignmentCenter);
    }
}
@end

// ---------------------------------------------------------------- chip
@implementation AXChip {
    BOOL dragging_, pressed_;
    CGFloat grabX_, startX_;
    NSPoint downAt_;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setOn:(BOOL)on { if (_on != on) { _on = on; self.needsDisplay = YES; } }
- (void)setSelected:(BOOL)selected { if (_selected != selected) { _selected = selected; self.needsDisplay = YES; } }
- (void)setSubtitle:(NSString*)subtitle { if (![_subtitle isEqualToString:subtitle]) { _subtitle = [subtitle copy]; self.needsDisplay = YES; } }
- (void)mouseDown:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    [self.window makeFirstResponder:nil];
    if (p.x < 34) { pressed_ = NO; if (_onToggle) _onToggle(); return; }
    pressed_ = YES; dragging_ = NO;
    downAt_ = [self.superview convertPoint:event.locationInWindow fromView:nil];
    startX_ = self.frame.origin.x;
}
- (void)mouseDragged:(NSEvent*)event {
    if (!pressed_) return;
    const NSPoint now = [self.superview convertPoint:event.locationInWindow fromView:nil];
    if (!dragging_) {
        if (std::abs(now.x-downAt_.x) < 6) return;
        dragging_ = YES;
        if (_onDragBegan) _onDragBegan(self);
    }
    if (_onDragMoved) _onDragMoved(self, startX_+(now.x-downAt_.x));
}
- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (!pressed_) return;
    pressed_ = NO;
    if (dragging_) { dragging_ = NO; if (_onDragEnded) _onDragEnded(self); return; }
    if (_onSelect) _onSelect();
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityButtonRole; }
- (NSString*)accessibilityLabel { return _title; }
- (id)accessibilityValue { return _on ? @"on" : @"off"; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    [NSGraphicsContext saveGraphicsState];
    if (_selected) { auto* glow = [[NSShadow alloc] init]; glow.shadowColor = [_tint colorWithAlphaComponent:.3]; glow.shadowBlurRadius = 10; glow.shadowOffset = NSZeroSize; [glow set]; }
    uiFillPanel(b, 11, _selected ? uiColor(kSurfaceRaised) : uiColor(kSurface), _selected ? _tint : uiColor(kBorder));
    [NSGraphicsContext restoreGraphicsState];
    const NSRect led = NSMakeRect(12, b.size.height/2-6, 12, 12);
    auto* dot = [NSBezierPath bezierPathWithOvalInRect:led];
    [NSGraphicsContext saveGraphicsState];
    if (_on) { auto* glow = [[NSShadow alloc] init]; glow.shadowColor = [_tint colorWithAlphaComponent:.8]; glow.shadowBlurRadius = 7; glow.shadowOffset = NSZeroSize; [glow set]; }
    [(_on ? _tint : uiColor(kBorderStrong)) setFill]; [dot fill];
    [NSGraphicsContext restoreGraphicsState];
    [uiColor(kBgSunken) setStroke]; dot.lineWidth = 1; [dot stroke];
    uiDrawText(_title, NSMakeRect(34, 12, b.size.width-38, 14), 9.5, _on ? uiColor(kText) : uiColor(kTextDim), NSFontWeightHeavy, 1, NSTextAlignmentLeft);
    uiDrawText(_subtitle ? _subtitle : @"", NSMakeRect(34, b.size.height-24, b.size.width-38, 11), 7.5, _on ? [_tint colorWithAlphaComponent:.85] : uiColor(kTextMute), NSFontWeightBold, .4, NSTextAlignmentLeft);
}
@end

// ---------------------------------------------------------------- pill
@implementation AXPill
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setOn:(BOOL)on { if (_on != on) { _on = on; self.needsDisplay = YES; } }
- (void)setTitle:(NSString*)title { if (![_title isEqualToString:title]) { _title = [title copy]; self.needsDisplay = YES; } }
- (void)mouseDown:(NSEvent*)event { (void)event; [self.window makeFirstResponder:nil]; if (_onClick) _onClick(); }
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return _sticky ? NSAccessibilityCheckBoxRole : NSAccessibilityButtonRole; }
- (NSString*)accessibilityLabel { return _title; }
- (id)accessibilityValue { return _sticky ? @(_on) : nil; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect r = NSInsetRect(self.bounds, 1, 1);
    NSColor* tint = _tint ? _tint : uiColor(kAccent);
    const BOOL lit = _on && _sticky;
    auto* path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:r.size.height/2 yRadius:r.size.height/2];
    [(lit ? [tint colorWithAlphaComponent:.14] : uiColor(kSurface)) setFill]; [path fill];
    [(lit ? tint : uiColor(kBorderStrong)) setStroke]; path.lineWidth = 1; [path stroke];
    uiDrawText(_title, NSMakeRect(r.origin.x, r.origin.y+(r.size.height-12)/2+.5, r.size.width, 13), 8.5, lit ? tint : uiColor(kTextDim), NSFontWeightBold, 1.2, NSTextAlignmentCenter);
}
@end
