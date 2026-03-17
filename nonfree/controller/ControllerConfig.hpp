#ifndef CONTROLLER_CONFIG_HPP
#define CONTROLLER_CONFIG_HPP

#include "Redis.hpp"

#include <string>

namespace ZeroTier {

struct PubSubConfig {
	std::string project_id;
	std::string member_change_recv_topic;
	std::string member_change_send_topic;
	std::string network_change_recv_topic;
	std::string network_change_send_topic;
	std::string sso_send_topic;
	std::string sso_recv_topic;
};

struct BigTableConfig {
	std::string project_id;
	std::string instance_id;
	std::string table_id;
};

struct ControllerConfig {
	bool ssoEnabled;
	std::string listenMode;
	std::string statusMode;
	std::string assignedCentralVersion;
	RedisConfig* redisConfig;
	PubSubConfig* pubSubConfig;
	BigTableConfig* bigTableConfig;

	ControllerConfig()
		: ssoEnabled(false)
		, listenMode("")
		, statusMode("")
		, assignedCentralVersion("all")
		, redisConfig(nullptr)
		, pubSubConfig(nullptr)
		, bigTableConfig(nullptr)
	{
	}

	~ControllerConfig()
	{
		if (redisConfig) {
			delete redisConfig;
			redisConfig = nullptr;
		}
		if (pubSubConfig) {
			delete pubSubConfig;
			pubSubConfig = nullptr;
		}
		if (bigTableConfig) {
			delete bigTableConfig;
			bigTableConfig = nullptr;
		}
	}
};

}	// namespace ZeroTier
#endif	 // CONTROLLER_CONFIG_HPP