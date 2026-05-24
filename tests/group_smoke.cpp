#include <leg/leg.h>
#include <spinscale/co/group.h>

namespace sscl::co::group_smoke {

[[gnu::used]] void instantiateGroupTemplate()
{
	using IntGroup = Group;
	(void)sizeof(IntGroup);
}

} // namespace sscl::co::group_smoke

int main()
{
	return 0;
}
