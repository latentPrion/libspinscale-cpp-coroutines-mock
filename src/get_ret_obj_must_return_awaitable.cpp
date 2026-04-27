#include <coroutine>
#include <exception>


class Awaitable
{
public:
	class promise_type
	{
	public:
		void get_return_object() noexcept { return; }
		std::suspend_never initial_suspend() noexcept { return {}; }
		std::suspend_never final_suspend() noexcept { return {}; }
		void return_void() noexcept {}
		void unhandled_exception() noexcept { std::terminate(); }
	};
};

Awaitable syncCoroFactory()
{
	return Awaitable{};
}

int main()
{
	Awaitable awaitable = syncCoroFactory();
	syncCoroInvoker(awaitable);
    return 0;
}
