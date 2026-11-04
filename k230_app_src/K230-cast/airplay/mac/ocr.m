
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Vision/Vision.h>

int main(int argc, char **argv){
  @autoreleasepool {
    if (argc < 2) { printf("usage: ocr <img>\n"); return 2; }
    NSString *p = [NSString stringWithUTF8String:argv[1]];
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:p];
    if (!img) { printf("ERR cannot load %s\n", argv[1]); return 3; }
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) { printf("ERR no cgimage\n"); return 4; }
    CGFloat W = CGImageGetWidth(cg), H = CGImageGetHeight(cg);
    VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
    VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
    req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
    req.usesLanguageCorrection = NO;
    req.recognitionLanguages = @[@"zh-Hans", @"en-US"];
    NSError *err = nil;
    if (![h performRequests:@[req] error:&err]) {
      printf("ERR vision %s\n", [[err description] UTF8String]); return 5;
    }
    printf("IMG %.0fx%.0f results=%lu\n", W, H, (unsigned long)req.results.count);
    for (VNRecognizedTextObservation *o in req.results) {
      VNRecognizedText *t = [[o topCandidates:1] firstObject];
      if (!t) continue;
      CGRect r = o.boundingBox;   // normalized, origin bottom-left
      double x = r.origin.x * W;
      double y = (1.0 - r.origin.y - r.size.height) * H;
      double w = r.size.width * W;
      double h = r.size.height * H;
      printf("%.0f %.0f %.0f %.0f\t%.3f\t%s\n", x, y, w, h, t.confidence, [t.string UTF8String]);
    }
  }
  return 0;
}
