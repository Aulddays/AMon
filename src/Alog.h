#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <array>
#include "AMon.h"
#include "resguard.h"
#include "pe_log.h"

#define ALOG_DEF_LVNUM 4
static_assert(ALOG_DEF_LVNUM >= 2, "Too few levels");

class Alog
{
public:
	Alog();
	int init(const char *dir, const char *name, StoreType type);
	~Alog();

	int addv(uint32_t time, double value, StoreType type)
	{
		if (type != h.stype) 
			PELOG_ERROR_RETURN((PLV_ERROR, "Type not match %s\n", filename.c_str()), -1);
		return addv(time, value);
	}
	int addv(uint32_t time, double value);
	void dump();

	// obtain best fit [start, end] and step (return value), based on suggested [start, end], curtime, and lenth
	static int32_t getrangeparam(uint32_t &start, uint32_t &end, uint32_t cur, int32_t len=600);
	int getrange(uint32_t start, uint32_t end, int32_t step, float *buf);

private:
	int updatelevels() { for (int i = 1; i < h.lvnum; ++i) if (updatelevel(i) < 0) return -1; return 0; }
	int updatelevel(int level);
	int updatefile(bool force=false);

	std::string name;
	std::string filename;
	bool inited = false;
	// storage type and helpers
	uint16_t (*raw2store)(double) = NULL;
	double (*store2raw)(uint16_t) = NULL;
	bool (*testnan)(uint16_t) = NULL;
	uint16_t (*setnan)() = NULL;

	// storage file struct:
	// Header, LevelInfo[LEVEL_NUM], databuf
#pragma pack(push, 4)
	struct FileHeader
	{
		int32_t stype = AMON_AUINT;
		int32_t lvnum = ALOG_DEF_LVNUM;
	} h;
	struct LevelInfo
	{
		int32_t step = 0;	// time length (seconds) of each value in this level
		int32_t off = 0;	// pos of begin of storage, in bytes
		int32_t len = 0;	// number of values in this level
		uint32_t time = 0;	// (data) time of last write
		int32_t pos = 0;	// pos of next write, ie, pos_of_`time` + 1, in float/uint_16, with base `pos`
	};
	std::vector<LevelInfo> lv;
#pragma pack(pop)
	std::vector<float> value0;
	std::vector<std::vector<uint16_t>> value;
	// pending data info
	std::vector<int32_t> pending;	// number of pending values of each level
	bool ispending = false;	// are there any pending values (exclude level 0)
	uint32_t firsttime = UINT32_MAX;	// (data) time of first received value
	uint32_t writetime = 0;	// last file write (system clock) time
	uint32_t writestep = 0;	// last file write (data) time
};

