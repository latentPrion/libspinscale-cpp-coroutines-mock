#ifndef SHARED_RESOURCE_GROUP_H
#define SHARED_RESOURCE_GROUP_H

namespace sscl::co {

template <typename LockType, typename ResourceType>
class SharedResourceGroup
{
public:
	SharedResourceGroup() = default;
	~SharedResourceGroup() = default;

	LockType lock;
	ResourceType rsrc;
};

} // namespace sscl::co

#endif // SHARED_RESOURCE_GROUP_H
