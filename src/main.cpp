#include <stdio.h>
#include "CollectdReceiver.h"
#include "GrafanaReader.h"
#include "Alog.h"

asio::io_service ioService;

void sighdl(int code)
{
	if (code == SIGINT || code == SIGTERM || code == SIGABRT)
	{
		PELOG_LOG((PLV_INFO, "Stopping\n"));
		ioService.stop();
	}
}

int main()
{
	AMon amon;
	amon.start();
	std::vector<std::unique_ptr<Worker>> workers;
//	std::unique_ptr<Worker> collectd = CollectdReceiver::byConfig(ioService, "conf/types.db", amon.gettaskq());
//	workers.push_back(std::move(collectd));
	std::unique_ptr<Worker> grafana = GrafanaReader::byConfig(ioService, amon.gettaskq());
	workers.push_back(std::move(grafana));
	for (const auto &worker: workers)
	{
		worker->start();
	}

	signal(SIGINT, sighdl);
	signal(SIGTERM, sighdl);
	signal(SIGABRT, sighdl);
	signal(SIGUSR1, sighdl);

	ioService.run();
	amon.stop();

//	// **** DEBUG
//	FILE *fp = fopen("data.dump", "rb");
//	fseek(fp, 0, SEEK_END);
//	long len = ftell(fp);
//	std::vector<uint8_t> buf(len);
//	fseek(fp, 0, SEEK_SET);
//	fread(&buf[0], 1, len, fp);
//	fclose(fp);
//	((CollectdReceiver *)worker.get())->parse(buf.data(), buf.size());

	return 0;
}