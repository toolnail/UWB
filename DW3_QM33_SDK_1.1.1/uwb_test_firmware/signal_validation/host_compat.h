/* Adapt the compiler intrinsic only; keep the vendor fixed-point math intact. */
#ifdef _MSC_VER
#include <intrin.h>
static unsigned host_clz(unsigned value)
{
    unsigned long index;
    return _BitScanReverse(&index, value) ? 31U - index : 32U;
}
#define __builtin_clz(value) host_clz(value)
#endif
