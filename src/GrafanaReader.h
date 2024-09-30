#pragma once

#include <memory>
#include <time.h>
#include <stdarg.h>
#include "AMon.h"
#include "asio.hpp"
#include "libconfig/libconfig.h"

class VarBuf
{
private:
	int cap = 256;
	int len = 0;
	char *buf = NULL;
public:
	VarBuf(): buf((char *)malloc(cap)) { clear(); }
	~VarBuf() { free(buf); }
	VarBuf &printf(const char *fmt, ...)
	{
		va_list v1, v2;
		va_start(v1, fmt);
		va_copy(v2, v1);
		int writelen = vsnprintf(buf + len, cap - len, fmt, v1);
		if (len + writelen >= cap)
		{
			expand(std::max(writelen * 2, cap / 3));
			writelen = vsnprintf(buf + len, cap - len, fmt, v2);
		}
		len += writelen;
		assert(len < cap);
		va_end(v1);
		va_end(v2);
		return *this;
	}
	VarBuf &append(const char *s, size_t alen)
	{
		if (len + (int)alen >= cap)
			expand(std::max((int)alen * 2, cap / 3));
		memcpy(buf + len, s, alen);
		len += alen;
		buf[len] = 0;
		assert(len < cap);
		return *this;
	}
	VarBuf &append(const char *s)
	{
		int alen = strlen(s);
		return append(s, alen);
	}
	VarBuf &expand(size_t size)
	{
		if (size > 0)
		{
			cap += size;
			buf = (char *)realloc(buf, cap);
		}
		return *this;
	}
	VarBuf &expandlen(int alen)
	{
		assert(len + alen < cap);
		len += alen;
		buf[len] = 0;
		return *this;
	}
	void clear() { *buf = 0; len = 0; }
	char *get() const { return buf; }
	int getlen() const { return len; }
	int getfree() const { return cap > 0 ? cap - len - 1 : 0; }
	operator char *() const { return buf; }
};

class GrafanaReader : public Reader
{
public:
	static std::unique_ptr<GrafanaReader> byConfig(asio::io_service &ioService, TaskQueue *taskq, config_setting_t *config)
	{
		int port;
		if (config_setting_lookup_int(config, "port", &port) == CONFIG_FALSE)
			PELOG_ERROR_RETURN((PLV_ERROR, "GrafanaReader port config missing\n"), NULL);
		auto ret = std::unique_ptr<GrafanaReader>(new GrafanaReader(ioService, taskq, port));
		return ret;
	}
	int start();
	int stop();

public:
	struct GRTask
	{
		enum { BUFSIZE = 1024 };
		GRTask(asio::io_service &ioService): socket(ioService), stime(time(NULL)) { }
		~GRTask() { if (socket.is_open()) socket.close(); }
		time_t stime;
		enum { GR_RECV, GR_PROC, GR_SEND } state = GR_RECV;
		asio::ip::tcp::socket socket;
		VarBuf buf;
		bool recvshutdown = false;
	};
private:
	GrafanaReader(asio::io_service &ioService, TaskQueue *taskq, int port):
		m_ioService(ioService), m_acceptor(ioService), port(port) { this->taskq = taskq; }
	void accept();
	void recv(std::shared_ptr<GRTask> task);
	int parsereq(std::shared_ptr<GRTask> grtask, TaskRead *amontask);
	int setresult(std::shared_ptr<GRTask> grtask, TaskRead *amontask);
	int response(std::shared_ptr<GRTask> grtask);
private:
	const char *m_name = "GrafanaReader";
	int port = 0;
	asio::io_service &m_ioService;
	asio::ip::tcp::acceptor m_acceptor;
//	asio::ip::tcp::socket m_socket;
};

