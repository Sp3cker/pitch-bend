// pugl_mac_metal.m — Pugl Metal backend for macOS
// Adapted from poryaaaa/plugin/metal/pugl_mac_metal.m

#include "internal.h"
#include "mac.h"
#include "stub.h"
#include "pugl_mac_metal.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// --- PuglMetalView ---

@interface PuglMetalView : NSView
@end

@implementation PuglMetalView {
@public
  PuglView        *puglview;
  PuglMetalContext  metalCtx;
}

- (id)initWithFrame:(NSRect)frame
{
  self = [super initWithFrame:frame];
  if (self) {
    metalCtx.device = MTLCreateSystemDefaultDevice();
    if (!metalCtx.device) {
      [self release];
      return nil;
    }
    metalCtx.commandQueue = [metalCtx.device newCommandQueue];
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
  }
  return self;
}

- (BOOL)wantsUpdateLayer
{
  return NO;
}

- (CALayer*)makeBackingLayer
{
  CAMetalLayer* const layer = [CAMetalLayer layer];
  layer.device = metalCtx.device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = YES;
  layer.contentsScale = self.window ? self.window.backingScaleFactor : 1.0;

  [metalCtx.metalLayer release];
  metalCtx.metalLayer = [layer retain];
  return layer;
}

- (void)dealloc
{
  [metalCtx.metalLayer release];
  [metalCtx.commandQueue release];
  [metalCtx.device release];

  metalCtx.metalLayer   = nil;
  metalCtx.commandQueue = nil;
  metalCtx.device       = nil;

  [super dealloc];
}

- (void)resizeWithOldSuperviewSize:(NSSize)oldSize
{
  PuglWrapperView *wrapper = (PuglWrapperView *)[self superview];
  [super resizeWithOldSuperviewSize:oldSize];
  [wrapper setReshaped];
}

- (void)setFrameSize:(NSSize)newSize
{
  [super setFrameSize:newSize];
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
  (void)event;
  return YES;
}

- (void)viewDidMoveToSuperview
{
  [super viewDidMoveToSuperview];
  PuglWrapperView *wrapper = (PuglWrapperView *)[self superview];
  if (wrapper) {
    // Ensure PUGL_CONFIGURE fires before the first PUGL_EXPOSE.
    // A plain layer-backed NSView does not trigger reshape automatically.
    [wrapper setReshaped];
  }
}

- (void)viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  if (self.window) {
    metalCtx.metalLayer.contentsScale = self.window.backingScaleFactor;
    [self setNeedsDisplay:YES];
  }
}

- (void)drawRect:(NSRect)rect
{
  [super drawRect:rect];
}

- (void)viewWillDraw
{
  [super viewWillDraw];
  if (!puglview) return;
  PuglWrapperView *wrapper = (PuglWrapperView *)[self superview];
  [wrapper dispatchExpose:self.bounds];
}

- (void)viewDidChangeBackingProperties
{
  [super viewDidChangeBackingProperties];
  if (self.window) {
    metalCtx.metalLayer.contentsScale = self.window.backingScaleFactor;
  }
}

@end

// --- Pugl backend callbacks ---

static PuglStatus
puglMacMetalCreate(PuglView *view)
{
  PuglInternals  *impl     = view->impl;
  PuglMetalView  *drawView = [PuglMetalView alloc];

  drawView->puglview = view;
  drawView = [drawView initWithFrame:[impl->wrapperView bounds]];
  if (!drawView) {
    return PUGL_CREATE_CONTEXT_FAILED;
  }

  if (view->hints[PUGL_RESIZABLE]) {
    [drawView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  } else {
    [drawView setAutoresizingMask:NSViewNotSizable];
  }

  impl->drawView = drawView;
  return PUGL_SUCCESS;
}

static void
puglMacMetalDestroy(PuglView *view)
{
  PuglMetalView *drawView = (PuglMetalView *)view->impl->drawView;

  drawView->puglview = NULL;

  [drawView setNeedsDisplay:NO];
  [drawView removeFromSuperview];
  [drawView release];

  view->impl->drawView = nil;
}

static PuglStatus
puglMacMetalEnter(PuglView *view, const PuglExposeEvent *expose)
{
  PuglMetalView *drawView = (PuglMetalView *)view->impl->drawView;
  if (!drawView) return PUGL_FAILURE;

  if (expose) {
    PuglMetalContext *ctx = &drawView->metalCtx;

    CGSize size = [drawView convertSizeToBacking:drawView.bounds.size];
    if (size.width <= 0 || size.height <= 0) return PUGL_FAILURE;

    ctx->metalLayer.drawableSize = size;

    ctx->currentDrawable = [ctx->metalLayer nextDrawable];
    if (!ctx->currentDrawable) return PUGL_FAILURE;

    ctx->commandBuffer = [ctx->commandQueue commandBuffer];
    if (!ctx->commandBuffer) return PUGL_FAILURE;

    ctx->renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    ctx->renderPassDescriptor.colorAttachments[0].texture     = ctx->currentDrawable.texture;
    ctx->renderPassDescriptor.colorAttachments[0].loadAction  = MTLLoadActionClear;
    ctx->renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    ctx->renderPassDescriptor.colorAttachments[0].clearColor  =
        MTLClearColorMake(0.12, 0.12, 0.12, 1.0);

    ctx->renderEncoder =
        [ctx->commandBuffer renderCommandEncoderWithDescriptor:ctx->renderPassDescriptor];
    if (!ctx->renderEncoder) return PUGL_FAILURE;
  }

  return PUGL_SUCCESS;
}

static PuglStatus
puglMacMetalLeave(PuglView *view, const PuglExposeEvent *expose)
{
  PuglMetalView *drawView = (PuglMetalView *)view->impl->drawView;
  if (!drawView) return PUGL_FAILURE;

  if (expose) {
    PuglMetalContext *ctx = &drawView->metalCtx;

    [ctx->renderEncoder endEncoding];
    [ctx->commandBuffer presentDrawable:ctx->currentDrawable];
    [ctx->commandBuffer commit];

    ctx->renderEncoder        = nil;
    ctx->commandBuffer        = nil;
    ctx->currentDrawable      = nil;
    ctx->renderPassDescriptor = nil;
  }

  return PUGL_SUCCESS;
}

static void *
puglMacMetalGetContext(PuglView *view)
{
  PuglMetalView *drawView = (PuglMetalView *)view->impl->drawView;
  if (!drawView) return NULL;
  return &drawView->metalCtx;
}

// --- Public API ---

const PuglBackend *
puglMetalBackend(void)
{
  static const PuglBackend backend = {puglStubConfigure,
                                      puglMacMetalCreate,
                                      puglMacMetalDestroy,
                                      puglMacMetalEnter,
                                      puglMacMetalLeave,
                                      puglMacMetalGetContext};
  return &backend;
}

PuglStatus
puglEnterContext(PuglView *view)
{
  return view->backend->enter(view, NULL);
}

PuglStatus
puglLeaveContext(PuglView *view)
{
  return view->backend->leave(view, NULL);
}
