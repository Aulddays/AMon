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
			if (res == 0 && (res = getdata(task)) != 0)
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

int AMon::getdata(TaskRead *task)
{
	if (task->aggr >= 0 && task->aggr < TaskRead::AMON_AGGRNUM)
		return doaggr(task);
	return doread(task);
}

int AMon::doread(TaskRead *task)
{
	uint32_t curtime = time(NULL);
	const int maxnum = 500;
	task->step = Alog::getrangeparam(task->start, task->end, curtime, maxnum);
	int datalen = (task->end - task->start) / task->step;
	task->datatime.resize(datalen);
	task->databuf.resize(datalen * task->names.size());
	for (uint32_t curtime = task->start, idx = 0; curtime < task->end; curtime += task->step, ++idx)
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

int AMon::doaggr(TaskRead *task)
{
	constexpr int32_t tzoff = 3600 * 8;	// Default TZ GMT+8
	constexpr int32_t weekoff = 86400 * 4 + tzoff;	// epoch is (86400 * 4(Thursday) + tzoff) in TZ
	task->datatime.clear();
	task->databuf.clear();
	if (task->start < 86400 * 7 - weekoff || task->end < task->start)
		return 0;
	// fix the aggr level if not approperate
	static constexpr int32_t aggrsteps[] = { 60, 3600, 86400, 86400 * 7, 86400 * 30, 86400 * 365 };
	static_assert(TaskRead::AMON_AGGRNUM == sizeof(aggrsteps) / sizeof(aggrsteps[0]), "aggrsteps not match");
	constexpr int MAXRAGES = 90;
	for (; task->aggr < TaskRead::AMON_AGGRNUM; task->aggr = (TaskRead::Aggr)(task->aggr + 1))
	{
		if ((task->end - task->start) / aggrsteps[task->aggr] <= MAXRAGES)
			break;
	}
	if (task->aggr == TaskRead::AMON_YEAR && (task->end - task->start) / aggrsteps[task->aggr] > MAXRAGES)
		task->start = task->end - aggrsteps[task->aggr] * MAXRAGES;
	// fill datatime
	if (task->aggr <= TaskRead::AMON_WEEK)
	{
		static constexpr int32_t aggroffs[] = { 0, 0, tzoff,  weekoff };
		static_assert(TaskRead::AMON_WEEK + 1 == sizeof(aggroffs) / sizeof(aggroffs[0]), "aggroffs not match");
		int32_t step = aggrsteps[task->aggr];
		task->start -= (task->start + aggroffs[task->aggr]) % step;
		for (uint32_t cur = task->start; true; cur += step)
		{
			task->datatime.push_back(cur);
			if (cur >= task->end)
				break;
		}
	}
	else if (task->aggr == TaskRead::AMON_MONTH || task->aggr == TaskRead::AMON_YEAR)
	{
		task->start -= (task->start + tzoff) % 86400;
		time_t timeshift = task->start + tzoff;
		struct tm stm;
		gmtime_r(&timeshift, &stm);
		assert(stm.tm_hour == 0 && stm.tm_min == 0 && stm.tm_sec == 0);
		int year = 1900 + stm.tm_year;
		int month = stm.tm_mon;
		task->start -= 86400 * (task->aggr == TaskRead::AMON_MONTH ? stm.tm_mday - 1 : stm.tm_yday);
		for (uint32_t cur = task->start; true; )
		{
			task->datatime.push_back(cur);
			if (cur >= task->end)
				break;
			bool isleap = year % 4 == 0 && year % 100 != 0 || year % 400 == 0;
			if (task->aggr == TaskRead::AMON_MONTH)
			{
				static constexpr uint32_t mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
				int step = mdays[month] + (isleap && month == 1 ? 1 : 0);
				cur += step * 86400;
				month += 1;
				if (month >= 12)
				{
					month = 0;
					year++;
				}
			}
			else	// task->aggr == TaskRead::AMON_YEAR
			{
				int step = 365 + (isleap ? 1 : 0);
				cur += step * 86400;
				year++;
			}
		}
	}	// else if (task->aggr == TaskRead::AMON_MONTH || task->aggr == TaskRead::AMON_YEAR)
	// now datatime contains 1 more element at the end specifying end of the last aggr range, which MUST be removed before return
	// fill data
	int datalen = task->datatime.size() - 1;
	task->databuf.resize(datalen * task->names.size());
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
			idata->second->aggrrange(task->datatime, databuf);
		else
		{
			std::for_each(databuf, databuf + datalen, [](float &d){ d = 0; });
			PELOG_LOG((PLV_WARNING, "No data %s\n", task->names[iname].c_str()));
		}
	}
	task->datatime.pop_back();	// remove the temporary extra element in datatime
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

