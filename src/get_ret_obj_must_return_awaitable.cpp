#include <coroutine>
#include <exception>
#include <iostream>

struct FireAndForget {
	struct promise_type {
		FireAndForget get_return_object() noexcept {
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

struct Awaitable {
	struct promise_type {
		Awaitable get_return_object() noexcept {
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

	bool await_ready() const noexcept {
		return true;
	}

	void await_suspend(std::coroutine_handle<>) const noexcept {}

	void await_resume() const noexcept {
		std::cout << "awaitable resumed\n";
	}
};

static FireAndForget sync_coroutine_invoker(Awaitable operand) {
	co_await operand;
}

static Awaitable sync_coroutine_factory() {
	co_return;
}

int main() {
	Awaitable operand = sync_coroutine_factory();
	sync_coroutine_invoker(operand);
	return 0;
}
