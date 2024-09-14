#include "CollectdReceiver.h"
#include <endian.h>
#include <map>
#include <set>
#include "resguard.h"

int CollectdReceiver::start()
{
	// start receive
	fprintf(stderr, "start\n");
	asio::error_code ec;
	m_socket.set_option(asio::socket_base::reuse_address(true));
	m_socket.set_option(asio::ip::v6_only(false));
	if (m_socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v6(), 25827), ec))
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] UDP bind failed: %s\n",
		m_name, ec.message().c_str()), ec.value());
	int res = 0;
	if ((res = recv()) != 0)
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] start failed.\n", m_name), res);
	return 0;
}

int CollectdReceiver::stop()
{
	if (m_socket.is_open())
		m_socket.close();
	return 0;
}

int CollectdReceiver::recv()
{
	fprintf(stderr, "recv\n");
	m_remote = asio::ip::udp::endpoint();	// clear remote
	m_buf.resize(std::max((size_t)1452, m_buf.size()));
	m_socket.async_receive_from(asio::buffer(m_buf, m_buf.size()), m_remote,
		std::bind(&CollectdReceiver::onRecv, this, std::placeholders::_1 /*error*/, std::placeholders::_2 /*bytes_transferred*/));
	return 0;
}

void CollectdReceiver::onRecv(const asio::error_code& error, size_t size)
{
	if (error)
		PELOG_LOG((PLV_ERROR, "[%s] recv failed. %s\n", m_name, error.message().c_str()));
	else
	{
		auto addr6 = m_remote.address().to_v6();
		std::string addrs = addr6.is_v4_mapped() || addr6.is_v4_compatible() ? addr6.to_v4().to_string() : addr6.to_string();
		PELOG_LOG((PLV_VERBOSE, "[%s] Got packet from %s:%d size " PL_SIZET "\n", m_name,
			addrs.c_str(), (int)m_remote.port(), size));
		std::unique_ptr<TaskWrite> dwrite = TaskWrite::alloc(size);
		dwrite->processor = std::bind(&CollectdReceiver::parse, this, std::placeholders::_1, std::placeholders::_2);
		memcpy(dwrite->data, &m_buf[0], size);
		taskq->put(std::move(dwrite));
		//parse(&m_buf[0], size);
//		FILE *fp = fopen("data.dump", "wb");
//		fwrite(&m_buf[0], 1, size, fp);
//		fclose(fp);
//		exit(0);
	}
	if (m_socket.is_open() && recv() != 0)
		PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] start recv failed.\n", m_name));
}

uint64_t fmttime(time_t time)
{
	tm ttm;
	localtime_r(&time, &ttm);
	return (ttm.tm_year + 1900) * (uint64_t)10000000000 + (ttm.tm_mon + 1) * 100000000 + ttm.tm_mday * 1000000 +
		ttm.tm_hour * 10000 + ttm.tm_min * 100 + ttm.tm_sec;
}


// helper parsers
std::string parsestring(const uint8_t *data, uint16_t len)
{
	if (data[len] == 0)
		return (const char *)data;
	return std::string((const char *)data, len); 
}
uint64_t parseint(const uint8_t *data, uint16_t len)
{
	if (len != 8)
		PELOG_ERROR_RETURN((PLV_ERROR, "parse invalid int %d\n", (int)len), 0);
	return be64toh(*(const uint64_t *)data);
}
int parsevalues(const uint8_t *data, uint16_t len, std::vector<double> &res)
{
	res.clear();
	if (len < sizeof(uint16_t))
		PELOG_ERROR_RETURN((PLV_ERROR, "parse value no num\n", (int)len), -1);
	uint16_t num = htons(*(const uint16_t *)data);
	data += sizeof(uint16_t);
	len -= sizeof(uint16_t);
	if (len != num * (1 + 8))
		PELOG_ERROR_RETURN((PLV_ERROR, "parse value too few values %d:%d\n", (int)num, (int)len), -1);
	const uint8_t *pval = data + num;
	for (int i = 0; i < num; ++i, ++data, pval += 8)
	{
		double val = 0;
		if (*data == CollectdReceiver::COUNTER || *data == CollectdReceiver::ABSOLUTE)	// COUNTER or ABSOLUTE
			val = be64toh(*(const uint64_t *)pval);
		else if (*data == CollectdReceiver::GAUGE)	// GAUGE
			val = *(const double *)pval;
		else if (*data == CollectdReceiver::DERIVE)	// DERIVE
			val = (int64_t)be64toh(*(const uint64_t *)pval);
		else
			PELOG_ERROR_RETURN((PLV_ERROR, "parse value unsupported datatype %d\n", (int)*data), -1);
//		PELOG_LOG((PLV_DEBUG, "val_type %d val %f\n",(int)*data, val));
		res.push_back(val);
	}
	return 0;
}

struct CollectdRec
{
	std::string host;
	std::string plugin;
	std::string instance;
	std::string type;
	std::string subtype;
	uint32_t interval;
	uint32_t time;
	std::vector<double> values;
};

// part types from collectd
#define TYPE_HOST 0x0000
#define TYPE_TIME 0x0001
#define TYPE_TIME_HR 0x0008
#define TYPE_PLUGIN 0x0002
#define TYPE_PLUGIN_INSTANCE 0x0003
#define TYPE_TYPE 0x0004
#define TYPE_TYPE_INSTANCE 0x0005
#define TYPE_VALUES 0x0006
#define TYPE_INTERVAL 0x0007
#define TYPE_INTERVAL_HR 0x0009
#define TYPE_MESSAGE 0x0100
#define TYPE_SEVERITY 0x0101
#define TYPE_SIGN_SHA256 0x0200
#define TYPE_ENCR_AES256 0x0210

int CollectdReceiver::parse(const uint8_t *data, size_t size)
{
	CollectdRec rec;
	PELOG_LOG((PLV_DEBUG, "to parse packet size %d\n", size));
	const uint8_t *p = data;
	while (p < data + size)
	{
		// parse one part
		uint16_t type = ntohs(*(uint16_t *)p);
		uint16_t plen = ntohs(*(uint16_t *)(p + 2));
		const uint8_t *pdata = p + 4;
		uint16_t pdatalen = plen - 4;
//		PELOG_LOG((PLV_INFO, "Packet part: len(%d), type(%d), datalen(%d)\n", (int)plen, (int)type, (int)pdatalen));
		if (plen == 0 || p + plen > data + size)
			PELOG_ERROR_RETURN((PLV_ERROR, "Invalid packet part len %d\n", (int)plen), -1);

		switch (type)
		{
		case TYPE_HOST:
			rec.host = parsestring(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse host: %s\n", rec.host.c_str()));
			break;
		case TYPE_PLUGIN:
			rec.plugin = parsestring(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse plugin: %s\n", rec.plugin.c_str()));
			break;
		case TYPE_PLUGIN_INSTANCE:
			rec.instance = parsestring(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse plugin_instance: %s\n", rec.instance.c_str()));
			break;
		case TYPE_TYPE:
			rec.type = parsestring(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse type: %s\n", rec.type.c_str()));
			break;
		case TYPE_TYPE_INSTANCE:
			rec.subtype = parsestring(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse subtype: %s\n", rec.subtype.c_str()));
			break;
		case TYPE_TIME_HR:
			rec.time = (uint32_t)(parseint(pdata, pdatalen) >> 30);
//			PELOG_LOG((PLV_INFO, "parse time hr: %u\n", rec.time));
			break;
		case TYPE_TIME:
			rec.time = (uint32_t)parseint(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse time: %u\n", rec.time));
			break;
		case TYPE_INTERVAL_HR:
			rec.interval = (uint32_t)(parseint(pdata, pdatalen) >> 30);
//			PELOG_LOG((PLV_INFO, "parse interval hr: %u\n", rec.interval));
			break;
		case TYPE_INTERVAL:
			rec.interval = (uint32_t)parseint(pdata, pdatalen);
//			PELOG_LOG((PLV_INFO, "parse interval: %u\n", rec.interval));
			break;
		case TYPE_VALUES:
//			PELOG_LOG((PLV_INFO, "parse values len %d\n", (int)pdatalen));
			parsevalues(pdata, pdatalen, rec.values);
			break;
		default:
			PELOG_LOG((PLV_INFO, "Packet part: len(%d), type(%d), datalen(%d)\n", (int)plen, (int)type, (int)pdatalen));
			break;
		}
		p += plen;
		if (type != TYPE_VALUES)	// more fields needed
			continue;

		// process the values
		{	// DEBUG log
			char valbuf[1024] = { 0 };
			for (size_t i = 0, valplen = 0; i < rec.values.size() && valplen + 4 < 1024; ++i)
				valplen += snprintf(valbuf + valplen, 1024 - valplen, "%s%.1f", i == 0 ? "" : ", ", rec.values[i]);
			PELOG_LOG((PLV_INFO, "PARSE value %u(%llu) %s.%s.%s.%s.%s: %s\n",
					   rec.time, fmttime(rec.time), rec.host.c_str(), rec.plugin.c_str(), rec.instance.c_str(),
					   rec.type.c_str(), rec.subtype.c_str(), valbuf));
		}
		process(rec);
	}

	return 0;
}

int CollectdReceiver::init(const char *typesdbfile, TaskQueue *taskq)
{
	this->taskq = taskq;
	// load types.db
	typesdb.clear();
	FILEGuard fp = fopen(typesdbfile, "rb");
	if (!fp)
		PELOG_ERROR_RETURN((PLV_ERROR, "CollectdReceiver init types.db failed %s\n", typesdbfile), -1);
	char buf[1024];
	while (fgets(buf, 1024, fp))
	{
		char *p = strpbrk(buf, "\r\n");
		if (p)
			*p = 0;
		if (!*buf || *buf == '#')
			continue;
		// type key
		p = buf;
		char *pe = buf;
		while (*pe && !isspace(*pe))
			++pe;
		if (!*pe || pe == p)
			continue;
		std::string key(p, pe);
		// values
		std::vector<TypesdbVal> values;
		bool valvalid = false;
		p = pe;
		while (*p)	// parse values
		{
			// name
			while (*p && isspace(*p))
				++p;
			if (!*p)
			{
				valvalid = true;
				break;
			}
			for (pe = p; *pe && *pe != ':';)
				++pe;
			if (*pe != ':')
				break;
			*pe = 0;
			const char *valname = p;
			// type
			p = pe + 1;
			for (pe = p; *pe && *pe != ':';)
				++pe;
			if (*pe != ':')
				break;
			*pe = 0;
			static const std::map<std::string, ValType> vtypes = {
				{ "COUNTER", COUNTER }, { "GAUGE", GAUGE }, { "DERIVE", DERIVE }, { "ABSOLUTE", ABSOLUTE }, 
			};
			if (vtypes.count(p) == 0)
				break;
			ValType type = vtypes.at(p);
			// min
			p = pe + 1;
			for (pe = p; *pe && *pe != ':';)
				++pe;
			if (*pe != ':')
				break;
			*pe = 0;
			if (*p == 'U')
			{
				PELOG_LOG((PLV_WARNING, "types.db unsupported range %s\n", key.c_str()));
				break;
			}
			int64_t minv = strtoll(p, NULL, 10);
			// max
			p = pe + 1;
			for (pe = p; *pe && *pe != ',';)
				++pe;
			if (*pe && *pe != ',')
				break;
			uint64_t maxv = *p == 'U' ? (uint64_t)-1 : strtoull(p, NULL, 10);
			if (minv < -65504 || minv < 0 && (*p == 'U' || maxv > 65504))	// unsupported range by fp16
			{
				PELOG_LOG((PLV_WARNING, "types.db unsupported range %s\n", key.c_str()));
				break;
			}
			StoreType stype = minv < 0 || (*p != 'U' && maxv <= 1000) ? ST_FP16 : ST_AUINT;
			p = *pe == ',' ? pe + 1 : pe;
			values.emplace_back(TypesdbVal{valname, type, stype});
			//PELOG_LOG((PLV_DEBUG, "%s: %s %d %d\n", key.c_str(), valname, type, stype));
			if (!*p)
				valvalid = true;
		}	// while (*p)	// parse values
		if (!valvalid)
		{
			PELOG_LOG((PLV_WARNING, "types.db parse error %s\n", key.c_str()));
			continue;
		}

//		// DEBUG log, borrow and overwrite `buf`
//		{
//			int loglen = 0;
//			for (const TypesdbVal &val: values)
//			{
//				static const char *vtypename[] = {"COUNTER", "GAUGE", "DERIVE", "ABSOLUTE"};
//				static const char *stypename[] = {"AUINT", "FP16"};
//				loglen += snprintf(buf + loglen, 1024 - loglen, "\t%s:%s:%s", val.name.c_str(), vtypename[val.vtype], stypename[val.stype]);
//				if (loglen >= 1024)
//					break;
//			}
//			PELOG_LOG((PLV_DEBUG, "typesdb: %s%s\n", key.c_str(), buf));
//		}

		typesdb.emplace(std::move(key), std::move(values));
	}	// while (fgets(buf, 1024, fp))
	fp.release();
	return 0;
}

int CollectdReceiver::process(struct CollectdRec &rec)
{
	static const std::set<std::tuple<std::string, std::string, std::string>> accepted = {
		std::make_tuple("interface", "if_octets", ""),
		std::make_tuple("cpu", "percent", "idle"),
		std::make_tuple("memory", "percent", "free"),
		std::make_tuple("load", "load", ""),
	};
	if (accepted.find(std::make_tuple(rec.plugin, rec.type, rec.subtype)) == accepted.end())
		return 0;	// skip unnecessary data

	// match types.db
	auto itype = typesdb.find(rec.type);
	if (itype == typesdb.end() || itype->second.size() != rec.values.size())
		PELOG_ERROR_RETURN((PLV_ERROR, "types.db mismatch %s.%s.%s.%s.%s %d\n",
			rec.host.c_str(), rec.plugin.c_str(), rec.instance.c_str(),
			rec.type.c_str(), rec.subtype.c_str(), (int)rec.values.size()), -1);
	std::vector<TypesdbVal> typedb = itype->second;	// make a copy, because some special rules may apply

	PELOG_LOG((PLV_DEBUG, "Processing %s.%s.%s.%s.%s\n",
		rec.host.c_str(), rec.plugin.c_str(), rec.instance.c_str(),
		rec.type.c_str(), rec.subtype.c_str()));
	// apply special type rules
	if (rec.plugin == "load" && rec.type == "load")	// only keep 1min load
	{
	   rec.values.resize(1);
	   typedb.resize(1);
	}
	if (rec.plugin == "cpu" && rec.type == "percent" && rec.subtype == "idle" ||
		rec.plugin == "memory" && rec.type == "percent" && rec.subtype == "free")
	{
		rec.subtype = "usage";
		rec.values[0] = 100 - rec.values[0];
	}

	// process values
	for (size_t ival = 0; ival < typedb.size(); ++ival)
	{
		// name
		std::string name = rec.host + "." + rec.plugin;
		if (!rec.instance.empty())
			name += "." + rec.instance;
		if (!rec.type.empty())
			name += "." + rec.type;
		if (!rec.subtype.empty())
			name += "." + rec.subtype;
		if (typedb.size() > 1)
			name += "." + typedb[ival].name;
		// time, round to DATAINTERVAL
		uint32_t time = (rec.time + DATAINTERVAL / 2) / DATAINTERVAL * DATAINTERVAL;
		// value
		double value = rec.values[ival];
		// process one value
		if (typedb[ival].vtype == DERIVE)	// DERIVE: cumulative -> average
		{
			std::array<HistVal, HISTLEN> &bufval = histvals[name];	// get buffered history values
			// find the last element in bufval with time before current
			if (time < bufval[0].time)
			{
				PELOG_LOG((PLV_WARNING, "CollectdReceiver::process outdated value %s %llu %llu\n",
						name.c_str(), fmttime(time), fmttime(bufval[0].time)));
				continue;
			}
			int bufidx = 0;
			while (bufidx < HISTLEN && bufval[bufidx].time < time)
				++bufidx;
			if (bufidx < HISTLEN && bufval[bufidx].time == time)	// already got the same value
				continue;
			--bufidx;
			assert(bufval[bufidx].time < time && bufval[bufidx].time % DATAINTERVAL == 0 && time % DATAINTERVAL == 0);
			if (time - bufval[bufidx].time <= 60 && bufval[bufidx].val <= value)
			{
				double avg = (value - bufval[bufidx].val) / (time - bufval[bufidx].time);
				for (uint32_t steptime = bufval[bufidx].time + DATAINTERVAL; steptime <= time; steptime += DATAINTERVAL)
				{
					PELOG_LOG((PLV_WARNING, "CollectdReceiver ADD value %s %llu %.3f\n", name.c_str(), fmttime(steptime), avg));
					// TODO: ADD
				}
			}
			assert(bufidx == HISTLEN - 1 || bufval[bufidx + 1].time > time && bufval[bufidx + 1].time % DATAINTERVAL == 0);
			if (bufidx < HISTLEN - 1 && bufval[bufidx + 1].time - time <= 60 && value <= bufval[bufidx + 1].val)
			{
				double avg = (bufval[bufidx + 1].val - value) / (bufval[bufidx + 1].time - time);
				for (uint32_t steptime = time + DATAINTERVAL; steptime <= bufval[bufidx + 1].time; steptime += DATAINTERVAL)
				{
					PELOG_LOG((PLV_WARNING, "CollectdReceiver ADD value %s %llu %.3f\n", name.c_str(), fmttime(steptime), avg));
					// TODO: ADD
				}
			}
			// add the new value to buffer
			bufval[0].time = time;
			bufval[0].val = value;
			std::sort(bufval.begin(), bufval.end());
		}	// if (rec.type == DERIVE)	// DERIVE: cumulative -> average
		else
		{
			PELOG_LOG((PLV_WARNING, "CollectdReceiver ADD value %s %llu %.3f\n", name.c_str(), fmttime(time), value));
		}
	}

	return 0;
}

