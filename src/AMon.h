#pragma once

#include <functional>
#include <stdint.h>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <assert.h>
#include <new>
#include <vector>
#include <unordered_map>
#include "pe_log.h"

// common defs
#define AMON_MINSTEP 5
enum StoreType { AMON_AUINT, AMON_FP16 };
class AMon;

#include "Alog.h"
//////// messaging between workers ///////
// task base
struct Task
{
	enum Type
	{
		TT_UNK,
		TT_WRITE,
		TT_READ,
		TT_STOP,
		RR_READ,
	} type = TT_UNK;
	virtual ~Task() { /*fprintf(stderr, "dtor Task %p\n", this);*/ }
};
// write task
struct TaskWrite: public Task
{
	std::function<int (const uint8_t *data, size_t size, AMon *amon)> processor;
	size_t dsize = 0;	// size of `data`
	uint8_t data[];
	static std::unique_ptr<TaskWrite> alloc(size_t dsize)
	{
		TaskWrite *p = (TaskWrite *)operator new(sizeof(TaskWrite) + sizeof(uint8_t[dsize]));
		new(p) TaskWrite();
		p->dsize = dsize;
		return std::unique_ptr<TaskWrite>(p);
	}
	int process(AMon *amon) { return processor(data, dsize, amon); }
protected:
	TaskWrite() { type = TT_WRITE; /*fprintf(stderr, "ctor TaskWrite %p\n", this);*/ }
};
// read task
struct TaskRead: public Task
{
	TaskRead() { type = TT_READ; }
	std::function<int (TaskRead *)> parsereq;
	std::function<int (TaskRead *)> response;
	// request
	uint32_t start = 0;
	uint32_t end = 0;
	std::vector<std::string> name;
	// result
	uint32_t step = 0;
	std::vector<float> databuf;
	std::vector<uint32_t> datatime;
};
#ifndef __cpp_lib_make_unique
namespace std
{
template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
}
#endif
struct TaskStop: public Task
{
	TaskStop() { type = TT_STOP; }
};
class TaskQueue
{
public:
	TaskQueue() { }
	~TaskQueue() { }
	void stop() { putfront(std::make_unique<TaskStop>()); }
	void put(std::unique_ptr<Task> &&task)
	{
		std::lock_guard<std::mutex> lock(mutex);
		tasks.push_back(std::move(task));
		cond.notify_all();
	}
	void putfront(std::unique_ptr<Task> &&task)
	{
		std::lock_guard<std::mutex> lock(mutex);
		tasks.push_front(std::move(task));
		cond.notify_all();
	}
	std::unique_ptr<Task> get()
	{
		std::unique_lock<std::mutex> lock(mutex);
		cond.wait(lock, [this] { return !tasks.empty(); });
		assert(!tasks.empty() && lock.owns_lock());
		std::unique_ptr<Task> task = std::move(tasks.front());
		tasks.pop_front();
		return task;
	}
	std::unique_ptr<Task> tryget()
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (tasks.empty())
			return NULL;
		std::unique_ptr<Task> task = std::move(tasks.front());
		tasks.pop_front();
		return task;
	}
	bool empty()
	{
		std::unique_lock<std::mutex> lock(mutex);
		return tasks.empty();
	}
private:
	std::deque<std::unique_ptr<Task>> tasks;
	std::mutex mutex;
	std::condition_variable cond;
};

//////// workers ////////
class Worker
{
public:
	Worker() { }
	virtual ~Worker() { }
	virtual int start() = 0;
	virtual int stop() = 0;
};

//////// interface workers (working in asio) ////////
class Receiver: public Worker
{
protected:
	TaskQueue *taskq = NULL;
};

class Reader: public Worker
{
protected:
	TaskQueue *taskq = NULL;
};

//////// data management worker (in separate thread) ///////
class AMon: public Worker
{
public:
	int stop();
	int start();
	TaskQueue *gettaskq() { return &taskq; }
	int addv(const char *name, uint32_t time, double value, StoreType type);
private:
	std::string datadir = "data";
	TaskQueue taskq;
	std::thread mainthrd;
	std::unordered_map<std::string, std::unique_ptr<Alog>> data;
private:
	void mainproc();
	int readdata(TaskRead *task);
};
