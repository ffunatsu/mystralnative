#pragma once

#include "mystral/js/engine.h"

namespace mystral {
namespace js {

#if defined(MYSTRAL_JS_V8)
bool initV8FfiBindings(Engine* engine);
#endif

}  // namespace js
}  // namespace mystral
