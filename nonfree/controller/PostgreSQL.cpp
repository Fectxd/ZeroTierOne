/* (c) ZeroTier, Inc.
 * See LICENSE.txt in nonfree/
 */

#ifdef ZT_CONTROLLER_USE_LIBPQ

#include "PostgreSQL.hpp"

#include "opentelemetry/trace/provider.h"

#include <nlohmann/json.hpp>

namespace ZeroTier {

PostgresMemberListener::PostgresMemberListener(
	DB* db,
	std::shared_ptr<ConnectionPool<PostgresConnection> > pool,
	const std::string& channel,
	uint64_t timeout)
	: NotificationListener()
	, _db(db)
	, _pool(pool)
	, _notification_timeout(timeout)
	, _listenerThread()
{
	_conn = _pool->borrow();
	_receiver = new _notificationReceiver<PostgresMemberListener>(this, *_conn->c, channel);
	_run = true;
	_listenerThread = std::thread(&PostgresMemberListener::listen, this);
}

PostgresMemberListener::~PostgresMemberListener()
{
	_run = false;
	if (_listenerThread.joinable()) {
		_listenerThread.join();
	}
	delete _receiver;
	if (_conn) {
		_pool->unborrow(_conn);
		_conn.reset();
	}
}

void PostgresMemberListener::listen()
{
	while (_run) {
		_conn->c->await_notification(_notification_timeout, 0);
	}
}

bool PostgresMemberListener::onNotification(const std::string& payload)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("PostgresMemberNotificationListener");
	auto span = tracer->StartSpan("PostgresMemberNotificationListener::onNotification");
	auto scope = tracer->WithActiveSpan(span);
	span->SetAttribute("payload", payload);

	Metrics::pgsql_mem_notification++;
	nlohmann::json tmp(nlohmann::json::parse(payload));
	nlohmann::json& ov = tmp["old_val"];
	nlohmann::json& nv = tmp["new_val"];
	nlohmann::json oldConfig, newConfig;
	if (ov.is_object())
		oldConfig = ov;
	if (nv.is_object())
		newConfig = nv;

	if (oldConfig.is_object() && newConfig.is_object()) {
		_db->save(newConfig, true);
	}
	else if (newConfig.is_object() && ! oldConfig.is_object()) {
		// new member
		Metrics::member_count++;
		_db->save(newConfig, true);
	}
	else if (! newConfig.is_object() && oldConfig.is_object()) {
		// member delete
		uint64_t networkId = OSUtils::jsonIntHex(oldConfig["nwid"], 0ULL);
		uint64_t memberId = OSUtils::jsonIntHex(oldConfig["id"], 0ULL);
		if (memberId && networkId) {
			_db->eraseMember(networkId, memberId);
		}
	}
	return true;
}

PostgresNetworkListener::PostgresNetworkListener(
	DB* db,
	std::shared_ptr<ConnectionPool<PostgresConnection> > pool,
	const std::string& channel,
	uint64_t timeout)
	: NotificationListener()
	, _db(db)
	, _pool(pool)
	, _notification_timeout(timeout)
	, _listenerThread()
{
	_conn = _pool->borrow();
	_receiver = new _notificationReceiver<PostgresNetworkListener>(this, *_conn->c, channel);
	_run = true;
	_listenerThread = std::thread(&PostgresNetworkListener::listen, this);
}

PostgresNetworkListener::~PostgresNetworkListener()
{
	_run = false;
	if (_listenerThread.joinable()) {
		_listenerThread.join();
	}
	delete _receiver;
	if (_conn) {
		_pool->unborrow(_conn);
		_conn.reset();
	}
}

void PostgresNetworkListener::listen()
{
	while (_run) {
		_conn->c->await_notification(_notification_timeout, 0);
	}
}

bool PostgresNetworkListener::onNotification(const std::string& payload)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("db_network_notification");
	auto span = tracer->StartSpan("db_network_notification::operator()");
	auto scope = tracer->WithActiveSpan(span);
	span->SetAttribute("payload", payload);

	Metrics::pgsql_net_notification++;
	nlohmann::json tmp(nlohmann::json::parse(payload));

	nlohmann::json& ov = tmp["old_val"];
	nlohmann::json& nv = tmp["new_val"];
	nlohmann::json oldConfig, newConfig;

	if (ov.is_object())
		oldConfig = ov;
	if (nv.is_object())
		newConfig = nv;

	if (oldConfig.is_object() && newConfig.is_object()) {
		std::string nwid = oldConfig["id"];
		span->SetAttribute("action", "network_change");
		span->SetAttribute("network_id", nwid);
		_db->save(newConfig, true);
	}
	else if (newConfig.is_object() && ! oldConfig.is_object()) {
		std::string nwid = newConfig["id"];
		span->SetAttribute("network_id", nwid);
		span->SetAttribute("action", "new_network");
		// new network
		_db->save(newConfig, true);
	}
	else if (! newConfig.is_object() && oldConfig.is_object()) {
		// network delete
		span->SetAttribute("action", "delete_network");
		std::string nwid = oldConfig["id"];
		span->SetAttribute("network_id", nwid);
		uint64_t networkId = Utils::hexStrToU64(nwid.c_str());
		span->SetAttribute("network_id_int", networkId);
		if (networkId) {
			_db->eraseNetwork(networkId);
		}
	}
	return true;
}

}	// namespace ZeroTier

#endif
