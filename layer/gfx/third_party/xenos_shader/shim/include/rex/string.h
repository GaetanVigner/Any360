// any360 shim for <rex/string.h> (ReXGlue string utilities over guest memory and UTF-8). Nothing of it is used by
// the shader translator sources that include it; the standard headers it brought in are kept.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
