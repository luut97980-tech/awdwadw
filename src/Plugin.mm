#import <UIKit/UIKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include "mobile_menu.hpp"
#include "game_backend.hpp"
#include "ios_build_guard.hpp"
#include "app_metadata_apple.hpp"
#include "generated_font.hpp"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>
#include "imgui_impl_metal.h"
#include <cfloat>
#include <cmath>
#include <limits>

@interface NSBundle (VTBundleSpoof)
-(NSString*)vt_bundleIdentifier;
-(id)vt_objectForInfoDictionaryKey:(NSString*)key;
-(NSDictionary<NSString*,id>*)vt_infoDictionary;
@end

@implementation NSBundle (VTBundleSpoof)
-(NSString*)vt_bundleIdentifier{
    if(self==NSBundle.mainBundle)return @"com.vng.playtogether";
    return [self vt_bundleIdentifier];
}
-(id)vt_objectForInfoDictionaryKey:(NSString*)key{
    if(self==NSBundle.mainBundle&&[key isKindOfClass:NSString.class]&&[key isEqualToString:@"CFBundleIdentifier"]){
        return @"com.vng.playtogether";
    }
    return [self vt_objectForInfoDictionaryKey:key];
}
-(NSDictionary<NSString*,id>*)vt_infoDictionary{
    NSDictionary<NSString*,id>*dict=[self vt_infoDictionary];
    if(self==NSBundle.mainBundle&&dict){
        id cur=dict[@"CFBundleIdentifier"];
        if(![cur isKindOfClass:NSString.class]||![(NSString*)cur isEqualToString:@"com.vng.playtogether"]){
            NSMutableDictionary<NSString*,id>*copy=[dict mutableCopy];
            copy[@"CFBundleIdentifier"]=@"com.vng.playtogether";
            return copy;
        }
    }
    return dict;
}
@end

static void VTSwizzleMethod(Class cls,SEL origSel,SEL swizSel){
    Method origMethod=class_getInstanceMethod(cls,origSel);
    Method swizMethod=class_getInstanceMethod(cls,swizSel);
    if(!origMethod||!swizMethod)return;
    if(class_addMethod(cls,origSel,method_getImplementation(swizMethod),method_getTypeEncoding(swizMethod))){
        class_replaceMethod(cls,swizSel,method_getImplementation(origMethod),method_getTypeEncoding(origMethod));
    }else{
        method_exchangeImplementations(origMethod,swizMethod);
    }
}

static void VTSpoofBundleIdentifier(){
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken,^{
        Class cls=NSBundle.class;
        VTSwizzleMethod(cls,@selector(bundleIdentifier),@selector(vt_bundleIdentifier));
        VTSwizzleMethod(cls,@selector(objectForInfoDictionaryKey:),@selector(vt_objectForInfoDictionaryKey:));
        VTSwizzleMethod(cls,@selector(infoDictionary),@selector(vt_infoDictionary));
        @try{
            CFBundleRef mainCF=CFBundleGetMainBundle();
            if(mainCF){
                CFDictionaryRef cfDict=CFBundleGetInfoDictionary(mainCF);
                if(cfDict){
                    NSDictionary*nsDict=(__bridge NSDictionary*)cfDict;
                    if([nsDict isKindOfClass:NSMutableDictionary.class]){
                        ((NSMutableDictionary*)nsDict)[@"CFBundleIdentifier"]=@"com.vng.playtogether";
                    }
                }
            }
        }@catch(id){}
    });
}

static void* VTUnityHandle=nullptr;
static std::string VTGuardMessage,VTGuardDetails;
static bool VTGuardMayRetry=true;
static bool VTCheckBuild(){
    VTGuardMayRetry=true;
    const auto metadata=vt::ios::readAppMetadata();
    const auto decision=vt::ios::chooseMetadata(metadata.disk,metadata.bundle);
    VTGuardDetails=vt::ios::metadataReport(metadata)+"\nMetadata source: "+decision.source;
    if(!decision.compatible){
        VTGuardMayRetry=decision.retryable;
        const auto& observed=metadata.disk.present?metadata.disk:metadata.bundle;
        VTGuardMessage=decision.retryable?"Waiting for app metadata":
            "App metadata mismatch: "+vt::ios::metadataLine("observed",observed);
        return false;
    }
    for(uint32_t i=0;i<_dyld_image_count();++i){
        const char*name=_dyld_get_image_name(i);if(!name||!strstr(name,"/UnityFramework.framework/UnityFramework"))continue;
        const auto*h=reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if(!h||h->magic!=MH_MAGIC_64||h->cputype!=CPU_TYPE_ARM64||h->ncmds>4096||h->sizeofcmds>1048576){VTGuardMayRetry=false;VTGuardMessage="Invalid UnityFramework Mach-O header";return false;}
        const auto*base=reinterpret_cast<const unsigned char*>(h);const unsigned char*cmd=base+sizeof(*h),*end=cmd+h->sizeofcmds;
        std::string uuid;uint64_t textStart=0,textEnd=0;
        for(uint32_t j=0;j<h->ncmds;++j){
            if(cmd+sizeof(load_command)>end){VTGuardMayRetry=false;VTGuardMessage="Invalid Mach-O command table";return false;}
            const auto*lc=reinterpret_cast<const load_command*>(cmd);if(lc->cmdsize<sizeof(*lc)||cmd+lc->cmdsize>end){VTGuardMayRetry=false;VTGuardMessage="Invalid Mach-O command";return false;}
            if(lc->cmd==LC_UUID&&lc->cmdsize>=sizeof(uuid_command)){auto*u=reinterpret_cast<const uuid_command*>(cmd);char hex[33];for(int k=0;k<16;k++)snprintf(hex+k*2,3,"%02x",u->uuid[k]);uuid=hex;}
            if(lc->cmd==LC_SEGMENT_64&&lc->cmdsize>=sizeof(segment_command_64)){auto*seg=reinterpret_cast<const segment_command_64*>(cmd);if(strncmp(seg->segname,"__TEXT",16)==0){textStart=seg->vmaddr;textEnd=textStart+seg->vmsize;}}
            cmd+=lc->cmdsize;
        }
        VTGuardDetails+="\nUnityFramework UUID: "+uuid+"\nExpected UUID: "+vt::ios::ExpectedFrameworkUUID;
        if(uuid!=vt::ios::ExpectedFrameworkUUID){VTGuardMayRetry=false;VTGuardMessage="UnityFramework UUID mismatch";return false;}
        for(const auto&g:vt::ios::MethodGuards){
            if(g.rva<textStart||g.rva>textEnd||g.bytes.size()>textEnd-g.rva||memcmp(base+g.rva,g.bytes.data(),g.bytes.size())!=0){
                char detail[80];snprintf(detail,sizeof(detail),"iOS method signature mismatch at RVA 0x%llx",static_cast<unsigned long long>(g.rva));
                VTGuardMayRetry=false;VTGuardMessage=detail;return false;
            }
        }
        if(!VTUnityHandle)VTUnityHandle=dlopen(name,RTLD_LAZY|RTLD_NOLOAD);
        if(!VTUnityHandle){VTGuardMessage="UnityFramework is not loaded";return false;}
        VTGuardMessage="Installed app metadata + UUID + iOS method signatures confirmed";return true;
    }
    VTGuardMessage="Waiting for UnityFramework (will retry)";return false;
}

@class VTMenuController;
@class VTPassthroughView;

@class VTMenuController;

@interface VTJumpGestureRecognizer : UIGestureRecognizer <UIGestureRecognizerDelegate>
@property(nonatomic, weak) VTMenuController* menuController;
@end

@implementation VTJumpGestureRecognizer {
    NSMutableSet<UITouch*>* _jumpTouches;
}
-(instancetype)init{
    if(self = [super initWithTarget:nil action:nil]){
        self.cancelsTouchesInView = NO;
        self.delaysTouchesBegan = NO;
        self.delaysTouchesEnded = NO;
        self.delegate = self;
        _jumpTouches = [NSMutableSet setWithCapacity:8];
    }
    return self;
}
-(BOOL)canPreventGestureRecognizer:(UIGestureRecognizer *)preventedGestureRecognizer { return NO; }
-(BOOL)canBePreventedByGestureRecognizer:(UIGestureRecognizer *)preventingGestureRecognizer { return NO; }
-(BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer { return YES; }

-(BOOL)isJumpTouch:(UITouch*)touch inView:(UIView*)view {
    if(!view) return NO;
    CGPoint p = [touch locationInView:view];
    CGSize sz = view.bounds.size;
    if(sz.width <= 0 || sz.height <= 0) return NO;
    if(sz.width >= sz.height) { // Landscape: jump button is bottom-right
        return (p.x >= sz.width * 0.58f && p.y >= sz.height * 0.40f);
    } else { // Portrait: jump button is bottom-right
        return (p.x >= sz.width * 0.50f && p.y >= sz.height * 0.60f);
    }
}
-(void)updateJumpState{
    BOOL held = (_jumpTouches.count > 0);
    [self.menuController setJumpHeld:held];
}
-(void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    [super touchesBegan:touches withEvent:event];
    for(UITouch* t in touches){
        if([self isJumpTouch:t inView:self.view]){
            [_jumpTouches addObject:t];
        }
    }
    [self updateJumpState];
}
-(void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    [super touchesMoved:touches withEvent:event];
    for(UITouch* t in touches){
        if([self isJumpTouch:t inView:self.view]){
            [_jumpTouches addObject:t];
        }else{
            [_jumpTouches removeObject:t];
        }
    }
    [self updateJumpState];
}
-(void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    [super touchesEnded:touches withEvent:event];
    for(UITouch* t in touches){
        [_jumpTouches removeObject:t];
    }
    [self updateJumpState];
}
-(void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    [super touchesCancelled:touches withEvent:event];
    for(UITouch* t in touches){
        [_jumpTouches removeObject:t];
    }
    [self updateJumpState];
}
-(void)reset{
    [super reset];
    [_jumpTouches removeAllObjects];
    [self updateJumpState];
}
@end

@interface VTMetalView : MTKView
@property(nonatomic,weak) id touchOwner;
@end

// Plain NSObject controller (not a UIViewController) so its overlay view is a
// pure UIView with no _viewDelegate. This prevents UIViewControllerHierarchyInconsistency
// without creating any extra UIWindow that would interfere with PassSDK/login flows.
@interface VTMenuController : NSObject<MTKViewDelegate> {
    ImGuiContext* _context;
    vt::ios::Model _model;
    vt::ios::Actions _actions;
    std::unique_ptr<vt::ios::GameBackend> _backend;
    UITouch* _primaryTouch;
    CGPoint _touchStart,_touchPrevious;
    BOOL _panning;
    CFTimeInterval _lastFrame,_nextBuildCheck,_resumeAfter;
    CGSize _lastLayoutSize;
    BOOL _isScreenCaptured;
}
@property(nonatomic,strong) VTPassthroughView* view;
@property(nonatomic,strong) VTMetalView* metalView;
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLCommandQueue> commandQueue;
-(instancetype)initWithFrame:(CGRect)frame;
-(void)beginTouches:(NSSet<UITouch*>*)touches;
-(void)moveTouches:(NSSet<UITouch*>*)touches;
-(void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled;
-(void)setRenderingActive:(BOOL)active;
-(BOOL)acceptsPoint:(CGPoint)point;
-(void)tryStartBackend:(CFTimeInterval)now;
-(void)handleLayoutChange:(CGSize)size;
-(void)setJumpHeld:(BOOL)held;
-(void)handleScreenCaptureChanged:(BOOL)captured;
-(void)activateKeyString:(NSString*)rawKey;
-(void)checkOrActivateKey:(NSString*)key token:(NSString*)token;
@end

@interface VTPassthroughView:UIView
@property(nonatomic,weak) VTMenuController*menuController;
@end
@implementation VTPassthroughView
-(UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event{
    if(self.hidden||self.alpha<.01||!self.userInteractionEnabled||![self pointInside:point withEvent:event]||!self.menuController.metalView)return nil;
    CGPoint local=[self.menuController.metalView convertPoint:point fromView:self];
    if(![self.menuController acceptsPoint:local])return nil;
    return [super hitTest:point withEvent:event];
}
-(void)layoutSubviews{
    [super layoutSubviews];
    [self.menuController handleLayoutChange:self.bounds.size];
}
@end

@implementation VTMetalView
-(void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner beginTouches:touches];}
-(void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner moveTouches:touches];}
-(void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner endTouches:touches cancelled:NO];}
-(void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{[(VTMenuController*)self.touchOwner endTouches:touches cancelled:YES];}
@end

@implementation VTMenuController
-(void)setJumpHeld:(BOOL)held{
    _model.jumpPressed = (held == YES);
}
-(void)handleScreenCaptureChanged:(BOOL)captured{
    _isScreenCaptured = captured;
    if(_model.antiCapture && captured){
        self.metalView.hidden = YES;
    } else {
        self.metalView.hidden = NO;
    }
}
-(NSString*)sha256Hex:(NSString*)input{
    const char* str = [input UTF8String];
    unsigned char result[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(str, (CC_LONG)strlen(str), result);
    NSMutableString* hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for(int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++){
        [hex appendFormat:@"%02x", result[i]];
    }
    return hex;
}
-(NSString*)randomHex64{
    unsigned char bytes[32];
    arc4random_buf(bytes, sizeof(bytes));
    NSMutableString* hex = [NSMutableString stringWithCapacity:64];
    for(int i = 0; i < 32; i++) [hex appendFormat:@"%02x", bytes[i]];
    return hex;
}
-(NSString*)deviceIdentifier{
    NSString* vendor = UIDevice.currentDevice.identifierForVendor.UUIDString;
    if(!vendor || vendor.length == 0) vendor = @"00000000-0000-0000-0000-000000000000";
    return [self sha256Hex:[NSString stringWithFormat:@"keydesk-device-v1:%@", vendor.lowercaseString]];
}
-(void)activateKeyString:(NSString*)rawKey{
    if(!rawKey || rawKey.length == 0) return;
    NSString* clean = [rawKey stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]].uppercaseString;
    _model.license.statusText = "Đang xác thực với máy chủ 45.131.65.244:30019...";
    NSString* devId = [self deviceIdentifier];
    NSString* nonce = [self randomHex64];
    NSDictionary* payload = @{
        @"key": clean,
        @"device_id": devId,
        @"nonce": nonce,
        @"build_id": @"PTG-iOS-R5.7"
    };
    NSURL* url = [NSURL URLWithString:@"http://45.131.65.244:30019/api/v1/activate"];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8.0];
    req.HTTPMethod = @"POST";
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    
    __weak VTMenuController* weakSelf = self;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            VTMenuController* strong = weakSelf;
            if(!strong) return;
            if(error){
                strong->_model.license.statusText = [NSString stringWithFormat:@"Lỗi mạng: %@", error.localizedDescription].UTF8String;
                return;
            }
            NSHTTPURLResponse* httpResp = (NSHTTPURLResponse*)response;
            id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            if([json isKindOfClass:NSDictionary.class] && [json[@"ok"] boolValue]){
                strong->_model.license.valid = true;
                strong->_model.license.key = clean.UTF8String;
                strncpy(strong->_model.keyInput, clean.UTF8String, sizeof(strong->_model.keyInput) - 1);
                if([json[@"token"] isKindOfClass:NSString.class]) strong->_model.license.token = [json[@"token"] UTF8String];
                if([json[@"expires_at"] isKindOfClass:NSNumber.class]) strong->_model.license.expiresAt = [json[@"expires_at"] longLongValue];
                if([json[@"remaining_seconds"] isKindOfClass:NSNumber.class]) strong->_model.license.remainingSeconds = [json[@"remaining_seconds"] longLongValue];
                long long rem = strong->_model.license.remainingSeconds;
                long days = rem / 86400; long hours = (rem % 86400) / 3600;
                strong->_model.license.statusText = [NSString stringWithFormat:@"Key Hợp Lệ • Còn %ld ngày %ld giờ", days, hours].UTF8String;
                strong->_model.toast = "Kích hoạt Key thành công!";
                strong->_model.toastUntil = strong->_model.time + 2.0;
                [strong saveSettings];
            } else {
                strong->_model.license.valid = false;
                NSString* errStr = nil;
                if([json isKindOfClass:NSDictionary.class] && [json[@"error"] isKindOfClass:NSString.class]){
                    errStr = json[@"error"];
                } else {
                    errStr = [NSString stringWithFormat:@"HTTP %ld", (long)httpResp.statusCode];
                }
                strong->_model.license.statusText = [NSString stringWithFormat:@"Kích hoạt thất bại: %@", errStr].UTF8String;
            }
        });
    }];
    [task resume];
}
-(void)checkOrActivateKey:(NSString*)key token:(NSString*)token{
    if(!token || token.length == 0){
        [self activateKeyString:key];
        return;
    }
    NSString* devId = [self deviceIdentifier];
    NSDictionary* payload = @{
        @"token": token,
        @"device_id": devId
    };
    NSURL* url = [NSURL URLWithString:@"http://45.131.65.244:30019/api/v1/check"];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8.0];
    req.HTTPMethod = @"POST";
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    
    __weak VTMenuController* weakSelf = self;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            VTMenuController* strong = weakSelf;
            if(!strong) return;
            if(error) return; // Retain offline state
            id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            if([json isKindOfClass:NSDictionary.class] && [json[@"ok"] boolValue]){
                strong->_model.license.valid = true;
                if([json[@"expires_at"] isKindOfClass:NSNumber.class]) strong->_model.license.expiresAt = [json[@"expires_at"] longLongValue];
                if([json[@"remaining_seconds"] isKindOfClass:NSNumber.class]) strong->_model.license.remainingSeconds = [json[@"remaining_seconds"] longLongValue];
                long long rem = strong->_model.license.remainingSeconds;
                long days = rem / 86400; long hours = (rem % 86400) / 3600;
                strong->_model.license.statusText = [NSString stringWithFormat:@"Key Hợp Lệ • Còn %ld ngày %ld giờ", days, hours].UTF8String;
                [strong saveSettings];
            } else {
                [strong activateKeyString:key];
            }
        });
    }];
    [task resume];
}

-(instancetype)initWithFrame:(CGRect)frame{
    self=[super init];
    if(!self)return nil;
    VTPassthroughView*root=[[VTPassthroughView alloc]initWithFrame:frame];
    root.menuController=self;
    root.backgroundColor=UIColor.clearColor;
    root.opaque=NO;
    root.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    self.view=root;
    [self setupView];
    return self;
}
-(NSURL*)settingsURL{
    NSURL*base=[[NSFileManager defaultManager]URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL*folder=[base URLByAppendingPathComponent:@"VTMenu" isDirectory:YES];
    [[NSFileManager defaultManager]createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return [folder URLByAppendingPathComponent:@"config.json"];
}
-(void)restoreSettings{
    NSData*data=[NSData dataWithContentsOfURL:[self settingsURL]];
    id parsed=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    NSDictionary*saved=[parsed isKindOfClass:NSDictionary.class]?parsed:@{};
    if([saved[@"scale"] isKindOfClass:NSNumber.class])_model.scale=[saved[@"scale"] floatValue];
    if([saved[@"opacity"] isKindOfClass:NSNumber.class])_model.opacity=[saved[@"opacity"] floatValue];
    if([saved[@"jump_interval_ms"] isKindOfClass:NSNumber.class])_model.jumpIntervalMs=[saved[@"jump_interval_ms"] doubleValue];
    if([saved[@"anti_capture"] isKindOfClass:NSNumber.class])_model.antiCapture=[saved[@"anti_capture"] boolValue];
    if([saved[@"floating_btn_x"] isKindOfClass:NSNumber.class]&&[saved[@"floating_btn_y"] isKindOfClass:NSNumber.class]){
        _model.floatingButtonPos={ [saved[@"floating_btn_x"] floatValue], [saved[@"floating_btn_y"] floatValue] };
    }
    if([saved[@"menu_win_x"] isKindOfClass:NSNumber.class]&&[saved[@"menu_win_y"] isKindOfClass:NSNumber.class]){
        _model.menuWindowPos={ [saved[@"menu_win_x"] floatValue], [saved[@"menu_win_y"] floatValue] };
    }
    if([saved[@"license_valid"] isKindOfClass:NSNumber.class])_model.license.valid=[saved[@"license_valid"] boolValue];
    if([saved[@"license_key"] isKindOfClass:NSString.class]){
        _model.license.key=[saved[@"license_key"] UTF8String];
        strncpy(_model.keyInput, _model.license.key.c_str(), sizeof(_model.keyInput)-1);
    }
    if([saved[@"license_token"] isKindOfClass:NSString.class])_model.license.token=[saved[@"license_token"] UTF8String];
    if([saved[@"license_expires"] isKindOfClass:NSNumber.class])_model.license.expiresAt=[saved[@"license_expires"] longLongValue];
    if([saved[@"license_remaining"] isKindOfClass:NSNumber.class])_model.license.remainingSeconds=[saved[@"license_remaining"] longLongValue];
    if([saved[@"license_status"] isKindOfClass:NSString.class])_model.license.statusText=[saved[@"license_status"] UTF8String];
    
    vt::ios::sanitize(_model);_model.enabled.fill(false);_model.visible=false;_model.jumpPressed=false;
    if(!_model.license.key.empty()){
        NSString* savedKey=[NSString stringWithUTF8String:_model.license.key.c_str()];
        NSString* savedToken=[NSString stringWithUTF8String:_model.license.token.c_str()];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self checkOrActivateKey:savedKey token:savedToken];
        });
    }
}
-(void)saveSettings{
    vt::ios::sanitize(_model);
    NSDictionary*settings=@{
        @"schema":@4,
        @"scale":@(_model.scale),
        @"opacity":@(_model.opacity),
        @"jump_interval_ms":@(_model.jumpIntervalMs),
        @"anti_capture":@(_model.antiCapture),
        @"floating_btn_x":@(_model.floatingButtonPos.x),
        @"floating_btn_y":@(_model.floatingButtonPos.y),
        @"menu_win_x":@(_model.menuWindowPos.x),
        @"menu_win_y":@(_model.menuWindowPos.y),
        @"license_valid":@(_model.license.valid),
        @"license_key":[NSString stringWithUTF8String:_model.license.key.c_str()],
        @"license_token":[NSString stringWithUTF8String:_model.license.token.c_str()],
        @"license_expires":@(_model.license.expiresAt),
        @"license_remaining":@(_model.license.remainingSeconds),
        @"license_status":[NSString stringWithUTF8String:_model.license.statusText.c_str()]
    };
    NSData*data=[NSJSONSerialization dataWithJSONObject:settings options:NSJSONWritingPrettyPrinted error:nil];
    NSError*error=nil;
    if(!data||![data writeToURL:[self settingsURL] options:NSDataWritingAtomic error:&error])_model.status="Không ghi được config.json";
}
-(BOOL)acceptsPoint:(CGPoint)p{
    return vt::ios::acceptsPoint(_model,{float(p.x),float(p.y)});
}
-(void)setupView{
    [self restoreSettings];self.device=MTLCreateSystemDefaultDevice();
    if(!self.device){UILabel*label=[[UILabel alloc]initWithFrame:self.view.bounds];label.text=@"Thiết bị không khởi tạo được Metal.";label.textColor=UIColor.whiteColor;label.numberOfLines=0;label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:label];return;}
    self.commandQueue=[self.device newCommandQueue];
    self.metalView=[[VTMetalView alloc]initWithFrame:self.view.bounds device:self.device];self.metalView.delegate=self;self.metalView.touchOwner=self;
    self.metalView.multipleTouchEnabled=YES;self.metalView.preferredFramesPerSecond=60;
    self.metalView.colorPixelFormat=MTLPixelFormatBGRA8Unorm;self.metalView.opaque=NO;self.metalView.layer.opaque=NO;self.metalView.backgroundColor=UIColor.clearColor;self.metalView.clearColor=MTLClearColorMake(0,0,0,0);
    self.metalView.translatesAutoresizingMaskIntoConstraints=NO;[self.view addSubview:self.metalView];
    UIView*g=self.view;
    [NSLayoutConstraint activateConstraints:@[[self.metalView.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],[self.metalView.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],[self.metalView.topAnchor constraintEqualToAnchor:g.topAnchor],[self.metalView.bottomAnchor constraintEqualToAnchor:g.bottomAnchor]]];
    IMGUI_CHECKVERSION();_context=ImGui::CreateContext();ImGuiIO&io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    CGFloat scale=UIScreen.mainScreen.scale;ImFontConfig cfg;cfg.OversampleH=2;cfg.OversampleV=1;cfg.FontDataOwnedByAtlas=false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(vt::ios::FontBytes),int(sizeof(vt::ios::FontBytes)),16.f*scale,&cfg,io.Fonts->GetGlyphRangesVietnamese());
    if(io.Fonts->Fonts.empty())io.Fonts->AddFontDefault();
    // Rasterize at Retina scale, then return glyph geometry to UIKit points.
    for(ImFont*f:io.Fonts->Fonts)f->Scale=1.f/float(scale);
    vt::ios::style();ImGui_ImplMetal_Init(self.device);
    _model.available.fill(false);
    for(int id:{vt::Esp,vt::Line,vt::SpamJump})_model.available[id]=true;
    _resumeAfter=0;
    _nextBuildCheck=0;
    _model.status="Mở menu sau khi đăng nhập để kết nối";
    __weak VTMenuController*weakSelf=self;
    _actions.save=[weakSelf]{VTMenuController*strong=weakSelf;if(strong)[strong saveSettings];};
    _actions.stop=[weakSelf]{VTMenuController*strong=weakSelf;if(strong&&strong->_backend)strong->_backend->stop(strong->_model);};
    _actions.number=[weakSelf](int,double){VTMenuController*strong=weakSelf;if(strong)[strong saveSettings];};
    _actions.toggle=[weakSelf](int id){VTMenuController*strong=weakSelf;if(strong&&strong->_backend)strong->_backend->toggle(strong->_model,id);};
    _actions.activateKey=[weakSelf](const std::string& k){
        VTMenuController*strong=weakSelf;if(strong)[strong activateKeyString:[NSString stringWithUTF8String:k.c_str()]];
    };
    _actions.pasteKey=[weakSelf]{
        VTMenuController*strong=weakSelf;if(strong){
            NSString*clip=UIPasteboard.generalPasteboard.string;
            if(clip){
                NSString*clean=[clip stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]].uppercaseString;
                strncpy(strong->_model.keyInput, clean.UTF8String, sizeof(strong->_model.keyInput)-1);
            }
        }
    };
    _actions.clearKey=[weakSelf]{
        VTMenuController*strong=weakSelf;if(strong){
            strong->_model.license.valid=false;
            strong->_model.license.key.clear();
            strong->_model.license.token.clear();
            strong->_model.license.statusText="Đã đổi key; vui lòng nhập key mới";
            strong->_model.keyInput[0]="\0";
            [strong saveSettings];
        }
    };
    _actions.toggleAntiCapture=[weakSelf](bool enabled){
        VTMenuController*strong=weakSelf;if(strong){
            strong->_model.antiCapture=enabled;
            [strong saveSettings];
        }
    };
    _actions.diagnostic=[weakSelf]{VTMenuController*strong=weakSelf;if(strong){
        if(!strong->_backend){strong->_nextBuildCheck=0;[strong tryStartBackend:CACurrentMediaTime()];}
        const auto metadata=vt::ios::readAppMetadata();
        UIView*host=strong.view.superview;const CGSize size=host.bounds.size;const CGAffineTransform t=host.transform;const UIEdgeInsets insets=host.safeAreaInsets;
        const char*hostClass=NSStringFromClass(host.class).UTF8String;char hostInfo[320];
        snprintf(hostInfo,sizeof(hostInfo),"UI container: %s; bounds=%.0fx%.0f; inset=%.0f,%.0f,%.0f,%.0f; orientation=%ld; transform=%.2f,%.2f,%.2f,%.2f",hostClass?hostClass:"none",double(size.width),double(size.height),double(insets.top),double(insets.left),double(insets.bottom),double(insets.right),long(host.window.windowScene.interfaceOrientation),double(t.a),double(t.b),double(t.c),double(t.d));
        strong->_model.diagnostic="PTG iOS R5.7 | target VNG 2.32.0 (227628) | arm64\n"+VTGuardMessage+"\n"+VTGuardDetails+
            "\n"+std::string(hostInfo)+"\nCurrent metadata:\n"+vt::ios::metadataReport(metadata)+"\nStatus: "+strong->_model.status+"\n"+
            (strong->_backend?strong->_backend->diagnostic():"Backend unavailable")+
            "\nWeb key client (45.131.65.244:30019) and screen capture protection are active on iOS.";
        UIPasteboard.generalPasteboard.string=[NSString stringWithUTF8String:strong->_model.diagnostic.c_str()];
        strong->_model.toast="Đã sao chép chẩn đoán";strong->_model.toastUntil=strong->_model.time+1.;
    }};
}
-(void)tryStartBackend:(CFTimeInterval)now{
    if(_backend||now<_nextBuildCheck)return;
    _model.connected=false;_model.controlsAllowed=false;
    if(!VTCheckBuild()){
        _model.status=VTGuardMessage;
        _nextBuildCheck=VTGuardMayRetry?now+1.:std::numeric_limits<double>::infinity();
        return;
    }
    try{
        _backend=std::make_unique<vt::ios::GameBackend>([](const char*n){return dlsym(VTUnityHandle,n);});
        _model.connected=true;_model.controlsAllowed=true;
        _model.status="Đã xác minh bản game; bật ESP, Line hoặc Spam nhảy";
    }catch(const std::exception&e){
        _model.status=e.what();_nextBuildCheck=std::numeric_limits<double>::infinity();
    }
}
-(void)handleLayoutChange:(CGSize)size{
    if(size.width<=0||size.height<=0)return;
    if(!CGSizeEqualToSize(size,_lastLayoutSize)){
        [self cancelMouse];_primaryTouch=nil;_panning=NO;_model.scrollDelta=0;_model.suppressActions=true;
        _model.windowMin=_model.windowMax={};_lastLayoutSize=size;
        if(_backend)_backend->suspend(CACurrentMediaTime());
    }
}
-(void)cancelMouse{
    if(!_context)return;ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);io.AddMouseButtonEvent(0,false);
}
-(void)beginTouches:(NSSet<UITouch*>*)touches{
    if(!_context||_primaryTouch)return;_primaryTouch=touches.anyObject;
    _touchStart=_touchPrevious=[_primaryTouch locationInView:self.metalView];_panning=NO;_model.suppressActions=false;
    ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(_touchStart.x,_touchStart.y);io.AddMouseButtonEvent(0,true);
}
-(void)moveTouches:(NSSet<UITouch*>*)touches{
    if(!_context||!_primaryTouch||![touches containsObject:_primaryTouch])return;
    CGPoint p=[_primaryTouch locationInView:self.metalView];CGFloat dx=p.x-_touchStart.x,dy=p.y-_touchStart.y;
    const BOOL inContent=_touchStart.x>=_model.contentMin.x&&_touchStart.x<_model.contentMax.x&&_touchStart.y>=_model.contentMin.y&&_touchStart.y<_model.contentMax.y;
    if(!_panning&&_model.visible&&inContent&&!_model.sliderActive&&fabs(dy)>14&&fabs(dy)>fabs(dx)*1.5){_panning=YES;_model.suppressActions=true;[self cancelMouse];}
    if(_panning){_model.scrollDelta+=p.y-_touchPrevious.y;}
    else{ImGui::SetCurrentContext(_context);ImGui::GetIO().AddMousePosEvent(p.x,p.y);}
    _touchPrevious=p;
}
-(void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled{
    if(!_context||!_primaryTouch||![touches containsObject:_primaryTouch])return;
    ImGui::SetCurrentContext(_context);
    if(cancelled||_panning){_model.suppressActions=true;[self cancelMouse];}
    else{CGPoint p=[_primaryTouch locationInView:self.metalView];auto&io=ImGui::GetIO();io.AddMousePosEvent(p.x,p.y);io.AddMouseButtonEvent(0,false);}
    _primaryTouch=nil;_panning=NO;
}
-(void)setRenderingActive:(BOOL)active{
    [self cancelMouse];_primaryTouch=nil;_panning=NO;_model.scrollDelta=0;_model.suppressActions=true;
    if(!active){if(_backend)_backend->stop(_model);_model.visible=false;_model.enabled.fill(false);[self saveSettings];_resumeAfter=0;}
    else{
        _resumeAfter=CACurrentMediaTime()+1.;
        if(_backend)_backend->suspend(CACurrentMediaTime());
    }
    self.metalView.paused=!active;_lastFrame=0;
}
-(void)drawInMTKView:(MTKView*)view{
    if(!_context||view.paused||view.bounds.size.width<=0||view.bounds.size.height<=0)return;
    if(_model.antiCapture&&(_isScreenCaptured||UIScreen.mainScreen.isCaptured)){
        view.clearColor=MTLClearColorMake(0,0,0,0);
        return;
    }
    ImGui::SetCurrentContext(_context);auto&io=ImGui::GetIO();io.DisplaySize={float(view.bounds.size.width),float(view.bounds.size.height)};
    io.DisplayFramebufferScale={float(view.drawableSize.width/view.bounds.size.width),float(view.drawableSize.height/view.bounds.size.height)};
    CFTimeInterval now=CACurrentMediaTime();io.DeltaTime=_lastFrame?float(fmin(.1,fmax(.001,now-_lastFrame))):1.f/60;_lastFrame=now;
    const CGSize bounds=view.bounds.size;
    if(!CGSizeEqualToSize(bounds,_lastLayoutSize)){
        [self cancelMouse];_primaryTouch=nil;_panning=NO;_model.scrollDelta=0;_model.suppressActions=true;
        _model.windowMin=_model.windowMax={};_lastLayoutSize=bounds;
        if(_backend)_backend->suspend(now);
    }
    id<CAMetalDrawable> drawable=view.currentDrawable;MTLRenderPassDescriptor*pass=view.currentRenderPassDescriptor;if(!drawable||!pass)return;
    id<MTLCommandBuffer>command=[self.commandQueue commandBuffer];if(!command)return;
    _model.time=now;_model.insetMin={float(view.safeAreaInsets.left),float(view.safeAreaInsets.top)};_model.insetMax={float(view.safeAreaInsets.right),float(view.safeAreaInsets.bottom)};
    // Only connect backend when user opens the menu or enables a feature,
    // staying completely passive during app startup and account login.
    if(_model.visible||vt::ios::runtimeRequested(_model)){
        [self tryStartBackend:now];
    }
    if(now>=_resumeAfter&&_backend)_backend->tick(_model,now);
    bool busy=_model.visible||vt::ios::runtimeRequested(_model);view.preferredFramesPerSecond=busy?60:15;
    ImGui_ImplMetal_NewFrame(pass);ImGui::NewFrame();
    if(_backend&&(_model.enabled[vt::Line]||_model.enabled[vt::Esp])){
        auto*d=ImGui::GetBackgroundDrawList();
        const float w=io.DisplaySize.x,h=io.DisplaySize.y;
        const ImU32 darkOutline=IM_COL32(0,0,0,185);
        // 1. Draw Route Line segments (Ported from Windows overlay.html)
        if(_model.enabled[vt::Line]){
            for(const auto&seg:_backend->route()){
                const ImVec2 a{seg.x0*w,seg.y0*h};
                const ImVec2 b{seg.x1*w,seg.y1*h};
                ImU32 col=IM_COL32(53,255,101,242); // #35ff65 green (walk)
                if(seg.colorKind==1)col=IM_COL32(255,213,74,235); // #ffd54a yellow (drop)
                else if(seg.colorKind==2)col=IM_COL32(32,224,255,220); // #20e0ff cyan (next stage walk)
                if(seg.dashed){
                    const float dx=b.x-a.x,dy=b.y-a.y;
                    const float len=hypotf(dx,dy);
                    if(len<1.f)continue;
                    const float ux=dx/len,uy=dy/len;
                    const float dashLen=7.f,gapLen=5.f;
                    for(float t=0.f;t<len;t+=dashLen+gapLen){
                        const float tEnd=fminf(t+dashLen,len);
                        const ImVec2 p0{a.x+ux*t,a.y+uy*t};
                        const ImVec2 p1{a.x+ux*tEnd,a.y+uy*tEnd};
                        d->AddLine(p0,p1,darkOutline,4.5f);
                        d->AddLine(p0,p1,col,2.25f);
                    }
                }else{
                    d->AddLine(a,b,darkOutline,4.5f);
                    d->AddLine(a,b,col,2.25f);
                }
            }
        }
        // 2. Draw Minigame ESP rings + center dots + labels (Ported from Windows overlay.html)
        if(_model.enabled[vt::Esp]){
            std::vector<ImVec2> ringPts;
            ringPts.reserve(16);
            for(const auto&p:_backend->esp()){
                ImU32 col=IM_COL32(53,255,101,245); // #35ff65 green
                if(p.colorKind==1)col=IM_COL32(255,77,77,235); // #ff4d4d red
                else if(p.colorKind==2)col=IM_COL32(255,213,74,240); // #ffd54a yellow

                if(p.ring.size()>=3){
                    ringPts.clear();
                    for(const auto&pt:p.ring){
                        ringPts.push_back({pt.x*w,pt.y*h});
                    }
                    d->AddPolyline(ringPts.data(),int(ringPts.size()),darkOutline,ImDrawFlags_Closed,3.8f);
                    d->AddPolyline(ringPts.data(),int(ringPts.size()),col,ImDrawFlags_Closed,2.0f);
                }

                const ImVec2 center{p.x*w,p.y*h};
                if(center.x>=-32.f&&center.x<=w+32.f&&center.y>=-32.f&&center.y<=h+32.f){
                    d->AddCircleFilled(center,4.2f,darkOutline,12);
                    d->AddCircleFilled(center,2.8f,col,12);

                    if(!p.label.empty()){
                        char label[128];
                        if(p.showDistance&&p.distance>0.5f){
                            snprintf(label,sizeof(label),"%s (%.0fm)",p.label.c_str(),p.distance);
                        }else{
                            snprintf(label,sizeof(label),"%s",p.label.c_str());
                        }
                        const ImVec2 tsz=ImGui::CalcTextSize(label);
                        const ImVec2 tpos{center.x-tsz.x*0.5f,center.y-tsz.y-8.f};
                        d->AddRectFilled({tpos.x-4.f,tpos.y-2.f},{tpos.x+tsz.x+4.f,tpos.y+tsz.y+2.f},IM_COL32(8,12,18,195),4.f);
                        d->AddText(tpos,col,label);
                    }
                }
            }
        }
    }
    vt::ios::draw(_model,_actions);ImGui::Render();
    id<MTLRenderCommandEncoder>encoder=[command renderCommandEncoderWithDescriptor:pass];if(!encoder)return;
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),command,encoder);[encoder endEncoding];[command presentDrawable:drawable];[command commit];
}
-(void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size{}
-(void)dealloc{
    if(_backend){_backend->stop(_model);_backend.reset();}
    _metalView.delegate=nil;_metalView.touchOwner=nil;
    if(_context){ImGui::SetCurrentContext(_context);ImGui_ImplMetal_Shutdown();ImGui::DestroyContext(_context);_context=nullptr;}
}
@end

static VTMenuController* VTController;
static __weak UIView* VTHostView;
static __weak UIWindowScene* VTHostScene;
static __weak UIWindow* VTHostWindow;
static NSTimer* VTHostTimer;

static UIView* VTFindGameView(UIView*view,int depth){
    if(!view||depth>24||view.hidden||view.alpha<.01||!view.window||[view isKindOfClass:VTPassthroughView.class])return nil;
    Class unity=NSClassFromString(@"UnityView"),rendering=NSClassFromString(@"UnityRenderingView");
    const bool unityView=unity&&[view isKindOfClass:unity];
    if((rendering&&[view isKindOfClass:rendering])||(unityView&&[view.layer isKindOfClass:CAMetalLayer.class]))return view;
    for(UIView*child in view.subviews){UIView*found=VTFindGameView(child,depth+1);if(found)return found;}
    if([view.layer isKindOfClass:CAMetalLayer.class]||unityView)return view;
    return nil;
}

static void VTDetach(){
    if(!VTController)return;
    [VTController setRenderingActive:NO];
    [VTController.view removeFromSuperview];
    VTController=nil;VTHostView=nil;VTHostScene=nil;VTHostWindow=nil;
}

static void VTAttach(){
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{VTAttach();});return;}
    if(UIApplication.sharedApplication.applicationState==UIApplicationStateBackground)return;
    UIWindow*game=nil;UIView*unity=nil;
    for(UIScene*scene in UIApplication.sharedApplication.connectedScenes){
        if(![scene isKindOfClass:UIWindowScene.class])continue;
        if(scene.activationState!=UISceneActivationStateForegroundActive&&scene.activationState!=UISceneActivationStateForegroundInactive)continue;
        for(UIWindow*window in ((UIWindowScene*)scene).windows){
            if(window.hidden||window.alpha<.01||![window.rootViewController isViewLoaded])continue;
            UIView*found=VTFindGameView(window.rootViewController.view,0);
            if(!found)continue;
            if(!game||[window isKeyWindow]){game=window;unity=found;}
            if([window isKeyWindow])break;
        }
        if(game&&[game isKeyWindow])break;
    }
    if(!game||!unity)return;
    UIViewController*parent=game.rootViewController;
    if(!parent||![parent isViewLoaded])return;
    UIView*container=parent.view;
    if(!container)return;
    if(!CGAffineTransformIsIdentity(container.transform))container=game;
    if(container!=game&&container.window!=game)return;
    const CGSize size=container.bounds.size;
    if(size.width<=0||size.height<=0)return;
    if(VTController&&VTController.view.superview==container&&VTHostView==container&&VTHostWindow==game&&!container.hidden&&container.alpha>=.01){
        if(!CGRectEqualToRect(VTController.view.frame,container.bounds))VTController.view.frame=container.bounds;
        if(!CGRectEqualToRect(VTController.metalView.frame,VTController.view.bounds))VTController.metalView.frame=VTController.view.bounds;
        if(container.subviews.lastObject!=VTController.view)[container bringSubviewToFront:VTController.view];
    UIView* touchTarget = unity ? unity : container;
    if (touchTarget) {
        BOOL hasJumpRec = NO;
        for (UIGestureRecognizer* gr in touchTarget.gestureRecognizers) {
            if ([gr isKindOfClass:VTJumpGestureRecognizer.class]) {
                ((VTJumpGestureRecognizer*)gr).menuController = VTController;
                hasJumpRec = YES;
                break;
            }
        }
        if (!hasJumpRec) {
            VTJumpGestureRecognizer* jg = [[VTJumpGestureRecognizer alloc] init];
            jg.menuController = VTController;
            [touchTarget addGestureRecognizer:jg];
        }
    }
    if (container && container != touchTarget) {
        BOOL hasOnContainer = NO;
        for (UIGestureRecognizer* gr in container.gestureRecognizers) {
            if ([gr isKindOfClass:VTJumpGestureRecognizer.class]) {
                ((VTJumpGestureRecognizer*)gr).menuController = VTController;
                hasOnContainer = YES;
                break;
            }
        }
        if (!hasOnContainer) {
            VTJumpGestureRecognizer* jg = [[VTJumpGestureRecognizer alloc] init];
            jg.menuController = VTController;
            [container addGestureRecognizer:jg];
        }
    }
        if(VTController.view.hidden||VTController.metalView.paused){
            VTController.view.hidden=NO;
            VTController.metalView.hidden=NO;
            [VTController setRenderingActive:YES];
        }
        return;
    }
    VTDetach();
    VTController=[[VTMenuController alloc]initWithFrame:container.bounds];
    VTHostView=container;VTHostScene=game.windowScene;VTHostWindow=game;
    [container addSubview:VTController.view];
    [container bringSubviewToFront:VTController.view];
}

extern "C" __attribute__((visibility("default"))) void VTMenuStart(){dispatch_async(dispatch_get_main_queue(),^{VTAttach();});}
__attribute__((constructor)) static void VTBootstrap(){
    VTSpoofBundleIdentifier();
    dispatch_async(dispatch_get_main_queue(),^{
        NSNotificationCenter*c=NSNotificationCenter.defaultCenter;
        [c addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){VTAttach();}];
        [c addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){VTAttach();}];
        [c addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){[VTController setRenderingActive:NO];}];
        [c addObserverForName:UISceneDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){if(n.object==VTHostScene)VTDetach();}];
        [c addObserverForName:UIScreenCapturedDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*n){
            if(VTController)[VTController handleScreenCaptureChanged:UIScreen.mainScreen.isCaptured];
        }];
        VTHostTimer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(NSTimer*t){VTAttach();}];
        [[NSRunLoop mainRunLoop]addTimer:VTHostTimer forMode:NSRunLoopCommonModes];
        VTAttach();
    });
}
