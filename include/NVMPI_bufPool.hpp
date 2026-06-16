/*
 * NVMPI_bufPool.hpp — generic thread-safe producer/consumer buffer pool
 * (libnvmpi layer, header-only template).
 *
 * Both pipelines in libnvmpi recycle buffers through one of these pools:
 *   - decoder (nvmpi_dec_capture.cpp): NVMPI_bufPool<NVMPI_frameBuf*> — the capture
 *     thread takes "empty" DMA frame buffers, fills them via hw transform
 *     and queues them "filled"; the user thread consumes filled frames and
 *     returns them to the empty queue.
 *   - encoder (nvmpi_enc.cpp): NVMPI_bufPool<nvPacket*> — the FFmpeg wrapper
 *     supplies empty packet buffers, the encoder capture callback fills
 *     them, and the user dequeues filled packets.
 *
 * Semantics: two independent FIFO queues ("empty" and "filled"), each
 * guarded by its own mutex so a producer and a consumer never contend on
 * the same lock. The filled queue additionally supports:
 *   - condition-variable blocking: dqFilledBuf(timeout) blocks until an
 *     item arrives or the deadline expires, using 100ms internal wake-up
 *     granularity to remain responsive to shutdown.
 *   - shutdown(): sets a flag and wakes all blocked consumers, causing
 *     them to return NULL immediately.
 *   - reset(): clears the shutdown flag for flush/restart cycles.
 *
 * Thread safety: m_emptyBuf guards the empty queue; m_filledBuf guards
 * the filled queue AND the condition variable. Never hold both locks
 * simultaneously. m_shutdown is atomic and may be read without holding
 * any lock; it is written under m_filledBuf to ensure the CV wakeup
 * is visible to waiters.
 */
#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

template <typename T>
struct NVMPI_bufPool
{
	/* m_emptyBuf: guards emptyBuf queue only.
	 * Never hold together with m_filledBuf — deadlock. */
	std::mutex m_emptyBuf;

	/* m_filledBuf: guards filledBuf queue AND cv_filledBuf.
	 * qFilledBuf() locks this, pushes, then notifies one waiter.
	 * shutdown() locks this, sets m_shutdown, then notifies all. */
	std::mutex m_filledBuf;

	/* Signalled by qFilledBuf() (notify_one) and shutdown() (notify_all).
	 * Waited on by dqFilledBuf(timeout) with 100ms granularity ticks. */
	std::condition_variable cv_filledBuf;

	/* Set by shutdown(), cleared by reset(). Checked by blocking
	 * dqFilledBuf to break out early. Atomic so the non-blocking
	 * dqFilledBuf can read it without taking the mutex. */
	std::atomic<bool> m_shutdown{false};

	std::queue<T> emptyBuf;
	std::queue<T> filledBuf;

	T dqEmptyBuf();
	void qEmptyBuf(T buf);
	T peekEmptyBuf();

	/* Non-blocking: returns NULL immediately if queue is empty. */
	T dqFilledBuf();
	/* Blocking: waits up to timeout for an item. Returns NULL on timeout
	 * or shutdown. Uses 100ms internal granularity for shutdown responsiveness. */
	T dqFilledBuf(std::chrono::milliseconds timeout);
	void qFilledBuf(T buf);

	/* Signal all blocked dqFilledBuf consumers to return NULL. */
	void shutdown();
	/* Clear shutdown flag — call after flush/restart before reusing the pool. */
	void reset();
};

/* Dequeue one empty buffer (non-blocking). Returns NULL if none available. */
template<typename T>
T NVMPI_bufPool<T>::dqEmptyBuf()
{
	T buf = NULL;
	m_emptyBuf.lock();
	if(!emptyBuf.empty())
	{
		buf = emptyBuf.front();
		emptyBuf.pop();
	}
	m_emptyBuf.unlock();
	return buf;
}

/* Peek at the front empty buffer without removing it (non-blocking).
 * Used by updateFrameSizeParams() to read buffer geometry before any
 * consumer is active — safe only in that context. */
template<typename T>
T NVMPI_bufPool<T>::peekEmptyBuf()
{
	T buf = NULL;
	m_emptyBuf.lock();
	if(!emptyBuf.empty())
	{
		buf = emptyBuf.front();
	}
	m_emptyBuf.unlock();
	return buf;
}

/* Non-blocking dequeue from filled queue. Returns NULL immediately
 * if the queue is empty. This is the original API — unchanged. */
template<typename T>
T NVMPI_bufPool<T>::dqFilledBuf()
{
	T buf = NULL;
	m_filledBuf.lock();
	if(!filledBuf.empty())
	{
		buf = filledBuf.front();
		filledBuf.pop();
	}
	m_filledBuf.unlock();
	return buf;
}

/* Blocking dequeue: waits up to `timeout` for a filled buffer.
 *
 * Uses tiered 100ms cv.wait_for iterations rather than a single
 * wait_for(timeout) so that:
 *   1. shutdown() wakes the thread within 100ms even if no item arrives.
 *   2. Spurious wakeups are harmless — the loop re-checks the queue.
 *
 * Returns NULL if the timeout expires or shutdown() was called. */
template<typename T>
T NVMPI_bufPool<T>::dqFilledBuf(std::chrono::milliseconds timeout)
{
	using Clock = std::chrono::steady_clock;
	constexpr auto GRANULARITY = std::chrono::milliseconds(100);
	auto deadline = Clock::now() + timeout;

	std::unique_lock<std::mutex> lock(m_filledBuf);
	while (true)
	{
		/* Check queue first — may already have data. */
		if (!filledBuf.empty())
		{
			T buf = filledBuf.front();
			filledBuf.pop();
			return buf;
		}
		/* Shutdown requested — exit immediately. */
		if (m_shutdown.load(std::memory_order_acquire))
			return NULL;

		/* Compute remaining time; bail if deadline passed. */
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - Clock::now());
		if (remaining <= std::chrono::milliseconds(0))
			return NULL;

		/* Wait for min(remaining, 100ms) — caps shutdown latency. */
		auto wait_time = std::min(remaining, GRANULARITY);
		cv_filledBuf.wait_for(lock, wait_time);
	}
}

/* Enqueue a buffer to the empty pool (producer returns buffer). */
template<typename T>
void NVMPI_bufPool<T>::qEmptyBuf(T buf)
{
	m_emptyBuf.lock();
	emptyBuf.push(buf);
	m_emptyBuf.unlock();
}

/* Enqueue a filled buffer and wake one blocked consumer.
 * Lock ordering: only m_filledBuf is held; notify_one after unlock
 * would also be correct but notify under lock avoids a missed-wakeup
 * race where the consumer checks the queue between push and notify. */
template<typename T>
void NVMPI_bufPool<T>::qFilledBuf(T buf)
{
	{
		std::lock_guard<std::mutex> lock(m_filledBuf);
		filledBuf.push(buf);
	}
	cv_filledBuf.notify_one();
}

/* Signal shutdown: set the flag under the filled-queue lock so that
 * any consumer currently inside wait_for sees the flag on wakeup,
 * then notify_all to wake every blocked thread. */
template<typename T>
void NVMPI_bufPool<T>::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_filledBuf);
		m_shutdown.store(true, std::memory_order_release);
	}
	cv_filledBuf.notify_all();
}

/* Clear the shutdown flag — called during flush/restart before the
 * pool is reused so that dqFilledBuf(timeout) blocks again. */
template<typename T>
void NVMPI_bufPool<T>::reset()
{
	std::lock_guard<std::mutex> lock(m_filledBuf);
	m_shutdown.store(false, std::memory_order_release);
}
