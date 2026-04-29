#ifndef PROMISE_CHAIN_LINK_H
#define PROMISE_CHAIN_LINK_H

#include <functional>
#include <list>

class CoQutex;

/**
 * Non-template base for coroutine promises participating in a logical
 * promise chain (analogous to libspinscale AsynchronousContinuationChainLink).
 * A future deadlock detector can walk callerPromiseChainLink() without
 * knowing concrete promise_type.
 */
class PromiseChainLink
{
public:
	virtual ~PromiseChainLink() = default;

	/** Reserved for deadlock detection: link toward the caller / outer coroutine. */
	virtual const PromiseChainLink *callerPromiseChainLink() const noexcept
		{ return nullptr; }
	virtual PromiseChainLink *callerPromiseChainLink() noexcept
		{ return nullptr; }

	void addAcquiredLock(CoQutex &coQutex) noexcept
		{ acquiredLocks.emplace_back(std::ref(coQutex)); }

	virtual void removeAcquiredLock(CoQutex &coQutex) noexcept = 0;

protected:
	void eraseFirstMatchingAcquiredLock(CoQutex &coQutex) noexcept
	{
		for (auto it = acquiredLocks.begin(); it != acquiredLocks.end(); ++it) {
			if (&it->get() == &coQutex) {
				acquiredLocks.erase(it);
				return;
			}
		}
	}

	std::list<std::reference_wrapper<CoQutex>> acquiredLocks;
};

#endif // PROMISE_CHAIN_LINK_H
