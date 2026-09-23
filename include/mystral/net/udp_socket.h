#pragma once

#include "mystral/js/engine.h"

namespace mystral {
namespace net {

bool initUDPBindings(js::Engine* engine);
void initUDPNetworking();
void processUDPEvents();
bool hasActiveUDPSockets();
void shutdownUDPNetworking();

}  // namespace net
}  // namespace mystral