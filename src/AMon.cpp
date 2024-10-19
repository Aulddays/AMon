#include "AMon.h"
#include <math.h>
#include <algorithm>

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
			int res = 0;
			if ((res = task->parsereq(task)) != 0)
				PELOG_LOG((PLV_WARNING, "AMon process read parsereq failed\n"));
			if (res == 0 && (res = readdata(task)) != 0)
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
	uint32_t curtime = time(NULL);
	const int maxnum = 600;
	task->step = Alog::getrangeparam(task->start, task->end, curtime, maxnum);
	int datalen = (task->end - task->start) / task->step + 1;
	task->datatime.resize(datalen);
	task->databuf.resize(datalen * task->names.size());
	for (uint32_t curtime = task->start, idx = 0; curtime <= task->end; curtime += task->step, ++idx)
		task->datatime[idx] = curtime;
	for (size_t iname = 0; iname < task->names.size(); ++iname)
	{
		float *databuf = task->databuf.data() + iname * datalen;
		auto idata = data.find(task->names[iname]);
		if (idata == data.end())	// try load log data if not found in memory
		{
			std::unique_ptr<Alog> plog = std::make_unique<Alog>();
			if(plog->init(datadir.c_str(), task->names[iname].c_str(), AMON_NULL) == 0)
				idata = data.emplace(task->names[iname], std::move(plog)).first;
		}
		if (idata != data.end())
			idata->second->getrange(task->start, task->end, task->step, databuf);
		else
		{
			std::for_each(databuf, databuf + datalen, [](float &d){ d = NAN; });
			PELOG_LOG((PLV_WARNING, "No data %s\n", task->names[iname].c_str()));
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

