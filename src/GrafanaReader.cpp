#include "GrafanaReader.h"

int GrafanaReader::start()
{
	asio::error_code ec;
	asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v6(), port);
	if (m_acceptor.open(endpoint.protocol(), ec))
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Failed opening acceptor (%d:%s)\n", m_name, ec.value(), ec.message().c_str()), 1);
	m_acceptor.set_option(asio::ip::v6_only(false));
	m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
	if(m_acceptor.bind(endpoint, ec))
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Failed binding to port (%d:%s)\n", m_name, ec.value(), ec.message().c_str()), 1);
	if(m_acceptor.listen(asio::socket_base::max_connections, ec))
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Failed listenint on port (%d:%s)\n", m_name, ec.value(), ec.message().c_str()), 1);
	PELOG_LOG((PLV_INFO, "Service \"%s\" started listening on port %d\n", m_name, port));
	accept();
	return 0;
}

int GrafanaReader::stop()
{
	if (m_acceptor.is_open())
		m_acceptor.close();
	return 0;
}

void GrafanaReader::accept()
{
	std::shared_ptr<GRTask> task = std::make_shared<GRTask>(m_ioService);
	PELOG_LOG((PLV_DEBUG, "[%s] async_accept begin wait %d\n", m_name, (int)task->socket.native_handle()));
	m_acceptor.async_accept(task->socket, [task, this](const asio::error_code& error) {
		if(error)
			PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] accept failed (%d:%s)\n", m_name, error.value(), error.message().c_str()));
		// test whether socket is ok
		asio::error_code ec;
		task->socket.remote_endpoint(ec);
		if (ec)	// socket is invalid
		{
			PELOG_LOG((PLV_ERROR, "[%s]: accept error. tag(%d)\n", m_name, (int)task->socket.native_handle()));
			task->socket.close();
		}
		else	// ok
		{
			task->socket.set_option(asio::ip::tcp::no_delay(true));
			recv(task);
		}
		accept();
	});
}

void GrafanaReader::recv(std::shared_ptr<GRTask> task)
{
	if (task->buf.getlen() > 10240)
		PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] request too long\n", m_name));
	if (task->buf.getfree() < 128)
		task->buf.expand(std::max(task->buf.getlen() / 3, 128));
	asio::async_read(task->socket,
		asio::buffer(task->buf.get() + task->buf.getlen(), task->buf.getfree()), asio::transfer_at_least(1),
		[task, this](const asio::error_code &error, size_t len) {
			if (error && error != asio::error::eof)
				PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] recv request failed %s\n", m_name, error.message().c_str()));
			if (error == asio::error::eof)
				task->recvshutdown = true;
			task->buf.expandlen(len);
//			if (task->buf.getlen() > 9 && task->buf.getlen() - len < 9 && strncmp(task->buf, "GET /amon", 9) != 0)
//				PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] Invalid request\n", m_name));
			uint8_t *pos = (uint8_t *)memmem(task->buf, task->buf.getlen(), "\r\n\r\n", 4);
			if (pos)	// request finished, make a task obj and send to task queue
			{
				*pos = 0;
				std::unique_ptr<TaskRead> amontask = std::make_unique<TaskRead>();
				amontask->parsereq = std::bind(&GrafanaReader::parsereq, this, task, std::placeholders::_1);
				amontask->response = std::bind(&GrafanaReader::setresult, this, task, std::placeholders::_1);
				//parsereq(task);
				taskq->put(std::move(amontask));
				return;
			}
			if (!pos && task->recvshutdown)
				PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] Invalid request1\n", m_name));
			recv(task);
	});
}

int GrafanaReader::parsereq(std::shared_ptr<GRTask> grtask, TaskRead *amontask)
{
	amontask->start = amontask->end = 0;
	amontask->names.clear();
	amontask->aggr = TaskRead::AMON_NOAGGR;
	grtask->status = GRTask::GR_REQERR;
	//fprintf(stderr, "toparse %s\n", grtask->buf.get());
	if (strncmp(grtask->buf, "GET /amon", 9) != 0)
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Invalid request1\n", m_name), -1);
	char *p = strchr(grtask->buf, '?');
	if (!p)
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Invalid request2\n", m_name), -1);
	p++;
	char *pe = strpbrk(p, " \t\r\n");
	if (pe)
		*pe = 0;
	std::string names;
	for (p = strtok_r(p, "&", &pe); p; p = strtok_r(NULL, "&", &pe))
	{
		if (strncmp(p, "start=", 6) == 0)
			amontask->start = strtoul(p + 6, NULL, 10);
		else if (strncmp(p, "end=", 4) == 0)
			amontask->end = strtoul(p + 4, NULL, 10);
		else if (strncmp(p, "name=", 5) == 0)
		{
			p += 5;
			for (char *pi = p, *po = p; true; )	// urlunescape
			{
				if (*pi == '%' && isxdigit(pi[1]) && isxdigit(pi[2]))
				{
					*po++ = strtoul(std::string{pi[1], pi[2], 0}.c_str(), NULL, 16);
					pi += 3;
				}
				else if (*pi)
					*po++ = *pi++;
				else
				{
					*po = 0;
					break;
				}
			}
			names = p;
			for (const char *pt = p, *pte = strchr(pt, ','); true; pt = pte + 1, pte = strchr(pt, ','))
			{
				if (pte)
					amontask->names.emplace_back(pt, pte);
				else
				{
					if (*pt)
						amontask->names.emplace_back(pt);
					break;
				}
			}
		}
		else if (strncmp(p, "aggr=", 5) == 0)
		{
			p += 5;
			constexpr const char *aggrnames[] = { "min", "hour", "day", "week", "month", "year" };
			static_assert(sizeof(aggrnames) / sizeof(aggrnames[0]) == TaskRead::AMON_AGGRNUM, "aggrnames mismatch");
			for (int i = 0; i < TaskRead::AMON_AGGRNUM; ++i)
			{
				if (strcmp(p, aggrnames[i]) == 0)
				{
					amontask->aggr = (TaskRead::Aggr)i;
					break;
				}
			}
			if (amontask->aggr == TaskRead::AMON_NOAGGR)
				PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Invalid aggr type %s\n", m_name, p), -1);
		}
	}	// for (p = strtok_r(p, "&", &pe); p; p = strtok_r(NULL, "&", &pe))
	// verification
	if (amontask->start <= 0 || amontask->end <= 0 || amontask->names.empty())
		PELOG_ERROR_RETURN((PLV_ERROR, "[%s] Incomplete request\n", m_name), -1);
	grtask->status = GRTask::GR_OK;
	PELOG_LOG((PLV_INFO, "[%s] req %s (%u:%u)\n", m_name, names.c_str(), amontask->start, amontask->end));
	return 0;
}

int GrafanaReader::setresult(std::shared_ptr<GrafanaReader::GRTask> grtask, TaskRead *amontask)
{
	grtask->buf.clear();
	if (grtask->status == GRTask::GR_OK)
	{
		assert(amontask->datatime.size() * amontask->names.size() == amontask->databuf.size());
		grtask->buf.append("{\"values\":[\n");
		for (size_t itime = 0; itime < amontask->datatime.size(); ++itime)
		{
			grtask->buf.printf(R"(%s{"time":%u)", itime == 0 ? "" : ",", amontask->datatime[itime]);
			for (size_t ival = 0; ival < amontask->names.size(); ++ival)
			{
				float val = amontask->databuf[ival * amontask->datatime.size() + itime];
				if (!isnan(val))
					grtask->buf.printf(R"( ,"val%d":%f)", (int)ival, val);
			}
			grtask->buf.append("}\n");
		}
		grtask->buf.append("]}\n");
	}
	m_ioService.post(std::bind(&GrafanaReader::response, this, grtask));
	return 0;
}

int GrafanaReader::response(std::shared_ptr<GrafanaReader::GRTask> grtask)
{
	VarBuf header;
	if (grtask->status == GRTask::GR_OK)
		header.printf("HTTP/1.0 200 OK\r\ncontent-type: application/json; charset=utf-8\r\ncontent-length: %d\r\n\r\n", grtask->buf.getlen());
	else
		header.append("HTTP/1.0 404 \r\ncontent-length: 0\r\n\r\n");
	int headerlen = header.getlen();
	asio::async_write(grtask->socket, asio::buffer(header.get(), headerlen),
		[this, grtask, headerlen](const asio::error_code &err, size_t len) {
			if (err)
				PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] response header failed (%d:%s)\n", m_name, err.value(), err.message().c_str()));
			assert((int)len == headerlen);
			if (grtask->status == GRTask::GR_OK && grtask->buf.getlen() > 0)
			{
				asio::async_write(grtask->socket, asio::buffer(grtask->buf.get(), grtask->buf.getlen()),
					[this, grtask](const asio::error_code &err, size_t len) {
						if (err)
							PELOG_ERROR_RETURNVOID((PLV_ERROR, "[%s] response content failed (%d:%s)\n", m_name, err.value(), err.message().c_str()));
						});
			}
	});
	return 0;
}

