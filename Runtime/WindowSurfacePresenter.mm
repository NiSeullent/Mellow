// SPDX-License-Identifier: MIT
#define GL_SILENCE_DEPRECATION
#import "WindowSurfacePresenter.h"
#import "NativeMetalRender.h"
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/gl3.h>
#include "PresenterPixels.hpp"
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {
NSString *const ErrorDomain = @"MellowWindowSurfacePresenter";
NSError *failure(NSString *message) {
    return [NSError errorWithDomain:ErrorDomain code:1
                          userInfo:@{NSLocalizedDescriptionKey: message}];
}
struct SurfaceFrame {
    IOSurfaceRef surface {};
    uint64_t sequence {};
    size_t width {}, height {}, pitch {};
    ~SurfaceFrame() { if (surface) CFRelease(surface); }
};
struct PresenterState {
    std::mutex mutex;
    std::shared_ptr<SurfaceFrame> queued;
    uint64_t drawnSequence {}, drawCount {}, completedFences {};
    NSInteger windowNumber {};
    bool attached {};
    std::string vendor, renderer, version;
    __strong NSError *error = nil;
};
struct GLResources {
    GLuint program {}, vao {}, texture {};
    GLsync fence {};
    std::shared_ptr<SurfaceFrame> pending;
};
void setFailure(const std::shared_ptr<PresenterState> &state, NSString *message) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->error = failure(message);
}
class CurrentContext {
    CGLContextObj context_ {}, previous_ {};
    bool locked_ {}, current_ {};
public:
    explicit CurrentContext(CGLContextObj context) : context_(context) {
        if (context_ && CGLLockContext(context_) == kCGLNoError) {
            locked_ = true;
            previous_ = CGLGetCurrentContext();
            current_ = CGLSetCurrentContext(context_) == kCGLNoError;
        }
    }
    ~CurrentContext() {
        if (current_) CGLSetCurrentContext(previous_);
        if (locked_) CGLUnlockContext(context_);
    }
    explicit operator bool() const { return current_; }
};
GLuint compileShader(GLenum stage, const char *source, std::string &error) {
    GLuint shader = glCreateShader(stage);
    if (!shader) { error = "glCreateShader failed"; return 0; }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled {};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096] {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = std::string("Presentation shader compilation failed: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
bool createResources(GLResources &resources, std::string &error) {
    const char *vertex =
        "#version 410 core\n"
        "out vec2 uv;\n"
        "void main() {\n"
        " vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
        " gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n"
        " uv=vec2(p.x,1.0-p.y);\n"
        "}\n";
    const char *fragment =
        "#version 410 core\n"
        "uniform sampler2DRect surfaceTexture; uniform vec2 surfaceSize;\n"
        "in vec2 uv; out vec4 color;\n"
        "void main() { color=texture(surfaceTexture,uv*surfaceSize); }\n";
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertex, error);
    if (!vs) return false;
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment, error);
    if (!fs) { glDeleteShader(vs); return false; }
    resources.program = glCreateProgram();
    if (resources.program) {
        glAttachShader(resources.program, vs);
        glAttachShader(resources.program, fs);
        glLinkProgram(resources.program);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked {};
    if (resources.program) glGetProgramiv(resources.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096] {};
        if (resources.program) glGetProgramInfoLog(resources.program, sizeof(log), nullptr, log);
        error = std::string("Presentation shader link failed: ") + log;
        return false;
    }
    glGenVertexArrays(1, &resources.vao);
    if (!resources.vao || glGetError() != GL_NO_ERROR) {
        error = "Presentation VAO creation failed";
        return false;
    }
    return true;
}
void deleteResources(GLResources &resources) {
    if (resources.fence) glDeleteSync(resources.fence);
    if (resources.texture) glDeleteTextures(1, &resources.texture);
    if (resources.vao) glDeleteVertexArrays(1, &resources.vao);
    if (resources.program) glDeleteProgram(resources.program);
    resources.fence = nullptr;
    resources.texture = resources.vao = resources.program = 0;
    // Called only at context retirement or after a signaled consumption fence.
    resources.pending.reset();
}
NSString *asNSString(const std::string &value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
}

@interface MellowSurfaceLayer : CAOpenGLLayer {
@public
    std::shared_ptr<PresenterState> _state;
@private
    std::mutex _resourcesMutex;
    std::map<CGLContextObj, GLResources> _resources;
}
@end

@implementation MellowSurfaceLayer
- (instancetype)initWithLayer:(id)layer {
    self = [super initWithLayer:layer];
    if (self && [layer isKindOfClass:[MellowSurfaceLayer class]])
        _state = ((MellowSurfaceLayer *)layer)->_state;
    return self;
}
- (CGLPixelFormatObj)copyCGLPixelFormatForDisplayMask:(uint32_t)mask {
    const CGLPixelFormatAttribute attributes[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAColorSize, static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAAlphaSize, static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFADisplayMask, static_cast<CGLPixelFormatAttribute>(mask),
        static_cast<CGLPixelFormatAttribute>(0)
    };
    CGLPixelFormatObj format {};
    GLint count {};
    CGLError code = CGLChoosePixelFormat(attributes, &format, &count);
    if (code != kCGLNoError || !format || count <= 0) {
        if (format) CGLDestroyPixelFormat(format);
        if (_state) setFailure(_state, @"No accelerated Core OpenGL pixel format for the layer");
        return nullptr;
    }
    return format;
}
- (CGLContextObj)copyCGLContextForPixelFormat:(CGLPixelFormatObj)format {
    CGLContextObj context {};
    GLint screen {}, accelerated {}, noRecovery {};
    if (!format || CGLCreateContext(format, nullptr, &context) != kCGLNoError || !context ||
        CGLGetVirtualScreen(context, &screen) != kCGLNoError ||
        CGLDescribePixelFormat(format, screen, kCGLPFAAccelerated, &accelerated) != kCGLNoError ||
        CGLDescribePixelFormat(format, screen, kCGLPFANoRecovery, &noRecovery) != kCGLNoError ||
        !accelerated || !noRecovery) {
        if (context) CGLDestroyContext(context);
        if (_state) setFailure(_state, @"Layer requires an accelerated CGL context without software fallback");
        return nullptr;
    }
    return context;
}
- (BOOL)canDrawInCGLContext:(CGLContextObj)context pixelFormat:(CGLPixelFormatObj)format
              forLayerTime:(CFTimeInterval)time displayTime:(const CVTimeStamp *)stamp {
    (void)context; (void)format; (void)time; (void)stamp;
    if (!_state) return NO;
    std::lock_guard<std::mutex> lock(_state->mutex);
    return _state->attached && _state->queued && !_state->error;
}
- (void)drawInCGLContext:(CGLContextObj)context pixelFormat:(CGLPixelFormatObj)format
           forLayerTime:(CFTimeInterval)time displayTime:(const CVTimeStamp *)stamp {
    if (!_state) return;
    std::shared_ptr<SurfaceFrame> frame;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (!_state->attached || _state->error) return;
        frame = _state->queued;
    }
    if (!frame) return;
    std::lock_guard<std::mutex> resourcesLock(_resourcesMutex);
    CurrentContext current(context);
    if (!current) { setFailure(_state, @"Could not acquire the layer CGL context"); return; }
    auto &resources = _resources[context];
    if (resources.pending) { setFailure(_state, @"A previous layer consumption fence remains unresolved"); return; }
    if (!resources.program) {
        std::string error;
        if (!createResources(resources, error)) {
            setFailure(_state, asNSString(error));
            return;
        }
        std::lock_guard<std::mutex> lock(_state->mutex);
        const auto *vendor = glGetString(GL_VENDOR);
        const auto *renderer = glGetString(GL_RENDERER);
        const auto *version = glGetString(GL_VERSION);
        _state->vendor = vendor ? reinterpret_cast<const char *>(vendor) : "";
        _state->renderer = renderer ? reinterpret_cast<const char *>(renderer) : "";
        _state->version = version ? reinterpret_cast<const char *>(version) : "";
    }
    // CAOpenGLLayer owns the destination framebuffer and viewport. Establish
    // every other GL state used here; this context belongs only to this layer.
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_RASTERIZER_DISCARD); glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_COVERAGE); glDisable(GL_SAMPLE_MASK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(resources.program);
    glBindVertexArray(resources.vao);
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &resources.texture);
    glBindTexture(GL_TEXTURE_RECTANGLE, resources.texture);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    resources.pending = frame;
    CGLError bind = CGLTexImageIOSurface2D(context, GL_TEXTURE_RECTANGLE, GL_RGBA8,
        static_cast<GLsizei>(frame->width), static_cast<GLsizei>(frame->height),
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frame->surface, 0);
    if (bind != kCGLNoError || glGetError() != GL_NO_ERROR) {
        setFailure(_state, @"IOSurface texture binding failed");
        return; // Keep surface and texture until this context is retired.
    }
    glUniform1i(glGetUniformLocation(resources.program, "surfaceTexture"), 0);
    glUniform2f(glGetUniformLocation(resources.program, "surfaceSize"),
                static_cast<GLfloat>(frame->width), static_cast<GLfloat>(frame->height));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    resources.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!resources.fence || glGetError() != GL_NO_ERROR) {
        setFailure(_state, @"Layer GPU draw or fence creation failed");
        return;
    }
    glFlush();
    GLenum wait = glClientWaitSync(resources.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ULL);
    if ((wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED) || glGetError() != GL_NO_ERROR) {
        setFailure(_state, @"Layer GPU consumption fence failed or timed out");
        return; // Never claim completion or reuse the outstanding IOSurface.
    }
    glDeleteSync(resources.fence); resources.fence = nullptr;
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    glDeleteTextures(1, &resources.texture); resources.texture = 0;
    resources.pending.reset();
    glBindVertexArray(0); glUseProgram(0);
    [super drawInCGLContext:context pixelFormat:format forLayerTime:time displayTime:stamp];
    if (glGetError() != GL_NO_ERROR) {
        setFailure(_state, @"Core Animation layer finalization failed");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->drawnSequence = frame->sequence;
        ++_state->drawCount; ++_state->completedFences;
    }
}
- (void)releaseCGLContext:(CGLContextObj)context {
    std::unique_lock<std::mutex> lock(_resourcesMutex);
    auto it = _resources.find(context);
    std::shared_ptr<SurfaceFrame> heldUntilContextRetirement;
    if (it != _resources.end()) {
        heldUntilContextRetirement = it->second.pending;
        CurrentContext current(context);
        if (current) deleteResources(it->second);
        _resources.erase(it);
    }
    lock.unlock();
    [super releaseCGLContext:context];
    heldUntilContextRetirement.reset();
}
@end

@interface MellowWindowSurfacePresenter ()
- (void)refreshLayout;
@end

@implementation MellowWindowSurfacePresenter {
    __weak NSView *_view;
    MellowSurfaceLayer *_layer;
    std::shared_ptr<PresenterState> _state;
    id _resizeObserver;
    id _scaleObserver;
    BOOL _previousFrameNotifications;
}
- (instancetype)initWithView:(NSView *)view error:(NSError **)error {
    if (error) *error = nil;
    if (![NSThread isMainThread] || !view || view.layer || view.subviews.count) {
        if (error) *error = failure(@"Presenter requires a main-thread empty, unlayered NSView");
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _state = std::make_shared<PresenterState>();
    _state->attached = true;
    _state->windowNumber = view.window ? view.window.windowNumber : 0;
    _view = view;
    _layer = [MellowSurfaceLayer layer];
    _layer->_state = _state;
    _layer.asynchronous = NO;
    _layer.opaque = NO;
    _layer.frame = view.bounds;
    _layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    _layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
    if (!colorSpace) {
        if (error) *error = failure(@"Could not create the linear sRGB layer color space");
        return nil;
    }
    _layer.colorspace = colorSpace;
    CFRelease(colorSpace);
    // Apple's NSView layer-hosting order is setLayer: then setWantsLayer:YES.
    view.layer = _layer;
    view.wantsLayer = YES;
    _previousFrameNotifications = view.postsFrameChangedNotifications;
    view.postsFrameChangedNotifications = YES;
    __weak MellowWindowSurfacePresenter *weakSelf = self;
    _resizeObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
        object:view queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *notification) {
            (void)notification;
            MellowWindowSurfacePresenter *presenter = weakSelf;
            if (presenter) [presenter refreshLayout];
        }];
    _scaleObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeBackingPropertiesNotification
        object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *notification) {
            MellowWindowSurfacePresenter *presenter = weakSelf;
            if (presenter && presenter->_view.window == notification.object) [presenter refreshLayout];
        }];
    return self;
}
- (void)refreshLayout {
    NSView *view = _view;
    if (!view || view.layer != _layer) return;
    _layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
    _layer.frame = view.bounds;
    [_layer setNeedsDisplay];
}
- (BOOL)enqueueCompletedTexture:(id<MTLTexture>)texture error:(NSError **)error {
    if (error) *error = nil;
    if (![NSThread isMainThread]) {
        if (error) *error = failure(@"Layer queue updates require the main thread");
        return NO;
    }
    NSView *view = _view;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (!_state->attached || !view || view.layer != _layer || _state->error) {
            if (error) *error = _state->error ?: failure(@"Presenter is detached or the host layer changed");
            return NO;
        }
    }
    NSUInteger width {}, height {};
    uint64_t sequence {};
    NSData *pixels = MellowCopyNativeRenderTextureRGBA8(texture, &width, &height, &sequence, error);
    if (!pixels) return NO;
    if (!sequence || !width || !height || width > 2048 || height > 2048 ||
        pixels.length != width * height * 4) {
        if (error) *error = failure(@"Completed texture snapshot has an invalid sequence or dimensions");
        return NO;
    }
    const size_t pitch = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4);
    if (pitch < width * 4 || pitch > SIZE_MAX / height) {
        if (error) *error = failure(@"IOSurface row alignment overflow");
        return NO;
    }
    const size_t allocation = IOSurfaceAlignProperty(kIOSurfaceAllocSize, pitch * height);
    if (allocation < pitch * height) {
        if (error) *error = failure(@"IOSurface allocation alignment overflow");
        return NO;
    }
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @(width),
        (__bridge NSString *)kIOSurfaceHeight: @(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(pitch),
        (__bridge NSString *)kIOSurfaceAllocSize: @(allocation),
        (__bridge NSString *)kIOSurfacePixelFormat: @(0x42475241u)
    };
    auto frame = std::make_shared<SurfaceFrame>();
    frame->surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!frame->surface || IOSurfaceGetPlaneCount(frame->surface) != 0 ||
        IOSurfaceGetWidth(frame->surface) != width || IOSurfaceGetHeight(frame->surface) != height ||
        IOSurfaceGetBytesPerRow(frame->surface) < width * 4 ||
        IOSurfaceGetPixelFormat(frame->surface) != 0x42475241u) {
        if (error) *error = failure(@"IOSurface creation returned an incompatible layout");
        return NO;
    }
    uint32_t seed {};
    if (IOSurfaceLock(frame->surface, 0, &seed) != kIOReturnSuccess) {
        if (error) *error = failure(@"IOSurface CPU write lock failed");
        return NO;
    }
    frame->pitch = IOSurfaceGetBytesPerRow(frame->surface);
    bool copied = MellowRT::copyPresenterPixels(static_cast<const uint8_t *>(pixels.bytes), pixels.length,
        width, height, static_cast<uint8_t *>(IOSurfaceGetBaseAddress(frame->surface)),
        IOSurfaceGetAllocSize(frame->surface), frame->pitch);
    IOReturn unlocked = IOSurfaceUnlock(frame->surface, 0, &seed);
    if (!copied || unlocked != kIOReturnSuccess) {
        if (error) *error = failure(@"IOSurface pixel copy or unlock failed");
        return NO;
    }
    frame->width = width; frame->height = height; frame->sequence = sequence;
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->queued = std::move(frame);
        _state->windowNumber = view.window ? view.window.windowNumber : 0;
    }
    [self refreshLayout];
    return YES;
}
- (void)detach {
    if (![NSThread isMainThread])
        [NSException raise:NSInternalInconsistencyException format:@"Presenter detach requires the main thread"];
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->attached = false;
        _state->queued.reset();
    }
    NSView *view = _view;
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_scaleObserver) [[NSNotificationCenter defaultCenter] removeObserver:_scaleObserver];
    _resizeObserver = _scaleObserver = nil;
    if (view && view.layer == _layer) {
        view.postsFrameChangedNotifications = _previousFrameNotifications;
        view.wantsLayer = NO; view.layer = nil;
    }
    _view = nil;
}
- (void)dealloc {
    // Do not mutate AppKit from an arbitrary ARC destruction thread. Callers
    // detach on main before releasing; destruction still stops new layer work.
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_scaleObserver) [[NSNotificationCenter defaultCenter] removeObserver:_scaleObserver];
    if (_state) {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->attached = false;
        _state->queued.reset();
    }
}
- (NSDictionary<NSString *,id> *)copyEvidence {
    std::lock_guard<std::mutex> lock(_state->mutex);
    return @{
        @"route": @"completed-CGL-render/readback/private-IOSurface/CAOpenGLLayer",
        @"attached": @(_state->attached), @"window_number": @(_state->windowNumber),
        @"queued_sequence": @(_state->queued ? _state->queued->sequence : 0),
        @"layer_drawn_sequence": @(_state->drawnSequence),
        @"layer_draw_count": @(_state->drawCount), @"consumption_fences": @(_state->completedFences),
        @"vendor": asNSString(_state->vendor), @"renderer": asNSString(_state->renderer),
        @"version": asNSString(_state->version),
        @"error": _state->error.localizedDescription ?: @"",
        @"pixel_layout": @"top-left premultiplied BGRA8, linear-sRGB",
        @"system_metal_registered": @NO, @"physical_pci_identity_verified": @NO,
        @"windowserver_accelerator_verified": @NO, @"display_scanout_verified": @NO
    };
}
@end
