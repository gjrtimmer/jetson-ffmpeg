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
 * the same lock. All operations are non-blocking: dq* returns NULL when the
 * queue is empty — callers implement their own waiting/polling. The pool
 * stores raw values/pointers only and never owns or frees the buffers.
 */
#pragma once
#include <queue>
#include <mutex>

template <typename T>
struct NVMPI_bufPool
{	
	std::mutex m_emptyBuf;
	std::mutex m_filledBuf;
	std::queue<T> emptyBuf; //list of buffers available to fill 
	std::queue<T> filledBuf; //filled buffers to consume
	
	T dqEmptyBuf();
	void qEmptyBuf(T buf);
	//Peek at the buffer without dequeuing. Potentially dangerous. Use
	//only in places where simultaneous access to the queue is not possible.
	T peekEmptyBuf();
	
	T dqFilledBuf();
	void qFilledBuf(T buf);
};

//Pop the oldest buffer from the "empty" queue (consumer side of the
//recycle path). Non-blocking: returns NULL immediately if none available.
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

//Return the front of the "empty" queue WITHOUT removing it (NULL if empty).
//The returned buffer can still be dequeued by another thread afterwards,
//hence the warning above: only safe when no concurrent dqEmptyBuf() can
//run (e.g. decoder setup, before the capture loop consumes buffers).
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

//Pop the oldest filled buffer (consumer side of the data path).
//Non-blocking: returns NULL if no filled buffer is ready yet.
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

//Return a consumed buffer to the "empty" queue so the producer can reuse
//it. Never blocks (std::queue grows as needed).
template<typename T>
void NVMPI_bufPool<T>::qEmptyBuf(T buf)
{
	m_emptyBuf.lock();
	emptyBuf.push(buf);
	m_emptyBuf.unlock();
	return;
}

//Publish a freshly filled buffer for the consumer (producer side of the
//data path). Never blocks.
template<typename T>
void NVMPI_bufPool<T>::qFilledBuf(T buf)
{
	m_filledBuf.lock();
	filledBuf.push(buf);
	m_filledBuf.unlock();
	return;
}
