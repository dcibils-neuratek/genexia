/*
 * psp_metal.m - draws the PSP's graphics on the Mac's GPU, at any resolution.
 * psp_ge.c still walks the lists and transforms the vertices; here each of the
 * PSP's fixed-function states becomes a pipeline, and each video memory address
 * drawn to becomes a render target several times the size of the original.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <stdio.h>
#include "psp_metal.h"

#define LOGICAL 512                  /* video memory's width, the unit vertices arrive in */
#define VERTS (1 << 21)
#define RINGS 3

static const char *shaders = R"(
#include <metal_stdlib>
using namespace metal;
struct Vert { float4 pos; float2 uv; float2 pad; float4 col; };
struct Out { float4 pos [[position]]; float2 uv; float4 col; };
struct U { float2 uv_scale; int tex, func, use_a, dbl; float4 env, fix; int atest, afunc, aref, amask, fix_src; };
vertex Out vert(uint i [[vertex_id]], const device Vert *v [[buffer(0)]]) {
    Out o; o.pos = v[i].pos; o.uv = v[i].uv; o.col = v[i].col; return o;
}
static bool compare(int f, int a, int b) {
    switch (f) { case 0: return false; case 1: return true; case 2: return a == b; case 3: return a != b;
                 case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b; }
}
fragment float4 frag(Out i [[stage_in]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    float4 c = i.col;
    if (u.tex) {
        const float4 x = t.sample(s, i.uv * u.uv_scale);
        switch (u.func) {
        case 0: c.rgb *= x.rgb; if (u.use_a) c.a *= x.a; break;
        case 1: c.rgb = u.use_a ? mix(c.rgb, x.rgb, x.a) : x.rgb; break;
        case 2: c.rgb = mix(c.rgb, u.env.rgb, x.rgb); if (u.use_a) c.a *= x.a; break;
        case 3: c.rgb = x.rgb; if (u.use_a) c.a = x.a; break;
        default: c.rgb += x.rgb; if (u.use_a) c.a *= x.a; break;
        }
        if (u.dbl) c.rgb *= 2;
        c.rgb = min(c.rgb, 1.0);
    }
    if (u.atest && !compare(u.afunc, int(c.a * 255 + 0.5) & u.amask, u.aref & u.amask)) discard_fragment();
    if (u.fix_src) c.rgb *= u.fix.rgb;
    return c;
}
struct Full { float4 pos [[position]]; float2 uv; };
vertex Full full(uint i [[vertex_id]], constant float2 &edge [[buffer(0)]]) {
    const float2 p = float2((i << 1) & 2, i & 2);
    Full o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y) * edge; return o;
}
fragment float4 show(Full i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, i.uv).rgb, 1);
}
)";

typedef struct { simd_float2 uv_scale; int tex, func, use_a, dbl; simd_float4 env, fix; int atest, afunc, aref, amask, fix_src; } Uniforms;
typedef struct { uint32_t addr; int drawn; id<MTLTexture> color, depth, smooth; } Target;

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLCommandBuffer> cb;
static id<MTLRenderCommandEncoder> enc;
static id<MTLBuffer> rings[RINGS];
static dispatch_semaphore_t in_flight;
static id<MTLLibrary> lib;
static id<MTLRenderPipelineState> present_pipe;
static id<MTLSamplerState> samplers[8];
static id<MTLTexture> blank;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipes;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *depths;
static CAMetalLayer *layer;
static void *window;
static Target targets[32];
static Target *current;
static int ntargets, ring, used, scale_by, samples = 1;

static Target *find(uint32_t addr) {
    for (int i = 0; i < ntargets; i++) if (targets[i].addr == addr) return &targets[i];
    return NULL;
}
int mtl_is_target(uint32_t addr) { return find(addr) != NULL; }
int mtl_drawn(uint32_t addr) { const Target *t = find(addr); return t && t->drawn; }

static Target *target_for(uint32_t addr) {
    Target *t = find(addr);
    if (t || ntargets == 32) return t ? t : &targets[0];
    t = &targets[ntargets++];
    t->addr = addr;
    const int size = LOGICAL * scale_by;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:size height:size mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    t->color = [dev newTextureWithDescriptor:d];
    d.usage = MTLTextureUsageRenderTarget;
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = (NSUInteger)samples;
        t->smooth = [dev newTextureWithDescriptor:d];
    }
    d.pixelFormat = MTLPixelFormatDepth32Float;
    t->depth = [dev newTextureWithDescriptor:d];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];     /* starts black */
    p.colorAttachments[0].texture = samples > 1 ? t->smooth : t->color;
    p.colorAttachments[0].resolveTexture = samples > 1 ? t->color : nil;
    p.colorAttachments[0].storeAction = samples > 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    p.depthAttachment.texture = t->depth;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.clearDepth = 0;
    if (enc) { [enc endEncoding]; enc = nil; current = NULL; }
    if (!cb) cb = [queue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    return t;
}

static void begin_on(Target *t) {
    if (enc && current == t) return;
    if (enc) [enc endEncoding];
    if (!cb) cb = [queue commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = samples > 1 ? t->smooth : t->color;
    p.colorAttachments[0].resolveTexture = samples > 1 ? t->color : nil;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = samples > 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
    p.depthAttachment.texture = t->depth;
    p.depthAttachment.loadAction = MTLLoadActionLoad;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    enc = [cb renderCommandEncoderWithDescriptor:p];
    [enc setCullMode:MTLCullModeNone];
    [enc setDepthClipMode:MTLDepthClipModeClamp];
    [enc setVertexBuffer:rings[ring] offset:0 atIndex:0];
    current = t;
}

static void finish_buffer(void) {
    if (enc) { [enc endEncoding]; enc = nil; current = NULL; }
    if (!cb) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { dispatch_semaphore_signal(in_flight); }];
    [cb commit];
    dispatch_semaphore_wait(in_flight, DISPATCH_TIME_FOREVER);
    cb = nil;
    ring = (ring + 1) % RINGS;
    used = 0;
}

static MTLBlendFactor factor(int f, int src) {
    switch (f) {
    case 0: return src ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceColor;
    case 1: return src ? MTLBlendFactorOneMinusDestinationColor : MTLBlendFactorOneMinusSourceColor;
    case 2: case 6: return MTLBlendFactorSourceAlpha;
    case 3: case 7: return MTLBlendFactorOneMinusSourceAlpha;
    case 4: case 8: return MTLBlendFactorDestinationAlpha;
    case 5: case 9: return MTLBlendFactorOneMinusDestinationAlpha;
    default: return src ? MTLBlendFactorOne : MTLBlendFactorBlendColor;    /* the shader applies a fixed source */
    }
}

static id<MTLRenderPipelineState> pipeline(const MtlState *s) {
    const unsigned src = s->src & 15, dst = s->dst & 15, eq = s->eq & 7, on = s->blend != 0, mask = s->write_mask & 15;
    NSNumber *key = @(src | dst << 4 | eq << 8 | on << 11 | mask << 12);
    id<MTLRenderPipelineState> p = pipes[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vert"];
    d.fragmentFunction = [lib newFunctionWithName:@"frag"];
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.rasterSampleCount = (NSUInteger)samples;
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatRGBA8Unorm;
    c.writeMask = (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) | (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    c.blendingEnabled = on;
    if (on) {
        static const MTLBlendOperation ops[8] = { MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationReverseSubtract, MTLBlendOperationMin, MTLBlendOperationMax, MTLBlendOperationAdd, MTLBlendOperationAdd, MTLBlendOperationAdd };
        c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = factor((int)src, 1);
        c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = factor((int)dst, 0);
        c.rgbBlendOperation = c.alphaBlendOperation = ops[eq];
    }
    NSError *e = nil;
    p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) { fprintf(stderr, "metal pipeline: %s\n", e.localizedDescription.UTF8String); exit(1); }
    pipes[key] = p;
    return p;
}

static id<MTLDepthStencilState> depth_state(const MtlState *s) {
    static const MTLCompareFunction fn[8] = { MTLCompareFunctionNever, MTLCompareFunctionAlways, MTLCompareFunctionEqual, MTLCompareFunctionNotEqual, MTLCompareFunctionLess, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionGreaterEqual };
    const int f = s->depth_test ? s->depth_func & 7 : 1, w = s->depth_write != 0;
    NSNumber *key = @(f | w << 3);
    id<MTLDepthStencilState> d = depths[key];
    if (d) return d;
    MTLDepthStencilDescriptor *q = [MTLDepthStencilDescriptor new];
    q.depthCompareFunction = fn[f];
    q.depthWriteEnabled = w;
    d = [dev newDepthStencilStateWithDescriptor:q];
    depths[key] = d;
    return d;
}

void mtl_texture(void **slot, int w, int h, int pitch, const uint32_t *rgba) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)*slot;
    if (!t || (int)t.width != w || (int)t.height != h) {
        if (*slot) CFRelease(*slot);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        t = [dev newTextureWithDescriptor:d];
        *slot = (void *)CFBridgingRetain(t);
    }
    [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:rgba bytesPerRow:(NSUInteger)pitch * 4];
}

void mtl_draw(uint32_t addr, const MtlState *s, const MtlVert *v, int count) {
    if (count <= 0) return;
    if (used + count > VERTS) finish_buffer();
    if (count > VERTS) return;
    Target *t = target_for(addr);
    begin_on(t);
    t->drawn = 1;
    memcpy((MtlVert *)rings[ring].contents + used, v, (size_t)count * sizeof *v);
    Uniforms u = { 0 };
    u.uv_scale = simd_make_float2(s->uv_scale[0], s->uv_scale[1]);
    id<MTLTexture> tex = blank;
    if (s->tex_target) { Target *r = find(s->tex_target); if (r && r != t) tex = r->color; }
    else if (s->tex) tex = (__bridge id<MTLTexture>)s->tex;
    u.tex = tex != blank;
    u.func = s->tfunc; u.use_a = s->use_alpha; u.dbl = s->doubled;
    u.env = simd_make_float4(s->env[0], s->env[1], s->env[2], 1);
    u.atest = s->alpha_test; u.afunc = s->alpha_func; u.aref = s->alpha_ref; u.amask = s->alpha_mask;
    u.fix_src = s->blend && s->src == 10;
    u.fix = simd_make_float4(s->fix_a[0], s->fix_a[1], s->fix_a[2], 1);
    [enc setRenderPipelineState:pipeline(s)];
    [enc setDepthStencilState:depth_state(s)];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentSamplerState:samplers[s->clamp_u | s->clamp_v << 1 | s->linear << 2] atIndex:0];
    [enc setBlendColorRed:s->fix_b[0] green:s->fix_b[1] blue:s->fix_b[2] alpha:1];
    const int x1 = s->sx1 * scale_by, y1 = s->sy1 * scale_by, x2 = (s->sx2 + 1) * scale_by, y2 = (s->sy2 + 1) * scale_by;
    const int top = LOGICAL * scale_by;
    if (x2 <= x1 || y2 <= y1) return;
    [enc setScissorRect:(MTLScissorRect){ (NSUInteger)x1, (NSUInteger)y1, (NSUInteger)((x2 > top ? top : x2) - x1), (NSUInteger)((y2 > top ? top : y2) - y1) }];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(NSUInteger)used vertexCount:(NSUInteger)count];
    used += count;
}

void mtl_present(uint32_t addr, const uint32_t *cpu_rgba) {
    @autoreleasepool {
        int w, h;                              /* the window may have changed size */
        SDL_Metal_GetDrawableSize(window, &w, &h);
        if (w != (int)layer.drawableSize.width || h != (int)layer.drawableSize.height) {
            layer.drawableSize = CGSizeMake(w, h);
#ifdef PSP_SIZE_LOG
            fprintf(stderr, "drawable %dx%d\n", w, h);
#endif
        }
        Target *t = target_for(addr);
        static id<MTLTexture> small;
        if (cpu_rgba) {                        /* drawn by the game's own code: a picture in memory */
            if (!small) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:480 height:272 mipmapped:NO];
                d.storageMode = MTLStorageModeShared;
                small = [dev newTextureWithDescriptor:d];
            }
            [small replaceRegion:MTLRegionMake2D(0, 0, 480, 272) mipmapLevel:0 withBytes:cpu_rgba bytesPerRow:480 * 4];
        }
        if (enc) { [enc endEncoding]; enc = nil; current = NULL; }
        if (!cb) cb = [queue commandBuffer];
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = drawable.texture;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
            const double dw = drawable.texture.width, dh = drawable.texture.height, k = fmin(dw / 480, dh / 272);
            [e setViewport:(MTLViewport){ (dw - 480 * k) / 2, (dh - 272 * k) / 2, 480 * k, 272 * k, 0, 1 }];
            [e setRenderPipelineState:present_pipe];
            const simd_float2 edge = cpu_rgba ? simd_make_float2(1, 1) : simd_make_float2(480.f / LOGICAL, 272.f / LOGICAL);
            [e setVertexBytes:&edge length:sizeof edge atIndex:0];
            [e setFragmentTexture:cpu_rgba ? small : t->color atIndex:0];
            [e setFragmentSamplerState:samplers[3 | 4] atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            [cb presentDrawable:drawable];
        }
        finish_buffer();
        for (int i = 0; i < ntargets; i++) targets[i].drawn = 0;
    }
}

void mtl_shot(uint32_t addr, const char *path) {
    Target *t = find(addr);
    if (!t) return;
    if (enc) { [enc endEncoding]; enc = nil; current = NULL; }
    if (!cb) cb = [queue commandBuffer];
    const int w = 480 * scale_by, h = 272 * scale_by;
    id<MTLBuffer> b = [dev newBufferWithLength:(NSUInteger)(w * h * 4) options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t->color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)(w * h * 4)];
    [blit endEncoding];
    id<MTLCommandBuffer> c = cb;
    finish_buffer();
    [c waitUntilCompleted];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint8_t *px = b.contents;
    for (int i = 0; i < w * h; i++) fwrite(px + i * 4, 1, 3, f);
    fclose(f);
}

void mtl_open(void *sdl_window, int scale, int msaa) {
    scale_by = scale;
    window = sdl_window;
    SDL_MetalView view = SDL_Metal_CreateView(sdl_window);
    layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
    dev = MTLCreateSystemDefaultDevice();
    layer.device = dev;
    for (samples = msaa < 1 ? 1 : msaa; samples > 1 && ![dev supportsTextureSampleCount:(NSUInteger)samples]; samples--) {}
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    int w, h;
    SDL_Metal_GetDrawableSize(sdl_window, &w, &h);
    layer.drawableSize = CGSizeMake(w, h);
    queue = [dev newCommandQueue];
    in_flight = dispatch_semaphore_create(RINGS - 1);
    for (int i = 0; i < RINGS; i++) rings[i] = [dev newBufferWithLength:VERTS * sizeof(MtlVert) options:MTLResourceStorageModeShared];
    NSError *e = nil;
    lib = [dev newLibraryWithSource:@(shaders) options:nil error:&e];
    if (!lib) { fprintf(stderr, "metal shaders: %s\n", e.localizedDescription.UTF8String); exit(1); }
    pipes = [NSMutableDictionary new];
    depths = [NSMutableDictionary new];
    for (int i = 0; i < 8; i++) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.sAddressMode = i & 1 ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        d.tAddressMode = i & 2 ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        d.minFilter = d.magFilter = i & 4 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        samplers[i] = [dev newSamplerStateWithDescriptor:d];
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    blank = [dev newTextureWithDescriptor:d];
    MTLRenderPipelineDescriptor *q = [MTLRenderPipelineDescriptor new];
    q.vertexFunction = [lib newFunctionWithName:@"full"];
    q.fragmentFunction = [lib newFunctionWithName:@"show"];
    q.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    present_pipe = [dev newRenderPipelineStateWithDescriptor:q error:&e];
    fprintf(stderr, "metal: %s, drawing at %dx%d, %dx anti-aliasing\n", dev.name.UTF8String, 480 * scale, 272 * scale, samples);
}
