#include <coroutine>

struct AwaitableReturnableToCaller
{
	struct promise_type
	{
		AwaitableReturnableToCaller get_return_object() noexcept { return {}; }
		std::suspend_never initial_suspend() noexcept { return {}; }
		std::suspend_never final_suspend() noexcept { return {}; }
		void return_void() noexcept {}
		void unhandled_exception() noexcept { std::terminate(); }
	};
};

int main()
{
	
	return 0;
}
