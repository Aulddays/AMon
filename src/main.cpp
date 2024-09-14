#include <stdio.h>
#include "CollectdReceiver.h"
#include "GrafanaReader.h"

std::vector<std::unique_ptr<Worker>> workers;

int main()
{
	asio::io_service ioService;
	AMon amon;
	amon.start();
//	std::unique_ptr<Worker> collectd = CollectdReceiver::byConfig(ioService, "conf/types.db", amon.gettaskq());
//	workers.push_back(std::move(collectd));
	std::unique_ptr<Worker> grafana = GrafanaReader::byConfig(ioService, amon.gettaskq());
	workers.push_back(std::move(grafana));
	for (const auto &worker: workers)
	{
		worker->start();
	}
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