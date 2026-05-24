#include <iostream>
#include <pthread.h>

#include <componentThread.h>
#include <marionette/marionette.h>
#include <spinscale/componentThread.h>
#include <spinscale/runtime.h>

int main(int argc, char *argv[], char *envp[])
{
	sscl::ComponentThread::setPuppeteerThreadId(ctest::CtestThreadId::MRNTT);
	sscl::ComponentThread::setPuppeteerThread(ctest::mrntt::thread);
	pthread_setname_np(pthread_self(), "ctest:CRT:main");

	std::cout << "CRT:" << __func__ << ": about to JOLT Mrntt with cmdline args\n";

	ctest::mrntt::thread->getIoService().post(
		[argc, argv, envp]()
		{
			std::cout << "Mrntt:" << __func__ << ":JOLTED: setting cmdline args\n";
			sscl::CrtCommandLineArgs::set(argc, argv, envp);
			ctest::mrntt::thread->getIoService().stop();
		});

	ctest::mrntt::thread->thread.join();

	std::cout << "CRT:" << __func__ << ": Mrntt exited with code '"
		<< sscl::pptr::exitCode << "'\n";

	return sscl::pptr::exitCode;
}
