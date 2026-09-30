// SPDX-License-Identifier: MIT
// Read-only system inventory. No private selector invocation or driver loading.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
NSString *systemString(const char *key) {
    size_t length = 0;
    if (sysctlbyname(key, nullptr, &length, nullptr, 0) || !length || length > 4096) return @"unavailable";
    NSMutableData *bytes = [NSMutableData dataWithLength:length];
    if (sysctlbyname(key, bytes.mutableBytes, &length, nullptr, 0)) return @"unavailable";
    const char *text = static_cast<const char *>(bytes.bytes);
    size_t count = 0; while (count < length && text[count]) ++count;
    return [[NSString alloc] initWithBytes:text length:count encoding:NSUTF8StringEncoding] ?: @"unavailable";
}
id registryValue(io_registry_entry_t entry, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!value) return NSNull.null;
    id object = CFBridgingRelease(value);
    if ([object isKindOfClass:NSString.class] || [object isKindOfClass:NSNumber.class]) return object;
    if ([object isKindOfClass:NSData.class]) {
        NSData *data = object;
        const auto *bytes = static_cast<const uint8_t *>(data.bytes);
        NSMutableString *hex = [NSMutableString string];
        for (NSUInteger i = 0; i < MIN(data.length, 256UL); ++i) [hex appendFormat:@"%02x", bytes[i]];
        NSMutableDictionary *record = [@{@"type":@"data", @"bytes":@(data.length), @"hex":hex,
            @"truncated":@(data.length > 256)} mutableCopy];
        if (data.length == 4) record[@"uint32LittleEndian"] = @(uint32_t(bytes[0]) |
            (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24));
        return record;
    }
    return @{@"type":NSStringFromClass([object class]), @"valueOmitted":@YES};
}
NSDictionary *registryRecord(io_registry_entry_t entry) {
    uint64_t identity = 0;
    const kern_return_t idStatus = IORegistryEntryGetRegistryEntryID(entry, &identity);
    io_name_t className = {}; io_string_t path = {};
    const kern_return_t classStatus = IOObjectGetClass(entry, className);
    const kern_return_t pathStatus = IORegistryEntryGetPath(entry, kIOServicePlane, path);
    NSMutableDictionary *properties = [NSMutableDictionary dictionary];
    NSArray<NSString *> *keys = @[@"vendor-id", @"device-id", @"subsystem-vendor-id", @"subsystem-id",
        @"class-code", @"revision-id", @"MetalPluginName", @"MetalPluginClassName", @"IOGLBundleName",
        @"IOAccelTypes", @"IOAccelRevision", @"IOAccelIndex", @"IOAccelDisplayPipeCapabilities",
        @"IOClass", @"IOBundleIdentifier", @"MellowPhysicalVendorID", @"MellowPhysicalDeviceID",
        @"MellowPhysicalBDF", @"MellowPhysicalIdentitySource"];
    for (NSString *key in keys) properties[key] = registryValue(entry, (__bridge CFStringRef)key);
    return @{@"registryID":idStatus == KERN_SUCCESS ? [NSString stringWithFormat:@"%llu", (unsigned long long)identity] : @"unavailable",
        @"class":classStatus == KERN_SUCCESS ? @(className) : @"unavailable",
        @"path":pathStatus == KERN_SUCCESS ? @(path) : @"unavailable", @"properties":properties};
}
NSArray *providerChain(uint64_t identity) {
    io_registry_entry_t entry = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(identity));
    NSMutableArray *records = [NSMutableArray array];
    for (unsigned depth = 0; entry && depth < 16; ++depth) {
        [records addObject:registryRecord(entry)];
        io_registry_entry_t parent = 0;
        const kern_return_t status = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry); entry = status == KERN_SUCCESS ? parent : 0;
    }
    if (entry) IOObjectRelease(entry);
    return records;
}
NSDictionary *methodInventory(Class cls, unsigned &total) {
    NSMutableArray *selectors = [NSMutableArray array];
    unsigned count = 0; Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; methods && i < count && total < 4096; ++i, ++total) {
        Method method = methods[i]; const char *encoding = method_getTypeEncoding(method);
        const unsigned argumentCount = method_getNumberOfArguments(method);
        NSMutableArray *arguments = [NSMutableArray array];
        for (unsigned argument = 0; argument < argumentCount && argument < 64; ++argument) {
            char *type = method_copyArgumentType(method, argument);
            [arguments addObject:type ? @(type) : @"unavailable"]; free(type);
        }
        char *returnType = method_copyReturnType(method);
        Dl_info image = {}; const bool located = dladdr(reinterpret_cast<const void *>(method_getImplementation(method)), &image) != 0;
        [selectors addObject:@{@"selector":NSStringFromSelector(method_getName(method)),
            @"typeEncoding":encoding ? @(encoding) : @"unavailable",
            @"returnType":returnType ? @(returnType) : @"unavailable",
            @"argumentCount":@(argumentCount), @"argumentTypes":arguments,
            @"argumentTypesTruncated":@(arguments.count != argumentCount),
            @"image":located && image.dli_fname ? @(image.dli_fname) : @"unavailable",
            @"symbol":located && image.dli_sname ? @(image.dli_sname) : @"unavailable"}];
        free(returnType);
    }
    free(methods);
    return @{@"methodCount":@(count), @"methods":selectors, @"truncated":@(selectors.count != count)};
}
NSArray *runtimeClasses(id device, BOOL *chainTruncated) {
    NSMutableArray *classes = [NSMutableArray array];
    unsigned total = 0;
    Class cls = object_getClass(device);
    for (; cls && cls != NSObject.class && classes.count < 16; cls = class_getSuperclass(cls)) {
        NSDictionary *instanceMethods = methodInventory(cls, total);
        NSDictionary *classMethods = methodInventory(object_getClass(cls), total);
        unsigned ivarCount = 0; Ivar *ivars = class_copyIvarList(cls, &ivarCount);
        NSMutableArray *layout = [NSMutableArray array];
        for (unsigned i = 0; ivars && i < ivarCount && i < 256; ++i) {
            const char *name = ivar_getName(ivars[i]);
            const char *encoding = ivar_getTypeEncoding(ivars[i]);
            [layout addObject:@{@"name":name ? @(name) : @"unavailable",
                @"typeEncoding":encoding ? @(encoding) : @"unavailable", @"offset":@(ivar_getOffset(ivars[i]))}];
        }
        free(ivars);
        NSBundle *bundle = [NSBundle bundleForClass:cls];
        const char *image = class_getImageName(cls);
        [classes addObject:@{@"class":NSStringFromClass(cls), @"image":image ? @(image) : @"unavailable",
            // Keep the original instance-method fields for existing readers.
            @"methodCount":instanceMethods[@"methodCount"], @"methods":instanceMethods[@"methods"],
            @"truncated":instanceMethods[@"truncated"], @"classMethods":classMethods,
            @"instanceSize":@(class_getInstanceSize(cls)), @"ivarCount":@(ivarCount), @"ivars":layout,
            @"ivarsTruncated":@(layout.count != ivarCount), @"ivarValuesRead":@NO,
            @"bundleIdentifier":bundle.bundleIdentifier ?: @"unavailable",
            @"bundleVersion":[bundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"unavailable",
            @"bundleExecutable":bundle.executablePath ?: @"unavailable"}];
    }
    if (chainTruncated) *chainTruncated = cls && cls != NSObject.class;
    return classes;
}
NSArray *services(NSString *className) {
    io_iterator_t iterator = 0;
    const kern_return_t status = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching(className.UTF8String), &iterator);
    if (status != KERN_SUCCESS) return @[@{@"matchingClass":className, @"error":@(status)}];
    NSMutableArray *records = [NSMutableArray array];
    io_service_t entry = 0;
    while (records.count < 256 && (entry = IOIteratorNext(iterator))) {
        [records addObject:registryRecord(entry)]; IOObjectRelease(entry);
    }
    IOObjectRelease(iterator);
    return records;
}
}

int main(int argc, const char **argv) {
    (void)argv;
    @autoreleasepool {
        if (argc != 1) { fprintf(stderr, "Usage: metal-inventory (JSON on stdout)\n"); return 2; }
        NSMutableArray *devices = [NSMutableArray array];
        for (id<MTLDevice> device in MTLCopyAllDevices()) {
            BOOL chainTruncated = NO;
            NSArray *classes = runtimeClasses(device, &chainTruncated);
            [devices addObject:@{@"name":device.name,
                @"registryID":[NSString stringWithFormat:@"%llu", (unsigned long long)device.registryID],
                @"providerChain":providerChain(device.registryID), @"runtimeClasses":classes,
                @"runtimeClassChainTruncated":@(chainTruncated),
                @"computeSubmitted":@NO, @"renderSubmitted":@NO}];
        }
        CGDirectDisplayID displayIDs[64] = {}; uint32_t displayCount = 0;
        const CGError displayStatus = CGGetActiveDisplayList(64, displayIDs, &displayCount);
        NSMutableArray *displays = [NSMutableArray array];
        if (displayStatus == kCGErrorSuccess) for (uint32_t i = 0; i < displayCount; ++i) {
            id<MTLDevice> device = CGDirectDisplayCopyCurrentMetalDevice(displayIDs[i]);
            [displays addObject:@{@"displayID":@(displayIDs[i]),
                @"metalRegistryID":device ? [NSString stringWithFormat:@"%llu", (unsigned long long)device.registryID] : @"unavailable",
                @"metalName":device ? device.name : @"unavailable", @"scanoutVerified":@NO}];
        }
        NSDictionary *report = @{@"schemaVersion":@1, @"scope":@"read-only-system-metal-and-ioregistry-inventory",
            @"osVersion":systemString("kern.osproductversion"), @"osBuild":systemString("kern.osversion"),
            @"architecture":systemString("hw.machine"), @"devices":devices,
            @"pciServices":services(@"IOPCIDevice"), @"acceleratorServices":services(@"IOAccelerator"),
            @"gpuServices":services(@"IOGPU"), @"displays":displays, @"displayQueryStatus":@(displayStatus),
            @"driverLoadedByProbe":@NO, @"privateSelectorsInvoked":@NO, @"privateAbiVerified":@NO,
            @"explicitDriverLoadingRequested":@NO, @"normalMetalEnumerationMayInitializeProvider":@YES,
            @"implicitProviderInitializationObserved":@NO,
            @"registryPhysicalIdentityTrusted":@NO, @"gpuExecutionVerified":@NO,
            @"systemWindowServerAccelerationVerified":@NO};
        NSError *error = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
        if (!json) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        [[NSFileHandle fileHandleWithStandardOutput] writeData:json];
        [[NSFileHandle fileHandleWithStandardOutput] writeData:[@"\n" dataUsingEncoding:NSUTF8StringEncoding]];
        return 0;
    }
}
