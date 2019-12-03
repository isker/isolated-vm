#include "./executor.h"
#include "./environment.h"
#include "../lib/timer.h"

namespace ivm {

/**
 * Executor implementation
 */
Executor::Executor(IsolateEnvironment& env) :
	env{env},
	default_executor{*(current_executor == nullptr ? (current_executor = this) : current_executor)},
	default_thread{&default_executor == this ? std::this_thread::get_id() : default_executor.default_thread} {}

thread_local Executor* Executor::current_executor = nullptr;
thread_local Executor::CpuTimer* Executor::cpu_timer_thread = nullptr;

/**
 * CpuTimer implementation
 */
Executor::CpuTimer::CpuTimer(Executor& executor) :
		executor{executor}, last{cpu_timer_thread}, time{Now()} {
	cpu_timer_thread = this;
	std::lock_guard<std::mutex> lock{executor.timer_mutex};
	assert(executor.cpu_timer == nullptr);
	executor.cpu_timer = this;
}

Executor::CpuTimer::~CpuTimer() {
	cpu_timer_thread = last;
	std::lock_guard<std::mutex> lock{executor.timer_mutex};
	executor.cpu_time += Now() - time;
	assert(executor.cpu_timer == this);
	executor.cpu_timer = nullptr;
}

auto Executor::CpuTimer::Delta(const std::lock_guard<std::mutex>& /* lock */) const -> std::chrono::nanoseconds {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(Now() - time);
}

void Executor::CpuTimer::Pause() {
	std::lock_guard<std::mutex> lock{executor.timer_mutex};
	executor.cpu_time += Now() - time;
	assert(executor.cpu_timer == this);
	executor.cpu_timer = nullptr;
	timer_t::pause(executor.env.timer_holder);
}

void Executor::CpuTimer::Resume() {
	std::lock_guard<std::mutex> lock{executor.timer_mutex};
	time = Now();
	assert(executor.cpu_timer == nullptr);
	executor.cpu_timer = this;
	timer_t::resume(executor.env.timer_holder);
}

#if USE_CLOCK_THREAD_CPUTIME_ID
auto Executor::CpuTimer::Now() -> TimePoint {
	timespec ts{};
	assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0);
	return TimePoint{std::chrono::duration_cast<std::chrono::system_clock::duration>{
		std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}
	}};
}
#else
auto Executor::CpuTimer::Now() -> TimePoint{
	return std::chrono::steady_clock::now();
}
#endif

/**
 * WallTimer implementation
 */
Executor::WallTimer::WallTimer(Executor& executor) :
		executor{executor}, cpu_timer{cpu_timer_thread} {
	// Pause current CPU timer which may not belong to this isolate
	if (cpu_timer != nullptr) {
		cpu_timer->Pause();
	}
	// Maybe start wall timer
	if (executor.wall_timer == nullptr) {
		std::lock_guard<std::mutex> lock{executor.timer_mutex};
		executor.wall_timer = this;
		time = std::chrono::steady_clock::now();
	}
}

Executor::WallTimer::~WallTimer() {
	// Resume old CPU timer
	if (cpu_timer != nullptr) {
		cpu_timer->Resume();
	}
	// Maybe update wall time
	if (executor.wall_timer == this) {
		std::lock_guard<std::mutex> lock{executor.timer_mutex};
		executor.wall_timer = nullptr;
		executor.wall_time += std::chrono::steady_clock::now() - time;
	}
}

auto Executor::WallTimer::Delta(const std::lock_guard<std::mutex>& /* lock */) const -> std::chrono::nanoseconds {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - time);
}

/**
 * Scope ctor
 */
Executor::Scope::Scope(IsolateEnvironment& env) : last{current_executor} {
	current_executor = &env.executor;
}

/**
 * Lock implementation
 */
Executor::Lock::Lock(IsolateEnvironment& env) :
	scope{env},
	wall_timer{env.executor},
	locker{env.isolate},
	cpu_timer{env.executor},
	isolate_scope{env.isolate},
	handle_scope{env.isolate} {}

/**
 * Unlock implementation
 */
Executor::Unlock::Unlock(IsolateEnvironment& env) : pause_scope{env.executor.cpu_timer}, unlocker{env.isolate} {}

} // namespace ivm
