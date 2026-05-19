#include "childThreads.h"
#include <spinscale/co/group.h>

namespace sscl::co::group_smoke {

[[gnu::used]] void instantiateGroupTemplate()
{
	using IntGroup = Group<LegViralInvoker<int>>;
	(void)sizeof(IntGroup);
}

} // namespace sscl::co::group_smoke
