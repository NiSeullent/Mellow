// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mellow contributors. See LICENSE and NOTICE.
#pragma once
#include <cstddef>

namespace MellowAppleRender {
// The public descriptor array has no exported capacity symbol. This adapter
// expects four counter attachments, separate from eight color attachments. CI
// checks the public descriptor's index boundary on its recorded macOS host.
constexpr std::size_t CounterAttachmentCount = 4;
}
