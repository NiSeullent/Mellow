// SPDX-License-Identifier: MIT
// Read-only metadata capture on a real macOS host. No plugin registration,
// method replacement, guessed structure dereference, GPU submission or kext load.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static NSString *Text(const char *value) {
    return value ? ([NSString stringWithUTF8String:value] ?: @"<non-UTF8>") : @"";
}
static NSDictionary *DescribeClass(Class klass) {
    NSArray<NSString *> *selectors = @[@"initWithAcceleratorPort:",
        @"newCommandQueueWithDescriptor:", @"newLibraryWithData:error:",
        @"_reserveKernelCommandBufferSpace:", @"getCurrentKernelCommandBufferPointer:end:",
        @"beginSegment:", @"endCurrentSegment", @"serializeToURL:error:"];
    NSMutableArray *methods = [NSMutableArray array];
    for (NSString *name in selectors) {
        SEL selector = NSSelectorFromString(name);
        Method method = class_getInstanceMethod(klass, selector);
        if (!method) { [methods addObject:@{@"selector": name, @"present": @NO}]; continue; }
        // Locate the actual declaring class, rather than attributing inherited
        // selector strings to every subclass of the device/queue/buffer hierarchy.
        Class declaration = Nil;
        for (Class candidate = klass; candidate && !declaration; candidate = class_getSuperclass(candidate)) {
            unsigned count = 0;
            Method *own = class_copyMethodList(candidate, &count);
            for (unsigned i = 0; i < count; ++i)
                if (method_getName(own[i]) == selector) { declaration = candidate; break; }
            free(own);
        }
        [methods addObject:@{@"selector": name, @"present": @YES,
            @"declaring_class": Text(declaration ? class_getName(declaration) : nullptr),
            @"type_encoding": Text(method_getTypeEncoding(method)),
            @"argument_count": @(method_getNumberOfArguments(method))}];
    }
    return @{@"name": Text(class_getName(klass)),
             @"superclass": Text(class_getSuperclass(klass) ? class_getName(class_getSuperclass(klass)) : nullptr),
             @"instance_size": @(class_getInstanceSize(klass)),
             @"image": Text(class_getImageName(klass)), @"methods": methods};
}
static NSString *ImageUUID(const mach_header *header) {
    if (!header || header->magic != MH_MAGIC_64) return @"";
    const auto *wide = reinterpret_cast<const mach_header_64 *>(header);
    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(wide + 1);
    size_t remaining = wide->sizeofcmds;
    for (uint32_t i = 0; i < wide->ncmds; ++i) {
        if (remaining < sizeof(load_command)) return @"";
        const auto *command = reinterpret_cast<const load_command *>(cursor);
        if (command->cmdsize < sizeof(load_command) || command->cmdsize > remaining) return @"";
        if (command->cmd == LC_UUID && command->cmdsize >= sizeof(uuid_command)) {
            const auto *uuid = reinterpret_cast<const uuid_command *>(command);
            NSUUID *value = [[NSUUID alloc] initWithUUIDBytes:uuid->uuid];
            return value.UUIDString;
        }
        remaining -= command->cmdsize;
        cursor += command->cmdsize;
    }
    return @"";
}
int main() {
    @autoreleasepool {
      @try {
        // Enumerate Apple's existing devices only for the ABI inventory. These
        // are never substituted for Mellow's explicit adapter factories.
        NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
        NSMutableArray *deviceRecords = [NSMutableArray array];
        NSMutableArray<NSString *> *classNames = [NSMutableArray arrayWithArray:@[
            @"MTLIOAccelDevice", @"MTLIOAccelBuffer", @"MTLIOAccelCommandQueue",
            @"MTLIOAccelCommandBuffer", @"MTLIGAccelDevice"]];
        for (id<MTLDevice> device in devices) {
            NSString *className = Text(object_getClassName(device));
            [deviceRecords addObject:@{@"name": device.name, @"registry_id": @(device.registryID),
                @"objc_class": className, @"origin": @"existing-system-device"}];
            if (![classNames containsObject:className]) [classNames addObject:className];
        }
        NSMutableArray *classes = [NSMutableArray array];
        for (NSString *name in classNames) {
            Class klass = NSClassFromString(name);
            [classes addObject:klass ? DescribeClass(klass) : @{@"name": name, @"loaded": @NO}];
        }
        NSMutableArray *images = [NSMutableArray array];
        for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
            NSString *path = Text(_dyld_get_image_name(i));
            if ([path containsString:@"Metal"] || [path containsString:@"IOAccel"] ||
                [path containsString:@"IOSurface"] || [path containsString:@"CoreDisplay"])
                [images addObject:@{@"path": path, @"uuid": ImageUUID(_dyld_get_image_header(i))}];
        }
        NSDictionary *record = @{
            @"schema": @1, @"captured_at": [NSISO8601DateFormatter stringFromDate:[NSDate date]
                timeZone:[NSTimeZone timeZoneForSecondsFromGMT:0] formatOptions:NSISO8601DateFormatWithInternetDateTime],
            @"os_version": NSProcessInfo.processInfo.operatingSystemVersionString,
            @"system_devices": deviceRecords, @"classes": classes, @"images": images,
            @"objc_metadata_observed": @YES, @"kernel_vtables_verified": @NO,
            @"private_plugin_abi_verified": @NO, @"mellow_system_device_registered": @NO,
            @"unsupported_gpu_execution_verified": @NO,
            @"scope": @"Loaded Objective-C metadata and existing system Metal device enumeration only"
        };
        NSError *error = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:&error];
        if (!json) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
        return 0;
      } @catch (NSException *exception) {
        fprintf(stderr, "%s: %s\n", exception.name.UTF8String, exception.reason.UTF8String); return 2;
      }
    }
}
