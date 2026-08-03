
// RED: stub — resource meter not implemented yet (zeros).

#include "bench/resources.h"

#include <sys/resource.h>

namespace bench {

void ResourceMeter::start() noexcept { started_ = true; }
void ResourceMeter::stop() noexcept { started_ = false; }

} // namespace bench
