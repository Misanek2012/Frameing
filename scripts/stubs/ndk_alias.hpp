// Header-check shim (not part of the mod): the generated Android64 headers
// specialise std::__ndk1 templates; alias that onto the host libc++ namespace.
// syntax-check shim: map NDK libc++ ABI namespace onto host libc++
#include <cstring>
namespace std { namespace __ndk1 = __1; }
