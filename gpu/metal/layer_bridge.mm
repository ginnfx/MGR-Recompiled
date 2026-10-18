#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <string>

namespace mgr::gpu {
// SDL owns the view and layer. ARC only borrows it for AppKit properties that
// aren't exposed by metal-cpp's narrow QuartzCore wrapper.
void ConfigureMetalLayer(void* layer) {
  CAMetalLayer* native_layer = (__bridge CAMetalLayer*)layer;
  native_layer.opaque = YES;
}

// Driver/compiler failures may use Objective-C exceptions. Keep that boundary
// here so a stale archive cannot terminate the C++ compilation worker.
void* CompileMetalPipeline(void* compiler, void* descriptor, void* options,
                          bool* archive_rejected, std::string* message) {
  id<MTL4Compiler> native_compiler = (__bridge id<MTL4Compiler>)compiler;
  MTL4PipelineDescriptor* native_descriptor = (__bridge MTL4PipelineDescriptor*)descriptor;
  MTL4CompilerTaskOptions* native_options = (__bridge MTL4CompilerTaskOptions*)options;
  NSError* error = nil;
  id<MTLRenderPipelineState> result = nil;
  @try {
    result = [native_compiler newRenderPipelineStateWithDescriptor:native_descriptor
        compilerTaskOptions:native_options error:&error];
  } @catch (NSException* exception) {
    *message = exception.reason.UTF8String ?: "Metal compiler exception";
  }
  if (!result && native_options.lookupArchives.count) {
    *archive_rejected = true;
    @try {
      error = nil;
      result = [native_compiler newRenderPipelineStateWithDescriptor:native_descriptor
          compilerTaskOptions:nil error:&error];
    } @catch (NSException* exception) {
      *message = exception.reason.UTF8String ?: "Metal compiler fallback exception";
    }
  }
  if (!result && error) *message = error.localizedDescription.UTF8String;
  return (__bridge_retained void*)result;
}
}
