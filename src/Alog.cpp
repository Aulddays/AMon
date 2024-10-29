#include "AMon.h"
#include <assert.h>
#include <time.h>
#include <thread>
#include "pe_log.h"
#include "AUint.h"
#include "fp16/fp16.h"

// data struct:
// two or more time levels, with fine to rough granularities (step) and short to long time coverage spans (period).
// level[0] are more precise (in float), others are in uint16_t (either fp16 or AUint).
// level data buffers are round-robin, except level[-1] which keeps all history (VPERIOD[-1] is initial value)
// file format:
// name being file name
// Header, LevelInfo[header.lvnum], level0 values (float), level[1] values (uint16_t), ...

// default level setups
const static int32_t VSTEP[] = { 5, 60, 600, 1800 };
const static int32_t VPERIOD[] = { 86400, 86400 * 15, 86400 * 183, 86400 * 365 };
//const static int32_t VSTEP[] = { 5, 10, 15, 15 };
//const static int32_t VPERIOD[] = { 180, 240, 300, 86400 };
const static int32_t VSTEPLEN[] = { VPERIOD[0] / VSTEP[0], VPERIOD[1] / VSTEP[1], VPERIOD[2] / VSTEP[2], VPERIOD[3] / VSTEP[3] };
static_assert(ALOG_DEF_LVNUM == sizeof(VSTEP) / sizeof(VSTEP[0]) &&
	sizeof(VSTEP) / sizeof(VSTEP[0]) == sizeof(VPERIOD) / sizeof(VPERIOD[0]), "VSTEP & VPERIOD mismatch");
// config check. could be made static_assert if constexpr is supported
static struct StaticParamChecker
{
	StaticParamChecker()
	{
		assert(VSTEP[0] == AMON_MINSTEP);
		assert(VPERIOD[0] >= 60);
		for (size_t i = 0; i < ALOG_DEF_LVNUM; ++i)
		{
			assert(86400 % VSTEP[i] == 0 || VSTEP[i] % 86400 == 0);
			assert(86400 % VPERIOD[i] == 0 || VPERIOD[i] % 86400 == 0);
			assert(VSTEP[i] % VSTEP[0] == 0);
		}
		assert(VPERIOD[0] >= VSTEP[ALOG_DEF_LVNUM - 1] * 10);
	}
} static_param_checker;

// get the round time (actual write time) of `steptime`
// eg, for level==1 whose VSTEP[1]==60, steptime=>round: 0=>0, 1~60=>60, 61~120=>120, ...
uint32_t roundtime(uint32_t steptime, int32_t step)
{
	uint32_t round = steptime + step - 1;
	return round - round % step;
}

uint32_t lvmintime(uint32_t wtime, int32_t len, int32_t step)
{
	return wtime < (uint32_t)(step * len - step) ? (wtime == 0 ? 0 : step) : wtime - (step * len) + step;
}

int32_t lvtimepos(uint32_t time, uint32_t wtime, int32_t wpos, int32_t lvlen, int32_t lvstep)
{
	assert(time <= wtime);
	assert(time % lvstep == 0 && wtime % lvstep == 0);
	uint32_t mintime = lvmintime(wtime, lvlen, lvstep);
	if (mintime == 0 || time < mintime)	// out of range
		return -1;
	int32_t pos = wpos - 1 - (int32_t)((wtime - time) / lvstep);	// (wpos-1) is the actual pos of wtime
	if (pos < 0)
		pos += lvlen;
	assert(pos >= 0);
	return pos;
}

// return the smallest value that is >= val and is multiple of mul
uint32_t roundup(uint32_t val, uint32_t mul)
{
	val = std::max(1u, val);
	val = val + mul - 1;
	return val - val % mul;
}

Alog::Alog()
{
}

Alog::~Alog()
{
	if (inited)
		updatefile(true);
}

int Alog::init(const char *dir, const char *logname, StoreType type)
{
	static_assert(AMON_AUINT == 0 && AMON_FP16 == 1, "StoreType does not match");
	static uint16_t(*raw2store_funcs[])(double) = {
		[](double v) -> uint16_t { return isnan(v) ? AUint<12>::fromnan() : AUint<12>::fromint((uint32_t)v); },
		[](double v) -> uint16_t { return fp16_ieee_from_fp32_value((float)v); },
	};
	static double(*store2raw_funcs[])(uint16_t) = {
		[](uint16_t v) -> double { return AUint<12>::isnan(v) ? NAN : AUint<12>::toint(v); },
		[](uint16_t v) -> double { return fp16_ieee_to_fp32_value(v); },
	};
	static bool(*testnan_funcs[])(uint16_t) = {
		AUint<12>::isnan,
		[](uint16_t v) -> bool { return ::isnan(fp16_ieee_to_fp32_value(v)); },
	};
	static uint16_t (*setnan_funcs[])() = {
		AUint<12>::fromnan,
		[]() -> uint16_t { return fp16_ieee_from_fp32_value(NAN); },
	};

	inited = false;
	name = logname;
	filename = std::string(dir) + '/' + logname;
	//value0.resize(VPERIOD[0] / VSTEP[0]);

	// load from file
	FILEGuard fp = fopen(filename.c_str(), "r+b");
	if (fp)
	{
		fseek(fp, 0, SEEK_END);
		long fsize = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		// read header
		if (fread(&h, sizeof(h), 1, fp) != 1)
			PELOG_ERROR_RETURN((PLV_ERROR, "Load failed %s\n", filename.c_str()), -1);
		if (type == AMON_NULL && (h.stype == AMON_AUINT || h.stype == AMON_FP16))
			type = (StoreType)h.stype;
		else if (type != h.stype)
			PELOG_ERROR_RETURN((PLV_ERROR, "Type not match %d:%d %s\n", type, h.stype, filename.c_str()), -1);
		if (type == AMON_NULL)
			PELOG_ERROR_RETURN((PLV_ERROR, "Invalid type for Alog %s\n", filename.c_str()), -1);
		if (h.lvnum < 2 || h.lvnum > 20)
			PELOG_ERROR_RETURN((PLV_ERROR, "Invalid level num %d %s\n", h.lvnum, filename.c_str()), -1);
		// read level info
		lv.resize(h.lvnum);
		if ((int)fread(lv.data(), sizeof(lv[0]), h.lvnum, fp) != h.lvnum)
			PELOG_ERROR_RETURN((PLV_ERROR, "Load info failed %s\n", filename.c_str()), -1);
		// verify
		if (lv[0].step != AMON_MINSTEP)
			PELOG_ERROR_RETURN((PLV_ERROR, "Incompatible step %d %s\n", lv[0].step, filename.c_str()), -1);
		int32_t basepos = sizeof(h) + sizeof(lv[0]) * h.lvnum;
		for (int i = 0; i < h.lvnum; ++i)
		{
			if (lv[i].off != basepos || lv[i].pos < 0 || lv[i].pos > lv[i].len || lv[i].len <= 0 || lv[i].step <= 0)
				PELOG_ERROR_RETURN((PLV_ERROR, "Data file corrupted (%d:%d:%d:%d:%d) %s\n",
				i, (int)lv[i].step, (int)lv[i].len, (int)lv[i].off, (int)basepos, filename.c_str()), -1);
			int32_t period = lv[i].step * lv[i].len;
			if (i == 0 && lv[i].step > 86400 || lv[i].step > 10 * 86400 || 86400 % lv[i].step != 0 && lv[i].step % 86400 != 0 ||
					i != h.lvnum - 1 && lv[i].len > 10 * 1024 * 1024 || period % 86400 != 0 && 86400 % period != 0)
				PELOG_ERROR_RETURN((PLV_ERROR, "Data file incompatible (%d:%d:%d:%d:%d) %s\n",
				i, (int)lv[i].step, (int)lv[i].len, (int)lv[i].off, (int)basepos, filename.c_str()), -1);
			lv[i].time -= lv[i].time % lv[i].step;
			if (lv[i].time > 500000000 && (lv[i].time < 1577808000 || lv[i].time > 2524579200u))
				PELOG_ERROR_RETURN((PLV_ERROR, "Data file currupted (%d:%u) %s\n", i, lv[i].time, filename.c_str()), -1);
			if (lv[i].step % lv[0].step != 0)
				PELOG_ERROR_RETURN((PLV_ERROR, "Data file incompatible (%d:%d) %s\n", i, (int)lv[i].step, filename.c_str()), -1);
			basepos += lv[i].len * (i == 0 ? sizeof(float) : sizeof(uint16_t));
		}
		if (fsize < basepos)
			PELOG_ERROR_RETURN((PLV_ERROR, "Data file corrupted (%d:%d:%d) %s\n",
			-1, (int)fsize, (int)basepos, filename.c_str()), -1);
		// read values
		value0.resize(lv[0].len);
		if ((int)fread(value0.data(), sizeof(value0[0]), lv[0].len, fp) != lv[0].len)
			PELOG_ERROR_RETURN((PLV_ERROR, "Load data failed 0 %s\n", filename.c_str()), -1);
		value.resize(h.lvnum);
		for (int i = 1; i < h.lvnum; ++i)
		{
			value[i].resize(lv[i].len);
			if ((int)fread(value[i].data(), sizeof(value[i][0]), lv[i].len, fp) != lv[i].len)
				PELOG_ERROR_RETURN((PLV_ERROR, "Load data failed %d %s\n", i, filename.c_str()), -1);
		}
		PELOG_LOG((PLV_INFO, "Loaded data %s\n", filename.c_str()));
	}
	else	// Datafile not found, init new
	{
		if (type == AMON_NULL)
			PELOG_ERROR_RETURN((PLV_ERROR, "Missing type for Alog %s\n", filename.c_str()), -1);
		fp = fopen(filename.c_str(), "wb");
		if (!fp)
			PELOG_ERROR_RETURN((PLV_ERROR, "Write failed %s\n", filename.c_str()), -1);
		// header
		h.stype = type;
		h.lvnum = ALOG_DEF_LVNUM;
		if (fwrite(&h, sizeof(h), 1, fp) != 1)
			PELOG_ERROR_RETURN((PLV_ERROR, "Init failed %s\n", filename.c_str()), -1);
		// level info
		lv.resize(h.lvnum);
		int32_t basepos = sizeof(h) + sizeof(lv[0]) * h.lvnum;
		for (int i = 0; i < h.lvnum; ++i)
		{
			lv[i].step = VSTEP[i];
			lv[i].off = basepos;
			lv[i].len = VSTEPLEN[i];
			lv[i].time = 0;
			lv[i].pos = 0;
			basepos += lv[i].len * (i == 0 ? sizeof(float) : sizeof(uint16_t));
		}
		if ((int)fwrite(lv.data(), sizeof(lv[0]), h.lvnum, fp) != h.lvnum)
			PELOG_ERROR_RETURN((PLV_ERROR, "Init info failed %s\n", filename.c_str()), -1);
		// values
		value0.clear();
		value0.resize(lv[0].len, NAN);
		if ((int)fwrite(value0.data(), sizeof(value0[0]), lv[0].len, fp) != lv[0].len)
			PELOG_ERROR_RETURN((PLV_ERROR, "Load data failed 0 %s\n", filename.c_str()), -1);
		value.clear();
		value.resize(h.lvnum);
		for (int i = 1; i < h.lvnum; ++i)
		{
			value[i].resize(lv[i].len, setnan_funcs[type]());
			if ((int)fwrite(value[i].data(), sizeof(value[i][0]), lv[i].len, fp) != lv[i].len)
				PELOG_ERROR_RETURN((PLV_ERROR, "Load data failed %d %s\n", i, filename.c_str()), -1);
		}
		PELOG_LOG((PLV_INFO, "Inited data %s\n", filename.c_str()));
	}
	
	raw2store = raw2store_funcs[type];
	store2raw = store2raw_funcs[type];
	testnan = testnan_funcs[type];
	setnan = setnan_funcs[type];

	writetime = (uint32_t)time(NULL);
	writestep = lv[0].time;
	pending.resize(h.lvnum);
	for (int i = 0; i < h.lvnum; ++i)
		pending[i] = 0;
	ispending = false;

	inited = true;
	return 0;
}

int Alog::addv(uint32_t time, double value)
{
	if (!inited)
		PELOG_ERROR_RETURN((PLV_WARNING, "Alog not inited %s\n", name.c_str()), -1);

	time -= time % lv[0].step;
	if (time + std::min(60, lv[0].step * lv[0].len) <= lv[0].time)
		PELOG_ERROR_RETURN((PLV_WARNING, "Alog ignore old data time\n"), 0);
	firsttime = std::min(time, firsttime);
	if (writestep == 0)
		writestep = time - lv[0].step;

	// fill missing values with NAN
	for (uint32_t uptime = lv[0].time + lv[0].step; lv[0].time != 0 && uptime < time; uptime += lv[0].step)
	{
		value0[lv[0].pos] = NAN;
		++lv[0].pos;
		pending[0]++;
		if (lv[0].pos >= lv[0].len)
			lv[0].pos = 0;
		lv[0].time = uptime;
		updatelevels();
	}
	// record the new value
	assert(lv[0].time == 0 || time <= lv[0].time + lv[0].step);
	int32_t uppos = lv[0].time == 0 ? 0 :
		(lv[0].pos + lv[0].len - (lv[0].time + lv[0].step - time) / lv[0].step) % lv[0].len;
	value0[uppos] = (float)value;
	if (time > lv[0].time)
	{
		assert(lv[0].pos == uppos);
		++lv[0].pos;
		pending[0]++;
		if (lv[0].pos >= lv[0].len)
			lv[0].pos = 0;
		lv[0].time = time;
		updatelevels();
	}
	else if (lv[0].time > 0)	// if got a history value, also expand pending num
		pending[0] = std::max(pending[0], (int32_t)(lv[0].time - time) / lv[0].step);
	// write to file
	if (ispending && updatefile() != 0)
		PELOG_ERROR_RETURN((PLV_ERROR, "Alog write data failed %s\n", name.c_str()), -1);
	return 0;
}

int Alog::updatelevel(int level)
{
	const int32_t UPDELAY = 60;	// delay writing in case of delayed data

	assert(lv[level].time % lv[level].step == 0);
	uint32_t lrtime = roundtime(lv[level].time, lv[level].step);
	if (lrtime == 0)
		lrtime = roundtime(firsttime, lv[level].step) - lv[level].step;
	uint32_t datatime = lv[0].time;
	if (datatime < lrtime + lv[level].step + UPDELAY)	// no need to write yet
		return 0;
	// prepare data
	uint32_t mintime0 = lvmintime(lv[0].time, lv[0].len, lv[0].step);
	for (uint32_t curround = lrtime + lv[level].step; curround <= datatime - UPDELAY; curround += lv[level].step)
	{
		uint32_t btime0 = std::max(curround - lv[level].step + lv[0].step, mintime0);
		int32_t bpos0 = lvtimepos(btime0, lv[0].time, lv[0].pos, lv[0].len, lv[0].step);
		double aggrv = 0;
		int32_t aggrc = 0;
		if (bpos0 >= 0)
		{
			for (uint32_t steptime = btime0; steptime <= curround; steptime += lv[0].step, bpos0++)
			{
				if (bpos0 >= lv[0].len)
					bpos0 = 0;
				if (!isnan(value0[bpos0]))
				{
					aggrv += value0[bpos0];
					aggrc++;
				}
			}
		}
		aggrv = aggrc > 0 ? (float)(aggrv / aggrc) : NAN;
		// record new value
		value[level][lv[level].pos] = aggrc > 0 ? raw2store(aggrv) : setnan();
		lv[level].time = curround;
		lv[level].pos++;
		if (lv[level].pos >= lv[level].len)	// need rotating
		{
			if (level != h.lvnum - 1)
				lv[level].pos = 0;
			else	// last level, do not rotate, but expand the storage
			{
				assert((int)value[level].size() == lv[level].len);
				size_t orilen = value[level].size();
				size_t oriperiod = value[level].size() * lv[level].step;
				size_t expandlen = std::max(86400, std::min(30 * 86400, (int)roundup(oriperiod / 4, 86400))) / lv[level].step;
				value[level].resize(value[level].size() + expandlen, setnan());
				// also expand the file
				FILEGuard fp = fopen(filename.c_str(), "r+b");
				if (!fp)
					PELOG_ERROR_RETURN((PLV_ERROR, "Expand data file failed %s\n", filename.c_str()), -1);
				fseek(fp, lv[level].off + sizeof(value[level][0]) * lv[level].len, SEEK_SET);
				if (fwrite(&value[level][orilen], sizeof(value[level][0]), expandlen, fp) != expandlen)
					PELOG_ERROR_RETURN((PLV_ERROR, "Expand data file failed %d %s\n", (int)expandlen, filename.c_str()), -1);
				lv[level].len += expandlen;
			}
		}
		pending[level]++;
		ispending = true;
	}
	return 0;
}

int Alog::updatefile(bool force)
{
	const int32_t MINWRITESTEP = 600;	// write to disk every WRITESTEP (data) seconds
	const int32_t MINWRITETIME = 120;	// write to disk every WRITETIME (system) seconds
	//const int32_t MINWRITETIME = 1;	// write to disk every WRITETIME (system) seconds

	if (force && !ispending && pending[0] == 0 || !force && (!ispending || lv[0].time < writestep + MINWRITESTEP))	// no pending data
		return 0;
	if (!force)
	{
		uint32_t curtime = (uint32_t)time(NULL);
		if (curtime < writetime + MINWRITETIME && curtime + MINWRITETIME > writetime)
			return 0;
		PELOG_LOG((PLV_DEBUG, "To write to file %d %d %d %d, %s\n", curtime, writetime, lv[0].time, writestep, filename.c_str()));
	}
	writetime = (uint32_t)time(NULL);
	writestep = lv[0].time;
	// to write to file
	PELOG_LOG((PLV_DEBUG, "To write to file %s\n", filename.c_str()));
	FILEGuard fp = fopen(filename.c_str(), "r+b");
	if (!fp)
		PELOG_ERROR_RETURN((PLV_WARNING, "Write failed %s\n", filename.c_str()), -1);
	// write level info
	fseek(fp, sizeof(FileHeader), SEEK_SET);
	if ((int)fwrite(lv.data(), sizeof(lv[0]), h.lvnum, fp) != h.lvnum)
		PELOG_ERROR_RETURN((PLV_WARNING, "Write lvinfo failed %s\n", filename.c_str()), -1);
	for (int level = 0; level < h.lvnum; ++level)
	{
		if (pending[level] <= 0)
			continue;
		pending[level] = std::min(pending[level], lv[level].len);
		int bpos = std::max(lv[level].pos - pending[level], 0);
		size_t isize = level == 0 ? sizeof(value0[0]) : sizeof(value[level][0]);
		const void *pdata = level == 0 ? (const void *)&value0[bpos] : (const void *)&value[level][bpos];
		fseek(fp, lv[level].off + isize * bpos, SEEK_SET);
		if ((int)fwrite(pdata, isize, lv[level].pos - bpos, fp) != lv[level].pos - bpos)
			PELOG_ERROR_RETURN((PLV_WARNING, "Write lvdata failed %s\n", filename.c_str()), -1);
		if (lv[level].pos < pending[level])	// more to write at the end of data buffer
		{
			bpos = lv[level].len - (pending[level] - lv[level].pos);
			pdata = level == 0 ? (const void *)&value0[bpos] : (const void *)&value[level][bpos];
			fseek(fp, lv[level].off + isize * bpos, SEEK_SET);
			if ((int)fwrite(pdata, isize, lv[level].len - bpos, fp) != lv[level].len - bpos)
				PELOG_ERROR_RETURN((PLV_WARNING, "Write lvdata failed %s\n", filename.c_str()), -1);
		}
		pending[level] = 0;
	}
	ispending = false;

	return 0;
}

void Alog::dump()
{
	if (!inited)
		PELOG_ERROR_RETURNVOID((PLV_WARNING, "Alog not inited %s\n", name.c_str()));
	for (int level = 0; level < h.lvnum; ++level)
	{
		printf("Level %d, step %d, len %d, period %d, time %u, pos %d\n",
			level, lv[level].step, lv[level].len, lv[level].step * lv[level].len, lv[level].time, lv[level].pos);
		if (lv[level].time == 0)
			continue;
		assert(level != h.lvnum - 1 || lv[level].pos > 0);
		uint32_t dtime = lvmintime(lv[level].time, lv[level].len, lv[level].step);
		{
			uint32_t allmintime = lvmintime(lv[h.lvnum - 1].time, lv[h.lvnum - 1].pos, lv[h.lvnum - 1].step);
			if (allmintime > (size_t)lv[h.lvnum - 1].step - lv[0].step)
				allmintime -= lv[h.lvnum - 1].step - lv[0].step;
			else
				allmintime = lv[0].step;
			if (dtime < allmintime && dtime != 0)
				dtime = roundup(allmintime, lv[level].step);
		}
		assert(lv[level].time >= dtime);
		for (int dpos = (lv[level].pos - 1 - (lv[level].time - dtime) / lv[level].step + lv[level].len) % lv[level].len;
			dtime <= lv[level].time; dtime += lv[level].step, dpos++)
		{
			if (dpos >= lv[level].len)
				dpos = 0;
			float data = level == 0 ? value0[dpos] : (float)store2raw(value[level][dpos]);
			printf("\t%d\t%u\t%.3f\n", dpos, dtime, data);
		}
	}
}

// obtain best fit [start, end] and step (return value), based on suggested [start, end], curtime, and lenth
int32_t Alog::getrangeparam(uint32_t &start, uint32_t &end, uint32_t cur, int32_t len/* = 500*/)
{
	end = std::min(end, cur);
	if (start >= end)
	{
		start = end = 0;
		return 0;
	}
	// determine the level to use
	int level = 0;
	for (level = 0; level < ALOG_DEF_LVNUM - 1; ++level)
		if (cur - start <= (uint32_t)VPERIOD[level])
			break;
	// determine the step
	int32_t step = VSTEP[level];
	step = roundup((end - start) / len, step);
	// determine the real range
	start = roundup(start, step);
	end = std::max(start + step, roundup(end, step));
	return step;
}

int32_t gcd(int32_t a, int32_t b)
{
	while (b != 0)
	{
		int32_t n = a % b;
		a = b;
		b = n;
	}
	return a;
}

int Alog::getrange(uint32_t start, uint32_t end, int32_t step, float *buf) const
{
	if (start >= end || start % step != 0 || end % step != 0)
		PELOG_ERROR_RETURN((PLV_WARNING, "Alog getrange param error\n"), -1);
	// look for a matched level
	int32_t level = -1;
	for (level = h.lvnum - 1; level >= 0; --level)
	{
		if (lv[level].time > 0 && lvmintime(lv[level].time, lv[level].len, lv[level].step) <= start && step % lv[level].step == 0)
			break;
	}
	if (level < 0)	// if no perfectly suitible level found, look for an approximation
	{
		for (level = 0; level < h.lvnum - 1; ++level)	// look for the first level that covers the whole range
			if (lv[level].time == 0 || lvmintime(lv[level].time, lv[level].len, lv[level].step) <= start)
				break;
		if (level > 0 && lv[level].time == 0)	// no data, use the one that has most data
			level--;
		else if (lv[level].time == 0)	// no data at all
			;
		else if (lv[level].step < step) // look for the level with smaller step and largest gcd(step)
		{
			int32_t higcd = gcd(lv[level].step, step);
			for (int32_t nowlevel = level + 1; nowlevel < h.lvnum && lv[nowlevel].step < step; nowlevel++)
			{
				int32_t nowgcd = gcd(lv[nowlevel].step, step);
				if (nowgcd >= higcd)
				{
					level = nowlevel;
					higcd = nowgcd;
				}
			}
		}
		// else: level.step >= step, just use level
		else
			;
	}
	// fill the data
	uint32_t lvtime = lvmintime(lv[level].time, lv[level].len, lv[level].step);
	if (lvtime == 0)	// no data at all
	{
		assert(level == 0);
		for (; start < end; start += step, ++buf)
			*buf = NAN;
		return 0;
	}
	int fillidx = 0;
	// before earliest data
	for (; start < lvtime && start < end; start += step, ++buf)
		*buf = NAN;
	// data within lv[level]
	int lvpos = 0;
	if (lvtime > 0 && lvtime < end)
	{
		uint32_t startstep = start - start % lv[level].step;
		assert(startstep >= lvtime && start - startstep < (uint32_t)step);
		while (startstep >= lvtime + lv[level].step && start - (startstep - lv[level].step) < (uint32_t)step)
			startstep -= lv[level].step;
		// startstep is the smallest value that are >(start-step) && >= lvtime && matches lv[level].step
		assert(lvtime <= startstep && startstep <= lv[level].time);
		lvtime = startstep;
		lvpos = lvtimepos(lvtime, lv[level].time, lv[level].pos, lv[level].len, lv[level].step);
	}
	if (lv[level].step <= step)
	{
		for (; start <= lv[level].time && start < end; start += step, ++buf)
		{
			float val = 0;
			int cnt = 0;
			for (; lvtime <= start; lvtime += lv[level].step, lvpos = (lvpos + 1) % lv[level].len)
			{
				float fval = level == 0 ? value0[lvpos] : store2raw(value[level][lvpos]);
				if (!isnan(fval))
				{
					val += fval;
					cnt++;
				}
			}
			*buf = cnt > 0 ? val / cnt : NAN;
		}
	}
	else
	{
		for (; lvtime <= lv[level].time && lvtime < end; lvtime += lv[level].step, lvpos = (lvpos + 1) % lv[level].len)
		{
			float val = level == 0 ? value0[lvpos] : store2raw(value[level][lvpos]);
			for (; start <= lvtime && start < end; start += step, ++buf)
				*buf = val;
		}
	}
	// data not in lv[level] but in lv[0]
	lvtime = lvmintime(lv[0].time, lv[0].len, lv[0].step);
	if (level != 0 && start <= lv[0].time && lvtime < end)
	{
		assert(lvtime <= start && lvtime > 0);
		lvtime = start - start % lv[0].step;;
		lvpos = lvtimepos(lvtime, lv[0].time, lv[0].pos, lv[0].len, lv[0].step);
		if (lv[0].step <= step)
		{
			for (; start <= lv[0].time && start < end; start += step, ++buf)
			{
				float val = 0;
				int cnt = 0;
				for (; lvtime <= start; lvtime += lv[0].step, lvpos = (lvpos + 1) % lv[0].len)
				{
					float fval = value0[lvpos];
					if (!isnan(fval))
					{
						val += fval;
						cnt++;
					}
				}
				*buf = cnt > 0 ? val / cnt : NAN;
			}
		}
		else
		{
			for (; lvtime <= lv[0].time && lvtime < end; lvtime += lv[0].step, lvpos = (lvpos + 1) % lv[0].len)
			{
				float val = value0[lvpos];
				for (; start <= lvtime && start < end; start += step, ++buf)
					*buf = val;
			}
		}
	}
	// unavailable new data
	for (; start < end; start += step, ++buf)
		*buf = NAN;
	return 0;
}

int Alog::aggrrange(const std::vector<uint32_t> &ranges, float *buf) const
{
	if (ranges.size() < 2 || lv[0].time == 0)
		return 0;
	// determine the level to use
	int level = 0;
	for (level = lv.size() - 1; level >= 0; --level)	// find the first level with appropriate step
	{
		if (lv[level].step <= (int)(ranges[1] - ranges[0]) && lv[level].time > 0)
			break;
	}
	while (level < (int)lv.size() - 1 && lv[level].time > 0 && lvmintime(lv[level].time, lv[level].len, lv[level].step) > ranges[0])
		level++;
	if (lv[level].time == 0)
		level--;
	// calculate data
	size_t ridx = 1;
	uint32_t lvend = lvmintime(lv[level].time, lv[level].len, lv[level].step);
	uint32_t lvbegin = lvend - lv[level].step;
	int32_t lvpos = lvtimepos(lvend, lv[level].time, lv[level].pos, lv[level].len, lv[level].step);
	assert(lvend != 0 && lvend > lvbegin);
	// values before level data
	for (; ridx < ranges.size() && ranges[ridx] <= lvbegin; ridx++, buf++)
		*buf = 0;
	// values within level
	while (lvend <= ranges[ridx - 1] && lvend <= lv[level].time)	// locate the first value in level for range
	{
		lvbegin = lvend;
		lvend += lv[level].step;
		lvpos = lvpos < lv[level].len - 1 ? (lvpos + 1) : 0;
	}
	float rangeval = 0;
	for (; ridx < ranges.size() && lvend <= lv[level].time; ridx++, buf++)
	{
		uint32_t rgbegin = ranges[ridx - 1];
		uint32_t rgend = ranges[ridx];
		while (true)
		{
			assert(lvbegin < rgend && rgbegin < lvend);
			int covertime = std::min(lvend, rgend) - std::max(lvbegin, rgbegin);
			assert(covertime > 0);
			float stepval = level == 0 ? value0[lvpos] : store2raw(value[level][lvpos]);
			if (!isnan(stepval))
				rangeval += stepval * covertime;
			if (lvend <= rgend)	// range covers all current value, go to next value in level
			{
				lvbegin = lvend;
				lvend += lv[level].step;
				lvpos = lvpos < lv[level].len - 1 ? (lvpos + 1) : 0;
				if (lvend > lv[level].time || lvbegin >= rgend)	// no more values or range finished
					break;
			}
			else	// current value goes beyond range
				break;
		}
		if (lvend <= lv[level].time && lvend >= rgend || lvend > lv[level].time && lvbegin >= rgend)  // current level value go beyond current range, finish current range
		{
			assert(!isnan(rangeval));
			*buf = rangeval;
			rangeval = 0;
			continue;
		}
		else	// current range is not finished but no more level values in range
		{
			assert(lvend > lv[level].time);
			break;
		}
	}
	// now we have finished level, keep going on level0
	if (level != 0 && ridx < ranges.size() && lvend > lv[level].time && lvbegin + lv[0].step <= lv[0].time &&
		lvbegin + lv[0].step >= lvmintime(lv[0].time, lv[0].len, lv[0].step))
	{
		lvend = lvbegin + lv[0].step;
		lvpos = lvtimepos(lvend, lv[0].time, lv[0].pos, lv[0].len, lv[0].step);
		assert(lvpos >= 0);
		for (; ridx < ranges.size() && lvend <= lv[0].time; ridx++, buf++)
		{
			uint32_t rgbegin = ranges[ridx - 1];
			uint32_t rgend = ranges[ridx];
			assert(rgbegin % lv[0].step == 0 && rgend % lv[0].step == 0);
			assert(lvbegin < rgend && rgbegin < lvend);
			while (lvend <= rgend && lvend <= lv[0].time)
			{
				if (!isnan(value0[lvpos]))
					rangeval += value0[lvpos] * lv[0].step;
				lvbegin = lvend;
				lvend += lv[0].step;
				lvpos = lvpos < lv[0].len - 1 ? (lvpos + 1) : 0;
			}
			assert(!isnan(rangeval));
			*buf = rangeval;
			rangeval = 0;
		}
	}
	assert(!isnan(rangeval));
	if (rangeval > 0)
	{
		assert(ridx < ranges.size());
		*buf++ = rangeval;
		ridx++;
	}
	// having done with all avaiable data, fill in the left ranges with 0
	for (; ridx < ranges.size(); ridx++, buf++)
		*buf = 0;
	return 0;
}

