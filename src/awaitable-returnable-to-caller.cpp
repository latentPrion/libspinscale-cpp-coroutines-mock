#include <coroutine>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

struct ProbeTask {
	struct promise_type {
		ProbeTask get_return_object() noexcept {
			return {};
		}

		std::suspend_never initial_suspend() noexcept {
			return {};
		}

		std::suspend_never final_suspend() noexcept {
			return {};
		}

		void return_void() noexcept {}

		void unhandled_exception() noexcept {
			std::terminate();
		}
	};
};

struct SharedState {
	int await_suspend_calls = 0;
};

struct TrackingAwaitable {
	std::shared_ptr<SharedState> state;
	std::string label;

	bool await_ready() const noexcept {
		return false;
	}

	bool await_suspend(std::coroutine_handle<> handle) const noexcept {
		++state->await_suspend_calls;
		std::cout << label << ": await_suspend(frame=" << handle.address()
		          << ", calls=" << state->await_suspend_calls << ")\n";
		return false;
	}

	int await_resume() const noexcept {
		return state->await_suspend_calls;
	}
};

static TrackingAwaitable pass_to_callee_and_return(TrackingAwaitable incoming) {
	std::cout << "callee saw awaitable labelled: " << incoming.label << "\n";
	return incoming;
}

static ProbeTask run_hypothesis_probe() {
	auto shared_state = std::make_shared<SharedState>();

	TrackingAwaitable original{shared_state, "original"};
	auto passed_back = pass_to_callee_and_return(original);

	const int first_result = co_await passed_back;
	std::cout << "first await result: " << first_result << "\n";

	const int second_result =
		co_await pass_to_callee_and_return(TrackingAwaitable{shared_state, "temporary"});
	std::cout << "second await result: " << second_result << "\n";
}

int main() {
	std::cout << "Hypothesis test: awaitables are objects that can be passed/returned.\n";
	run_hypothesis_probe();
	return 0;
}
