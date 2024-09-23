#include "AMon.h"

int AMon::start()
{
	if (mainthrd.joinable())
		PELOG_ERROR_RETURN((PLV_ERROR, "AMon already running\n"), -1);
	mainthrd = std::thread(&AMon::mainproc, this);
	return 0;
}

int AMon::stop()
{
	if (!mainthrd.joinable())
		return -1;
	PELOG_LOG((PLV_INFO, "AMon job to stop\n"));
	taskq.put(std::make_unique<TaskStop>());
	mainthrd.join();
	return 0;
}

void AMon::mainproc()
{
	PELOG_LOG((PLV_VERBOSE, "AMon job started\n"));
	bool running = true;
	while (running)
	{
		std::unique_ptr<Task> t = taskq.get();
		switch (t->type)
		{
		case Task::TT_STOP:
			running = false;
			break;
		case Task::TT_WRITE:
		{
			TaskWrite *task = (TaskWrite *)t.get();
			if (task->process(this) != 0)
				PELOG_LOG((PLV_WARNING, "AMon process write failed\n"));
			break;
		}
		case Task::TT_READ:
		{
			TaskRead *task = (TaskRead *)t.get();
			if (task->parsereq(task) != 0)
				PELOG_LOG((PLV_WARNING, "AMon process read parsereq failed\n"));
			if (readdata(task) != 0)
				PELOG_LOG((PLV_WARNING, "AMon process read failed\n"));
			if (task->response(task) != 0)
				PELOG_LOG((PLV_WARNING, "AMon process read response failed\n"));
			break;
		}
		default:
			PELOG_LOG((PLV_ERROR, "AMon unexpected task %d\n", t->type));
			break;
		}
	}
	PELOG_LOG((PLV_VERBOSE, "AMon job finished\n"));
}

int AMon::readdata(TaskRead *task)
{
	const int maxnum = 500;
	task->end = std::max(task->start, task->end);
	int step = (task->end - task->start) / maxnum;
	step = step + 4 - (step + 4) % 5;
	step = std::max(5, step);
	task->start = task->start - task->start % step;
	task->end = task->end - task->end % step;
	task->datatime.clear();
	task->databuf.clear();
	for (uint32_t cur = task->start; cur <= task->end; cur += step)
		task->datatime.push_back(cur);
	for (size_t iname = 0; iname < task->name.size(); ++iname)
	{
		for (uint32_t cur = task->start; cur <= task->end; cur += step)
		{
			uint64_t seed = cur * (iname + 1) % 10000;
			seed = (seed + 39916801) * (seed + 65537) * (seed + 160001);
			task->databuf.push_back(10 + seed % 1000 / 1000.0 - 5);
		}
	}
	return 0;
}

int AMon::addv(const char *name, uint32_t time, double value, StoreType type)
{
	auto ilog = data.find(name);
	if (ilog == data.end())
	{
		ilog = data.emplace(name, std::make_unique<Alog>()).first;
		ilog->second->init(datadir.c_str(), name, type);
	}
	return ilog->second->addv(time, value, type);
}

