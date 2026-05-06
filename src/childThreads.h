#ifndef CHILD_THREADS_H
#define CHILD_THREADS_H

#include <boost/asio/io_context.hpp>

#include "invokers.h"
#include "promises.h"

extern thread_local boost::asio::io_context *tls_current_io_context;

extern boost::asio::io_context mainIoContext;

/* Body thread */
extern boost::asio::io_context bodyIoContext;
struct BodyThreadTag
{
	static boost::asio::io_context &io_context() noexcept
		{ return bodyIoContext; }
};
template <typename T>
using BodyPostingPromise = TaggedPostingPromise<T, BodyThreadTag>;
using BodyNonViralNonSuspendingInvoker = NonViralNonSuspendingInvoker<BodyPostingPromise>;
template <typename T>
using BodyViralInvoker = ViralSuspendingInvoker<BodyPostingPromise, T>;

/* World thread */
extern boost::asio::io_context worldIoContext;
struct WorldThreadTag
{
	static boost::asio::io_context &io_context() noexcept
		{ return worldIoContext; }
};
template <typename T>
using WorldPostingPromise = TaggedPostingPromise<T, WorldThreadTag>;
using WorldNonViralNonSuspendingInvoker = NonViralNonSuspendingInvoker<WorldPostingPromise>;
template <typename T>
using WorldViralInvoker = ViralSuspendingInvoker<WorldPostingPromise, T>;

/* Leg thread */
extern boost::asio::io_context legIoContext;
struct LegThreadTag
{
	static boost::asio::io_context &io_context() noexcept
		{ return legIoContext; }
};
template <typename T>
using LegPostingPromise = TaggedPostingPromise<T, LegThreadTag>;
using LegNonViralNonSuspendingInvoker = NonViralNonSuspendingInvoker<LegPostingPromise>;
template <typename T>
using LegViralInvoker = ViralSuspendingInvoker<LegPostingPromise, T>;

void bodyThreadEntry(bool &body_keep_looping);

void worldThreadEntry(bool &world_keep_looping);

void legThreadEntry(bool &leg_keep_looping);

#endif // CHILD_THREADS_H
