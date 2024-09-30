#include <stdio.h>
#include "CollectdReceiver.h"
#include "GrafanaReader.h"
#include "Alog.h"
#include "libconfig/libconfig.h"
#include "resguard.h"

asio::io_service ioService;

void sighdl(int code)
{
	if (code == SIGINT || code == SIGTERM || code == SIGABRT)
	{
		PELOG_LOG((PLV_INFO, "Stopping\n"));
		ioService.stop();
	}
}

int main(int argc, char **argv)
{
	config_t config;
	config_init(&config);
	ResGuard<config_t> config_guard(&config, config_destroy);
	if (CONFIG_FALSE == config_read_file(&config, argc > 1 ? argv[1] : "conf/amon.conf"))
	{
		PELOG_ERROR_RETURN((PLV_ERROR, "Error loading config file (line %d): %s\n",
			config_error_line(&config), config_error_text(&config)), -1);
	}
	// logging & debugging
	pelog_setfile_rotate(config_get_int(&config, "general.logrotate_filesize_kb", -1),
		config_get_int(&config, "general.logrotate_history_num", -1),
		config_get_string(&config, "general.logfile", ""),
		config_get_bool(&config, "general.loglinebuf", false));
	pelog_setlevel(config_get_string(&config, "general.loglevel", "TRC"));

	// AMon
	std::string datadir = config_get_string(&config, "general.datadir", ".");
	AMon amon(datadir.c_str());
	amon.start();

	std::vector<std::unique_ptr<Worker>> workers;
	// CollectdReceiver
	std::unique_ptr<Worker> collectd = CollectdReceiver::byConfig(
		ioService, amon.gettaskq(), config_lookup(&config, "workers.CollectdReceiver"));
	if (!collectd)
		PELOG_ERROR_RETURN((PLV_ERROR, "CollectdReceiver creation failed"), -1);
	workers.push_back(std::move(collectd));
	// GrafanaReader
	std::unique_ptr<Worker> grafana = GrafanaReader::byConfig(
		ioService, amon.gettaskq(), config_lookup(&config, "workers.GrafanaReader"));
	if (!grafana)
		PELOG_ERROR_RETURN((PLV_ERROR, "GrafanaReader creation failed"), -1);
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