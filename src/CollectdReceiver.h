#pragma once

#include <memory>
#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include "AMon.h"
#include "asio.hpp"
#include "pe_log.h"

#define DATAINTERVAL 5

class CollectdReceiver: public Receiver
{
public:
	static std::unique_ptr<CollectdReceiver> byConfig(asio::io_service &ioService, const char *typesdbfile, TaskQueue *taskq)
	{
		auto ret = std::unique_ptr<CollectdReceiver>(new CollectdReceiver(ioService));
		if (ret->init(typesdbfile, taskq) != 0)
			PELOG_ERROR_RETURN((PLV_ERROR, "CollectdReceiver init failed\n"), NULL);
		return ret;
	}
	int start();
	int stop();
	enum ValType
	{
		COUNTER = 0,
		GAUGE = 1,
		DERIVE = 2,
		ABSOLUTE = 3,
	};
private:
	CollectdReceiver(asio::io_service &ioService):
		m_ioService(ioService), m_buf(1452), m_socket(m_ioService, asio::ip::udp::v6()) { }
	int init(const char *typesdbfile, TaskQueue *taskq);
	int parse(const uint8_t *data, size_t size, AMon *amon);
private:
	const char *m_name = "CollectdReceiver";
	asio::io_service &m_ioService;
	asio::ip::udp::socket m_socket;
	asio::ip::udp::endpoint m_remote;
	std::vector<uint8_t> m_buf;
	// types.db
	struct TypesdbVal
	{
		std::string name;
		ValType vtype;
		StoreType stype;
	};
	std::unordered_map<std::string, std::vector<TypesdbVal>> typesdb;
	// buffered history values for DERIVE values
	struct HistVal
	{
		uint32_t time = 0;
		double val = 0;
		bool operator <(const HistVal &r) const { return time < r.time; }
	};
	static const int HISTLEN = 3;
	std::map<std::string, std::array<HistVal, HISTLEN>> histvals;
private:
	int recv();
	void onRecv(const asio::error_code& error, size_t size);
	int process(struct CollectdRec &rec, AMon *amon);
};