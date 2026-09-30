// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
// Compile/link consumer only. Taking exported function addresses creates no GPU.
#import <MellowAppleUserspace/MellowAppleUserspace.h>
decltype(&MellowCreateDevice) volatile mellowFrameworkCompute = &MellowCreateDevice;
decltype(&MellowCreateRenderDevice) volatile mellowFrameworkRender = &MellowCreateRenderDevice;
int main() { return mellowFrameworkCompute && mellowFrameworkRender ? 0 : 1; }
