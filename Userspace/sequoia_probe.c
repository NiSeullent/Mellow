// SPDX-License-Identifier: MIT
// Public IOKit client. This checks discovery, not GPU execution or Metal support.
#include "../Mellow/TahoeDiagnosticABI.h"
#include "../Mellow/XeProbeABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { fputs("usage: sequoia-probe (read-only; run as administrator)\n",stderr); return 2; }
    if (geteuid()!=0) { fputs("Administrator privileges required by Mellow's diagnostic client\n",stderr); return 2; }
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(MELLOW_DIAG_SERVICE));
    if (!service) { fputs("Diagnostic service absent: check -mellowdiag, Darwin 24/25, PCI 8086:7D41 and D0; this is not a Metal test\n",stderr); return 3; }
    io_connect_t connection=IO_OBJECT_NULL;
    kern_return_t result=IOServiceOpen(service,mach_task_self(),MELLOW_DIAG_CONNECT_TYPE,&connection);
    IOObjectRelease(service);
    if (result!=KERN_SUCCESS) { fprintf(stderr,"IOServiceOpen failed: 0x%x\n",result); return 4; }
    MellowXeProbeRequest request={MELLOW_XE_PROBE_VERSION,sizeof(request),0,{0,0}};
    do { arc4random_buf(&request.nonce,sizeof(request.nonce)); } while (!request.nonce);
    MellowXeProbeReply reply;
    memset(&reply,0,sizeof(reply));
    size_t size=sizeof(reply);
    result=IOConnectCallStructMethod(connection,MELLOW_XE_PROBE_SELECTOR,&request,sizeof(request),&reply,&size);
    const kern_return_t closed=IOServiceClose(connection);
    if (result!=KERN_SUCCESS || closed!=KERN_SUCCESS) { fprintf(stderr,"Probe/close failed: 0x%x / 0x%x\n",result,closed); return 5; }
    if (size!=sizeof(reply) || reply.size!=sizeof(reply) || reply.version!=MELLOW_XE_PROBE_VERSION ||
        reply.nonce!=request.nonce || reply.reserved0 || reply.reserved[0] || reply.reserved[1] ||
        reply.gpuSubmissionSupported || reply.metalSupported) {
        fputs("Malformed probe response or unsupported acceleration claim\n",stderr); return 6;
    }
    if (reply.status) { fprintf(stderr,"Read-only physical sample unavailable; status=%u; do not reuse earlier evidence\n",reply.status); return 7; }
    if ((reply.darwinMajor!=24 && reply.darwinMajor!=25) || reply.pciId!=0x7d418086 ||
        reply.bus!=0 || reply.slot!=2 || reply.function!=0 ||
        (reply.gmd>>22)!=12 || ((reply.gmd>>14)&255U)!=70 || !(reply.command&2U) ||
        (reply.pmcsr&3U) || reply.barBytes<0xd90 || !reply.pciRegistryId) {
        fputs("Sample is not the admitted physical Xe-LPG target\n",stderr); return 8;
    }
    printf("{\"schema\":\"mellow.physical-probe/1\",\"status\":\"discovery-only\","
           "\"darwin_major\":%u,\"nonce\":\"%016" PRIx64 "\",\"sampled_us\":%" PRIu64 ","
           "\"pci_registry_id\":%" PRIu64 ",\"pci_id\":\"%08x\",\"subsystem_id\":\"%08x\","
           "\"class_revision\":\"%08x\",\"bdf\":\"%02x:%02x.%x\",\"bar0_bytes\":%" PRIu64 ","
           "\"gmd_raw\":\"%08x\",\"gmd_architecture\":12,\"gmd_release\":70,"
           "\"target_samsung_board_matches\":%s,\"atomic_snapshot\":false,"
           "\"gpu_submission_supported\":false,\"metal_supported\":false}\n",
           reply.darwinMajor,reply.nonce,reply.sampledMicros,reply.pciRegistryId,
           reply.pciId,reply.subsystemId,reply.classRevision,reply.bus,reply.slot,reply.function,
           reply.barBytes,reply.gmd,reply.subsystemId==0xc906144d?"true":"false");
    return 0;
}
