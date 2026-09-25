// Host-independent failure/lifetime checks; never GPU execution evidence.
#include "../Runtime/OpenGLProvider.hpp"
#include <iostream>
#include <stdexcept>
using namespace MellowRT;
static void check(bool value) { if (!value) throw std::runtime_error("Provider rejection contract failed"); }
int main() {
    OpenGLProvider provider;
    std::string error;
    check(!provider.initialize(error, false, 0, 48) && error.find("size") != std::string::npos);
    check(!provider.initialize(error, false, 2049, 48));
    auto info = provider.deviceInfo();
    check(!info.acceleratedPixelFormat && !info.physicalPciIdentityVerified && info.renderer.empty());
    check(!provider.compile("invalid", "invalid", error) && !error.empty());
    OpenGLFrame frame;
    frame.renderSubmitted = true; frame.rgba = {1, 2, 3};
    check(!provider.render({}, {}, frame) && !frame.renderSubmitted && frame.rgba.empty() && !frame.error.empty());
    provider.invalidateSession();
    check(provider.pipelineBuildCount() == 0);
#if defined(__APPLE__)
    check(!provider.initialize(error, true) && error.find("offscreen") != std::string::npos);
#elif !defined(_WIN32)
    check(!provider.initialize(error) && error.find("unsupported") != std::string::npos);
    check(!provider.initialize(error) && error.find("unsupported") != std::string::npos);
#endif
    check(!provider.deviceInfo().acceleratedPixelFormat);
    std::cout << "PASS provider failure/lifetime contracts (no GPU work)\n";
}
