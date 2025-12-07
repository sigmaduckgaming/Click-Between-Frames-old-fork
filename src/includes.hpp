#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <Geode/Geode.hpp>

using namespace geode::prelude;

using TimestampType = int64_t;
TimestampType getCurrentTimestamp();

enum GameAction : int {
	p1Jump = 0,
	p1Left = 1,
	p1Right = 2,
	p2Jump = 3,
	p2Left = 4,
	p2Right = 5
};

enum State : bool {
	Release = 0,
	Press = 1
};

struct InputEvent {
	TimestampType time;
	PlayerButton inputType;
	bool inputState;
	bool isPlayer1;
};

struct Step {
	InputEvent input;
	double deltaFactor;
	bool endStep;
};

// Lock-free ring buffer for high-performance input handling
template<typename T, size_t N>
class LockFreeQueue {
private:
	std::array<T, N> buffer;
	std::atomic<size_t> head{ 0 };
	std::atomic<size_t> tail{ 0 };

public:
	bool push(const T& item) {
		size_t current_tail = tail.load(std::memory_order_relaxed);
		size_t next_tail = (current_tail + 1) % N;
		if (next_tail == head.load(std::memory_order_acquire))
			return false; // Queue full
		buffer[current_tail] = item;
		tail.store(next_tail, std::memory_order_release);
		return true;
	}

	bool pop(T& item) {
		size_t current_head = head.load(std::memory_order_relaxed);
		if (current_head == tail.load(std::memory_order_acquire))
			return false; // Queue empty
		item = buffer[current_head];
		head.store((current_head + 1) % N, std::memory_order_release);
		return true;
	}

	void clear() {
		head.store(0, std::memory_order_relaxed);
		tail.store(0, std::memory_order_relaxed);
	}

	bool empty() const {
		return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_relaxed);
	}

	size_t size() const {
		size_t h = head.load(std::memory_order_relaxed);
		size_t t = tail.load(std::memory_order_relaxed);
		return (t >= h) ? (t - h) : (N - h + t);
	}
};

// Fixed-size step queue for optimal performance
struct StepQueue {
	static constexpr size_t MAX_STEPS = 64;
	std::array<Step, MAX_STEPS> steps;
	size_t read_idx = 0;
	size_t write_idx = 0;

	void push(const Step& s) {
		if (write_idx < MAX_STEPS) {
			steps[write_idx++] = s;
		}
	}

	Step pop() {
		return steps[read_idx++];
	}

	bool empty() const {
		return read_idx >= write_idx;
	}

	void clear() {
		read_idx = 0;
		write_idx = 0;
	}
};

// Use lock-free queue for inputs (up to 1024 inputs buffered)
extern LockFreeQueue<InputEvent, 1024> inputQueue;
extern std::deque<InputEvent> inputQueueCopy;

extern std::array<std::unordered_set<size_t>, 6> inputBinds;
extern std::unordered_set<uint16_t> heldInputs;

extern std::mutex keybindsLock;

extern std::atomic<bool> enableRightClick;
extern std::atomic<bool> softToggle;

extern bool threadPriority;

#if defined(GEODE_IS_WINDOWS)
#include "windows.hpp"
#else
extern TimestampType pendingInputTimestamp;
#endif