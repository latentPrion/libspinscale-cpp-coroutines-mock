#include <iostream>
#include <stdexcept>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "childThreads.h"
#include "current_io_context.h"

thread_local boost::asio::io_context *tls_current_io_context = nullptr;

boost::asio::io_context &current_io_context()
{
	if (!tls_current_io_context) {
		throw std::runtime_error("TLS io_context is not set");
	}
	return *tls_current_io_context;
}

boost::asio::io_context mainIoContext;
boost::asio::io_context bodyIoContext;
boost::asio::io_context worldIoContext;
boost::asio::io_context legIoContext;

boost::asio::executor_work_guard<
	boost::asio::io_context::executor_type> bodyIoContextIdleDoesNotEndRun(
		bodyIoContext.get_executor());
boost::asio::executor_work_guard<
	boost::asio::io_context::executor_type> worldIoContextIdleDoesNotEndRun(
		worldIoContext.get_executor());
boost::asio::executor_work_guard<
	boost::asio::io_context::executor_type> legIoContextIdleDoesNotEndRun(
		legIoContext.get_executor());

namespace {

void runNamedContextThreadLoop(
	boost::asio::io_context &context,
	bool &keepLooping,
	const char *contextName)
{
	tls_current_io_context = &context;

	std::cout << __func__ << " (" << contextName << "): My TID is: "
		<< std::this_thread::get_id() << ".\n";

	for (keepLooping = true; keepLooping;) {
		bool send_exception_ind = false;

		try {
			context.restart();
			context.run();
		} catch (const std::exception &exception) {
			send_exception_ind = true;
			std::cerr << contextName << ": Exception occurred: " << exception.what() << "\n";
		} catch (...) {
			send_exception_ind = true;
			std::cerr << contextName << ": Unknown exception occurred\n";
		}

		if (send_exception_ind) {
			std::cerr << contextName << ": loop exception hook stand-in\n";
		}
	}
	tls_current_io_context = nullptr;
}

}

void bodyThreadEntry(bool &body_keep_looping)
{
	runNamedContextThreadLoop(bodyIoContext, body_keep_looping, "body");
}

void worldThreadEntry(bool &world_keep_looping)
{
	runNamedContextThreadLoop(worldIoContext, world_keep_looping, "world");
}

void legThreadEntry(bool &leg_keep_looping)
{
	runNamedContextThreadLoop(legIoContext, leg_keep_looping, "leg");
}
