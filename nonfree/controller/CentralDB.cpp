/*
 * Copyright (c)2019 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2026-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "CentralDB.hpp"

#ifdef ZT_CONTROLLER_USE_LIBPQ

#include "../../node/Constants.hpp"
#include "../../node/SHA512.hpp"
#include "../../version.h"
#include "BigTableStatusWriter.hpp"
#include "ControllerChangeNotifier.hpp"
#include "ControllerConfig.hpp"
#include "CtlUtil.hpp"
#include "EmbeddedNetworkController.hpp"
#include "OtelCarrier.hpp"
#include "PostgresStatusWriter.hpp"
#include "PubSubListener.hpp"
#include "PubSubWriter.hpp"
#include "Redis.hpp"
#include "RedisListener.hpp"
#include "RedisStatusWriter.hpp"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/trace/provider.h"

#include <chrono>
#include <climits>
#include <iomanip>
#include <libpq-fe.h>
#include <optional>
#include <pqxx/pqxx>
#include <rustybits.h>
#include <sstream>

// #define REDIS_TRACE 1

using json = nlohmann::json;

using namespace ZeroTier;

using Attrs = std::vector<std::pair<std::string, std::string> >;
using Item = std::pair<std::string, Attrs>;
using ItemStream = std::vector<Item>;

CentralDB::CentralDB(
	const Identity& myId,
	const char* connString,
	int listenPort,
	CentralDB::ListenerMode listenMode,
	CentralDB::StatusWriterMode statusMode,
	const ControllerConfig* cc)
	: DB()
	, _listenerMode(listenMode)
	, _statusWriterMode(statusMode)
	, _cc(cc)
	, _pool()
	, _myId(myId)
	, _myAddress(myId.address())
	, _ready(0)
	, _connected(1)
	, _run(1)
	, _waitNoticePrinted(false)
	, _listenPort(listenPort)
	, _redis(NULL)
	, _cluster(NULL)
	, _redisMemberStatus(false)
	, _smee(NULL)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::CentralDB");
	auto scope = tracer->WithActiveSpan(span);

	rustybits::init_async_runtime();

	char myAddress[64];
	_myAddressStr = myId.address().toString(myAddress);
	_connString = std::string(connString);

	auto f = std::make_shared<PostgresConnFactory>(_connString);
	_pool =
		std::make_shared<ConnectionPool<PostgresConnection> >(15, 5, std::static_pointer_cast<ConnectionFactory>(f));

	memset(_ssoPsk, 0, sizeof(_ssoPsk));
	char* const ssoPskHex = getenv("ZT_SSO_PSK");
#ifdef ZT_TRACE
	fprintf(stderr, "ZT_SSO_PSK: %s\n", ssoPskHex);
#endif
	if (ssoPskHex) {
		// SECURITY: note that ssoPskHex will always be null-terminated if libc actually
		// returns something non-NULL. If the hex encodes something shorter than 48 bytes,
		// it will be padded at the end with zeroes. If longer, it'll be truncated.
		Utils::unhex(ssoPskHex, _ssoPsk, sizeof(_ssoPsk));
	}
	const char* redisMemberStatus = getenv("ZT_REDIS_MEMBER_STATUS");
	if (redisMemberStatus && (strcmp(redisMemberStatus, "true") == 0)) {
		_redisMemberStatus = true;
		fprintf(stderr, "Using redis for member status\n");
	}

	if ((listenMode == LISTENER_MODE_REDIS || statusMode == STATUS_WRITER_MODE_REDIS) && _cc->redisConfig != NULL) {
		auto innerspan = tracer->StartSpan("CentralDB::CentralDB::configureRedis");
		auto innerscope = tracer->WithActiveSpan(innerspan);

		sw::redis::ConnectionOptions opts;
		sw::redis::ConnectionPoolOptions poolOpts;
		opts.host = _cc->redisConfig->hostname;
		opts.port = _cc->redisConfig->port;
		opts.password = _cc->redisConfig->password;
		opts.db = 0;
		opts.keep_alive = true;
		opts.connect_timeout = std::chrono::seconds(3);
		poolOpts.size = 25;
		poolOpts.wait_timeout = std::chrono::seconds(5);
		poolOpts.connection_lifetime = std::chrono::minutes(3);
		poolOpts.connection_idle_time = std::chrono::minutes(1);
		if (_cc->redisConfig->clusterMode) {
			innerspan->SetAttribute("cluster_mode", "true");
			fprintf(stderr, "Using Redis in Cluster Mode\n");
			_cluster = std::make_shared<sw::redis::RedisCluster>(opts, poolOpts);
		}
		else {
			innerspan->SetAttribute("cluster_mode", "false");
			fprintf(stderr, "Using Redis in Standalone Mode\n");
			_redis = std::make_shared<sw::redis::Redis>(opts, poolOpts);
		}
	}

	_readyLock.lock();

	fprintf(
		stderr, "[%s] NOTICE: %.10llx controller PostgreSQL waiting for initial data download..." ZT_EOL_S,
		::_timestr(), (unsigned long long)_myAddress.toInt());
	_waitNoticePrinted = true;

	initializeNetworks();
	initializeMembers();

	_heartbeatThread = std::thread(&CentralDB::heartbeat, this);

	switch (listenMode) {
		case LISTENER_MODE_REDIS:
			fprintf(stderr, "Using Redis for change listeners\n");
			if (_cc->redisConfig != NULL) {
				if (_cc->redisConfig->clusterMode) {
					_membersDbWatcher = std::make_shared<RedisMemberListener>(_myAddressStr, _cluster, this);
					_networksDbWatcher = std::make_shared<RedisNetworkListener>(_myAddressStr, _cluster, this);
				}
				else {
					_membersDbWatcher = std::make_shared<RedisMemberListener>(_myAddressStr, _redis, this);
					_networksDbWatcher = std::make_shared<RedisNetworkListener>(_myAddressStr, _redis, this);
				}
			}
			else {
				throw std::runtime_error("CentralDB: Redis listener mode selected but no Redis configuration provided");
			}
		case LISTENER_MODE_PUBSUB:
			fprintf(stderr, "Using PubSub for change listeners\n");
			if (cc->pubSubConfig != NULL) {
				_membersDbWatcher = std::make_shared<PubSubMemberListener>(
					_myAddressStr, cc->pubSubConfig->project_id, cc->pubSubConfig->member_change_recv_topic, this);
				_networksDbWatcher = std::make_shared<PubSubNetworkListener>(
					_myAddressStr, cc->pubSubConfig->project_id, cc->pubSubConfig->network_change_recv_topic, this);
				_changeNotifier = std::make_shared<PubSubChangeNotifier>(
					_myAddressStr, cc->pubSubConfig->project_id, cc->pubSubConfig->member_change_send_topic,
					cc->pubSubConfig->network_change_send_topic);
			}
			else {
				throw std::runtime_error(
					"CentralDB: PubSub listener mode selected but no PubSub configuration provided");
			}
			break;
		case LISTENER_MODE_PGSQL:
		default:
			fprintf(stderr, "Using PostgreSQL for change listeners\n");
			_membersDbWatcher = std::make_shared<PostgresMemberListener>(this, _pool, "member_" + _myAddressStr, 5);
			_networksDbWatcher = std::make_shared<PostgresNetworkListener>(this, _pool, "network_" + _myAddressStr, 5);
			break;
	}

	std::shared_ptr<PubSubWriter> pubsubWriter;
	switch (statusMode) {
		case STATUS_WRITER_MODE_REDIS:
			fprintf(stderr, "Using Redis for status writer\n");
			if (_cc->redisConfig != NULL) {
				if (_cc->redisConfig->clusterMode) {
					_statusWriter = std::make_shared<RedisStatusWriter>(_cluster, _myAddressStr);
				}
				else {
					_statusWriter = std::make_shared<RedisStatusWriter>(_redis, _myAddressStr);
				}
			}
			else {
				throw std::runtime_error("CentralDB: Redis status mode selected but no Redis configuration provided");
			}
			break;
		case STATUS_WRITER_MODE_BIGTABLE:
			fprintf(stderr, "Using BigTable for status writer\n");
			if (cc->bigTableConfig == NULL) {
				throw std::runtime_error(
					"CentralDB: BigTable status mode selected but no BigTable configuration provided");
			}
			if (cc->pubSubConfig == NULL) {
				throw std::runtime_error(
					"CentralDB: BigTable status mode selected but no PubSub configuration provided");
			}

			_statusWriter = std::make_shared<BigTableStatusWriter>(
				cc->bigTableConfig->project_id, cc->bigTableConfig->instance_id, cc->bigTableConfig->table_id);
			break;
		case STATUS_WRITER_MODE_PGSQL:
		default:
			fprintf(stderr, "Using PostgreSQL for status writer\n");
			_statusWriter = std::make_shared<PostgresStatusWriter>(_pool);
			break;
	}

	// start background threads
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i] = std::thread(&CentralDB::commitThread, this);
	}
	_onlineNotificationThread = std::thread(&CentralDB::onlineNotificationThread, this);

	configureSmee();
}

CentralDB::~CentralDB()
{
	if (_smee != NULL) {
		rustybits::smee_client_delete(_smee);
		_smee = NULL;
	}

	rustybits::shutdown_async_runtime();

	_run = 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	_heartbeatThread.join();
	_commitQueue.stop();
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i].join();
	}
	_onlineNotificationThread.join();
}

void CentralDB::configureSmee()
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::configureSmee");
	auto scope = tracer->WithActiveSpan(span);

	const char* TEMPORAL_SCHEME = "ZT_TEMPORAL_SCHEME";
	const char* TEMPORAL_HOST = "ZT_TEMPORAL_HOST";
	const char* TEMPORAL_PORT = "ZT_TEMPORAL_PORT";
	const char* TEMPORAL_NAMESPACE = "ZT_TEMPORAL_NAMESPACE";
	const char* SMEE_TASK_QUEUE = "ZT_SMEE_TASK_QUEUE";

	const char* scheme = getenv(TEMPORAL_SCHEME);
	if (scheme == NULL) {
		scheme = "http";
	}
	const char* host = getenv(TEMPORAL_HOST);
	const char* port = getenv(TEMPORAL_PORT);
	const char* ns = getenv(TEMPORAL_NAMESPACE);
	const char* task_queue = getenv(SMEE_TASK_QUEUE);

	if (scheme != NULL && host != NULL && port != NULL && ns != NULL && task_queue != NULL) {
		fprintf(stderr, "creating smee client\n");
		std::string hostPort =
			std::string(scheme) + std::string("://") + std::string(host) + std::string(":") + std::string(port);
		this->_smee = rustybits::smee_client_new(hostPort.c_str(), ns, task_queue);
	}
	else {
		fprintf(stderr, "Smee client not configured\n");
	}
}

bool CentralDB::waitForReady()
{
	while (_ready < 2) {
		_readyLock.lock();
		_readyLock.unlock();
	}
	return true;
}

bool CentralDB::isReady()
{
	return ((_ready == 2) && (_connected));
}

bool CentralDB::save(nlohmann::json& record, bool notifyListeners)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::save");
	auto scope = tracer->WithActiveSpan(span);

	bool modified = false;
	try {
		if (! record.is_object()) {
			fprintf(stderr, "record is not an object?!?\n");
			return false;
		}
		const std::string objtype = record["objtype"];
		if (objtype == "network") {
			auto span = tracer->StartSpan("CentralDB::save::network");
			auto scope = tracer->WithActiveSpan(span);

			const uint64_t nwid = OSUtils::jsonIntHex(record["id"], 0ULL);
			if (nwid) {
				nlohmann::json old;
				get(nwid, old);
				if ((! old.is_object()) || (! _compareRecords(old, record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"], 0ULL) + 1ULL;
					_queueItem qi;
					qi.jsonData = record;
					qi.notifyListeners = notifyListeners;
					OtelCarrier<std::map<std::string, std::string> > carrier(qi.traceContext);
					auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
					auto propagator =
						opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
					propagator->Inject(carrier, current_ctx);
					_commitQueue.post(qi);
					modified = true;
				}
			}
		}
		else if (objtype == "member") {
			auto span = tracer->StartSpan("CentralDB::save::member");
			auto scope = tracer->WithActiveSpan(span);

			std::string networkId = record["nwid"];
			std::string memberId = record["id"];
			const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"], 0ULL);
			const uint64_t id = OSUtils::jsonIntHex(record["id"], 0ULL);
			if ((id) && (nwid)) {
				nlohmann::json network, old;
				get(nwid, network, id, old);
				if ((! old.is_object()) || (! _compareRecords(old, record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"], 0ULL) + 1ULL;

					_queueItem qi;
					qi.jsonData = record;
					qi.notifyListeners = notifyListeners;
					OtelCarrier<std::map<std::string, std::string> > carrier(qi.traceContext);
					auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
					auto propagator =
						opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
					propagator->Inject(carrier, current_ctx);
					_commitQueue.post(qi);

					modified = true;
				}
				else {
					// fprintf(stderr, "no change\n");
				}
			}
		}
		else {
			fprintf(stderr, "uhh waaat\n");
		}
	}
	catch (std::exception& e) {
		fprintf(stderr, "Error on CentralDB::save: %s\n", e.what());
	}
	catch (...) {
		fprintf(stderr, "Unknown error on CentralDB::save\n");
	}
	return modified;
}

void CentralDB::eraseNetwork(const uint64_t networkId)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::eraseNetwork");
	auto scope = tracer->WithActiveSpan(span);
	char networkIdStr[17];
	span->SetAttribute("network_id", Utils::hex(networkId, networkIdStr));

	char tmp2[24];
	waitForReady();
	Utils::hex(networkId, tmp2);

	_queueItem qi;
	qi.jsonData["id"] = tmp2;
	qi.jsonData["objtype"] = "_delete_network";
	qi.notifyListeners = true;
	OtelCarrier<std::map<std::string, std::string> > carrier(qi.traceContext);
	auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
	auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
	propagator->Inject(carrier, current_ctx);
	_commitQueue.post(qi);

	nlohmann::json nullJson;
	_networkChanged(qi.jsonData, nullJson, true);
}

void CentralDB::eraseMember(const uint64_t networkId, const uint64_t memberId)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::eraseMember");
	auto scope = tracer->WithActiveSpan(span);
	char networkIdStr[17];
	char memberIdStr[11];
	span->SetAttribute("network_id", Utils::hex(networkId, networkIdStr));
	span->SetAttribute("member_id", Utils::hex10(memberId, memberIdStr));

	char tmp2[24];
	waitForReady();

	_queueItem qi;
	qi.jsonData["nwid"] = tmp2;
	qi.jsonData["id"] = tmp2;
	qi.jsonData["objtype"] = "_delete_member";
	qi.notifyListeners = true;
	OtelCarrier<std::map<std::string, std::string> > carrier(qi.traceContext);
	auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
	auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
	propagator->Inject(carrier, current_ctx);
	_commitQueue.post(qi);

	nlohmann::json nullJson;
	_memberChanged(qi.jsonData, nullJson, true);
}

void CentralDB::nodeIsOnline(
	const uint64_t networkId,
	const uint64_t memberId,
	const InetAddress& physicalAddress,
	const char* osArch)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::nodeIsOnline");
	auto scope = tracer->WithActiveSpan(span);
	char networkIdStr[17];
	char memberIdStr[11];
	char ipStr[INET6_ADDRSTRLEN];
	span->SetAttribute("network_id", Utils::hex(networkId, networkIdStr));
	span->SetAttribute("member_id", Utils::hex10(memberId, memberIdStr));
	span->SetAttribute("physical_address", physicalAddress.toString(ipStr));
	span->SetAttribute("os_arch", osArch);

	std::lock_guard<std::mutex> l(_lastOnline_l);
	NodeOnlineRecord& i = _lastOnline[std::pair<uint64_t, uint64_t>(networkId, memberId)];
	i.lastSeen = OSUtils::now();
	if (physicalAddress) {
		i.physicalAddress = physicalAddress;
	}
	i.osArch = std::string(osArch);
}

void CentralDB::nodeIsOnline(const uint64_t networkId, const uint64_t memberId, const InetAddress& physicalAddress)
{
	this->nodeIsOnline(networkId, memberId, physicalAddress, "unknown/unknown");
}

AuthInfo CentralDB::getSSOAuthInfo(const nlohmann::json& member, const std::string& redirectURL)
{
	if (_cc->ssoEnabled) {
		auto provider = opentelemetry::trace::Provider::GetTracerProvider();
		auto tracer = provider->GetTracer("CentralDB");
		auto span = tracer->StartSpan("CentralDB::getSSOAuthInfo");
		auto scope = tracer->WithActiveSpan(span);

		Metrics::db_get_sso_info++;
		// NONCE is just a random character string.  no semantic meaning
		// state = HMAC SHA384 of Nonce based on shared sso key
		//
		// need nonce timeout in database? make sure it's used within X time
		// X is 5 minutes for now.  Make configurable later?
		//
		// how do we tell when a nonce is used? if auth_expiration_time is set
		std::string networkId = member["nwid"];
		std::string memberId = member["id"];

		char authenticationURL[4096] = { 0 };
		AuthInfo info;
		info.enabled = true;

		// if (memberId == "a10dccea52" && networkId == "8056c2e21c24673d") {
		//	fprintf(stderr, "invalid authinfo for grant's machine\n");
		//	info.version=1;
		//	return info;
		// }
		//  fprintf(stderr, "CentralDB::updateMemberOnLoad: %s-%s\n", networkId.c_str(), memberId.c_str());
		std::shared_ptr<PostgresConnection> c;
		try {
			c = _pool->borrow();
			pqxx::work w(*c->c);

			char nonceBytes[16] = { 0 };
			std::string nonce = "";

			// check if the member exists first.
			pqxx::row count =
				w.exec(
					 "SELECT count(id) FROM ztc_member WHERE id = $1 AND network_id = $2 AND deleted = false",
					 pqxx::params { memberId, networkId })
					.one_row();
			if (count[0].as<int>() == 1) {
				// get active nonce, if exists.
				pqxx::result r = w.exec(
					"SELECT nonce FROM ztc_sso_expiry "
					"WHERE network_id = $1 AND member_id = $2 "
					"AND ((NOW() AT TIME ZONE 'UTC') <= authentication_expiry_time) AND ((NOW() AT TIME ZONE 'UTC') <= "
					"nonce_expiration)",
					pqxx::params { networkId, memberId });

				if (r.size() == 0) {
					// no active nonce.
					// find an unused nonce, if one exists.
					pqxx::result r = w.exec(
						"SELECT nonce FROM ztc_sso_expiry "
						"WHERE network_id = $1 AND member_id = $2 "
						"AND authentication_expiry_time IS NULL AND ((NOW() AT TIME ZONE 'UTC') <= nonce_expiration)",
						pqxx::params { networkId, memberId });

					if (r.size() == 1) {
						// we have an existing nonce.  Use it
						nonce = r.at(0)[0].as<std::string>();
						Utils::unhex(nonce.c_str(), nonceBytes, sizeof(nonceBytes));
					}
					else if (r.empty()) {
						// create a nonce
						Utils::getSecureRandom(nonceBytes, 16);
						char nonceBuf[64] = { 0 };
						Utils::hex(nonceBytes, sizeof(nonceBytes), nonceBuf);
						nonce = std::string(nonceBuf);

						pqxx::result ir = w.exec(
							"INSERT INTO ztc_sso_expiry "
							"(nonce, nonce_expiration, network_id, member_id) VALUES "
							"($1, TO_TIMESTAMP($2::double precision/1000), $3, $4)",
							pqxx::params { nonce, OSUtils::now() + 300000, networkId, memberId });

						w.commit();
					}
					else {
						// > 1 ?!?  Thats an error!
						fprintf(stderr, "> 1 unused nonce!\n");
						exit(6);
					}
				}
				else if (r.size() == 1) {
					nonce = r.at(0)[0].as<std::string>();
					Utils::unhex(nonce.c_str(), nonceBytes, sizeof(nonceBytes));
				}
				else {
					// more than 1 nonce in use?  Uhhh...
					fprintf(stderr, "> 1 nonce in use for network member?!?\n");
					exit(7);
				}

				r = w.exec(
					"SELECT oc.client_id, oc.authorization_endpoint, oc.issuer, oc.provider, oc.sso_impl_version "
					"FROM ztc_network AS n "
					"INNER JOIN ztc_org o "
					"  ON o.owner_id = n.owner_id "
					"LEFT OUTER JOIN ztc_network_oidc_config noc "
					"  ON noc.network_id = n.id "
					"LEFT OUTER JOIN ztc_oidc_config oc "
					"  ON noc.client_id = oc.client_id AND oc.org_id = o.org_id "
					"WHERE n.id = $1 AND n.sso_enabled = true",
					pqxx::params { networkId });

				std::string client_id = "";
				std::string authorization_endpoint = "";
				std::string issuer = "";
				std::string provider = "";
				uint64_t sso_version = 0;

				if (r.size() == 1) {
					client_id = r.at(0)[0].as<std::optional<std::string> >().value_or("");
					authorization_endpoint = r.at(0)[1].as<std::optional<std::string> >().value_or("");
					issuer = r.at(0)[2].as<std::optional<std::string> >().value_or("");
					provider = r.at(0)[3].as<std::optional<std::string> >().value_or("");
					sso_version = r.at(0)[4].as<std::optional<uint64_t> >().value_or(1);
				}
				else if (r.size() > 1) {
					fprintf(
						stderr, "ERROR: More than one auth endpoint for an organization?!?!? NetworkID: %s\n",
						networkId.c_str());
				}
				else {
					fprintf(stderr, "No client or auth endpoint?!?\n");
				}

				info.version = sso_version;

				// no catch all else because we don't actually care if no records exist here. just continue as normal.
				if ((! client_id.empty()) && (! authorization_endpoint.empty())) {
					uint8_t state[48];
					HMACSHA384(_ssoPsk, nonceBytes, sizeof(nonceBytes), state);
					char state_hex[256];
					Utils::hex(state, 48, state_hex);

					if (info.version == 0) {
						char url[2048] = { 0 };
						OSUtils::ztsnprintf(
							url, sizeof(authenticationURL),
							"%s?response_type=id_token&response_mode=form_post&scope=openid+email+profile&redirect_uri="
							"%s&nonce=%s&state=%s&client_id=%s",
							authorization_endpoint.c_str(), url_encode(redirectURL).c_str(), nonce.c_str(), state_hex,
							client_id.c_str());
						info.authenticationURL = std::string(url);
					}
					else if (info.version == 1) {
						info.ssoClientID = client_id;
						info.issuerURL = issuer;
						info.ssoProvider = provider;
						info.ssoNonce = nonce;
						info.ssoState = std::string(state_hex) + "_" + networkId;
						info.centralAuthURL = redirectURL;
#ifdef ZT_DEBUG
						fprintf(
							stderr,
							"ssoClientID: %s\nissuerURL: %s\nssoNonce: %s\nssoState: %s\ncentralAuthURL: %s\nprovider: "
							"%s\n",
							info.ssoClientID.c_str(), info.issuerURL.c_str(), info.ssoNonce.c_str(),
							info.ssoState.c_str(), info.centralAuthURL.c_str(), provider.c_str());
#endif
					}
				}
				else {
					fprintf(
						stderr, "client_id: %s\nauthorization_endpoint: %s\n", client_id.c_str(),
						authorization_endpoint.c_str());
				}
			}

			_pool->unborrow(c);
		}
		catch (std::exception& e) {
			span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
			fprintf(stderr, "ERROR: Error updating member on load for network %s: %s\n", networkId.c_str(), e.what());
		}

		return info;   // std::string(authenticationURL);
	}
	return AuthInfo();
}

void CentralDB::initializeNetworks()
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::initializeNetworks");
	auto scope = tracer->WithActiveSpan(span);

	fprintf(stderr, "Initializing networks...\n");

	try {
		char qbuf[2048];
		sprintf(
			qbuf,
			"SELECT id, name, configuration , (EXTRACT(EPOCH FROM creation_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"(EXTRACT(EPOCH FROM last_modified AT TIME ZONE 'UTC')*1000)::bigint, revision, frontend "
			"FROM networks_ctl WHERE controller_id = '%s'",
			_myAddressStr.c_str());

		auto c = _pool->borrow();
		pqxx::work w(*c->c);

		fprintf(stderr, "Load networks from psql...\n");
		auto stream = pqxx::stream_from::query(w, qbuf);
		std::tuple<
			std::string	  // network ID
			,
			std::optional<std::string>	 // name
			,
			std::string	  // configuration
			,
			std::optional<uint64_t>	  // creation_time
			,
			std::optional<uint64_t>	  // last_modified
			,
			std::optional<uint64_t>	  // revision
			,
			std::string	  // frontend
			>
			row;
		uint64_t count = 0;
		uint64_t total = 0;
		while (stream >> row) {
			auto start = std::chrono::high_resolution_clock::now();

			json empty;
			json config;

			initNetwork(config);

			std::string nwid = std::get<0>(row);
			std::string name = std::get<1>(row).value_or("");
			json cfgtmp = json::parse(std::get<2>(row));
			std::optional<uint64_t> created_at = std::get<3>(row);
			std::optional<uint64_t> last_modified = std::get<4>(row);
			std::optional<uint64_t> revision = std::get<5>(row);
			std::string frontend = std::get<6>(row);

			config["id"] = nwid;
			config["name"] = name;
			config["creationTime"] = created_at.value_or(0);
			config["lastModified"] = last_modified.value_or(0);
			config["revision"] = revision.value_or(0);
			config["capabilities"] = cfgtmp["capabilities"].is_array() ? cfgtmp["capabilities"] : json::array();
			config["enableBroadcast"] =
				cfgtmp["enableBroadcast"].is_boolean() ? cfgtmp["enableBroadcast"].get<bool>() : false;
			config["mtu"] = cfgtmp["mtu"].is_number() ? cfgtmp["mtu"].get<int32_t>() : 2800;
			config["multicastLimit"] =
				cfgtmp["multicastLimit"].is_number() ? cfgtmp["multicastLimit"].get<int32_t>() : 64;
			config["private"] = cfgtmp["private"].is_boolean() ? cfgtmp["private"].get<bool>() : true;
			config["remoteTraceLevel"] =
				cfgtmp["remoteTraceLevel"].is_number() ? cfgtmp["remoteTraceLevel"].get<int32_t>() : 0;
			config["remoteTraceTarget"] =
				cfgtmp["remoteTraceTarget"].is_string() ? cfgtmp["remoteTraceTarget"].get<std::string>() : "";
			config["revision"] = revision.value_or(0);
			config["rules"] = cfgtmp["rules"].is_array() ? cfgtmp["rules"] : json::array();
			config["tags"] = cfgtmp["tags"].is_array() ? cfgtmp["tags"] : json::array();
			if (cfgtmp["v4AssignMode"].is_object()) {
				config["v4AssignMode"] = cfgtmp["v4AssignMode"];
			}
			else {
				config["v4AssignMode"] = json::object();
				config["v4AssignMode"]["zt"] = true;
			}
			if (cfgtmp["v6AssignMode"].is_object()) {
				config["v6AssignMode"] = cfgtmp["v6AssignMode"];
			}
			else {
				config["v6AssignMode"] = json::object();
				config["v6AssignMode"]["zt"] = true;
				config["v6AssignMode"]["6plane"] = true;
				config["v6AssignMode"]["rfc4193"] = false;
			}
			config["ssoEnabled"] = cfgtmp["ssoEnabled"].is_boolean() ? cfgtmp["ssoEnabled"].get<bool>() : false;
			if (config["ssoConfig"].is_object()) {
				config["ssoConfig"] = cfgtmp["ssoConfig"];
			}
			else {
				config["ssoConfig"] = empty;
			}
			config["objtype"] = "network";
			config["routes"] = cfgtmp["routes"].is_array() ? cfgtmp["routes"] : json::array();
			config["clientId"] = cfgtmp["clientId"].is_string() ? cfgtmp["clientId"].get<std::string>() : "";
			config["authorizationEndpoint"] =
				cfgtmp["authorizationEndpoint"].is_string() ? cfgtmp["authorizationEndpoint"].get<std::string>() : "";
			config["provider"] = cfgtmp["ssoProvider"].is_string() ? cfgtmp["ssoProvider"].get<std::string>() : "";
			if (! cfgtmp["dns"].is_object()) {
				cfgtmp["dns"] = json::object();
				cfgtmp["dns"]["domain"] = "";
				cfgtmp["dns"]["servers"] = json::array();
			}
			else {
				config["dns"] = cfgtmp["dns"];
			}
			config["ipAssignmentPools"] =
				cfgtmp["ipAssignmentPools"].is_array() ? cfgtmp["ipAssignmentPools"] : json::array();
			config["frontend"] = frontend;

			Metrics::network_count++;

			_networkChanged(empty, config, false);

			auto end = std::chrono::high_resolution_clock::now();
			auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

			total += dur.count();
			++count;
			if (count > 0 && count % 10000 == 0) {
				fprintf(stderr, "Averaging %lu us per network\n", (total / count));
			}
		}

		w.commit();
		_pool->unborrow(c);
		fprintf(stderr, "done.\n");

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(
					stderr, "[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S, _timestr(),
					(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
		fprintf(stderr, "network init done\n");
	}
	catch (std::exception& e) {
		fprintf(stderr, "ERROR: Error initializing networks: %s\n", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		std::this_thread::sleep_for(std::chrono::milliseconds(5000));
		exit(-1);
	}
}

void CentralDB::initializeMembers()
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::initializeMembers");
	auto scope = tracer->WithActiveSpan(span);

	std::string memberId;
	std::string networkId;
	try {
		std::unordered_map<std::string, std::string> networkMembers;
		fprintf(stderr, "Initializing Members...\n");

		std::string setKeyBase = "network-nodes-all:{" + _myAddressStr + "}:";

		if (_redisMemberStatus) {
			fprintf(stderr, "Initialize Redis for members...\n");
			std::unique_lock<std::shared_mutex> l(_networks_l);
			std::unordered_set<std::string> deletes;
			for (auto it : _networks) {
				uint64_t nwid_i = it.first;
				char nwidTmp[64] = { 0 };
				OSUtils::ztsnprintf(nwidTmp, sizeof(nwidTmp), "%.16llx", nwid_i);
				std::string nwid(nwidTmp);
				std::string key = setKeyBase + nwid;
				deletes.insert(key);
			}

			if (! deletes.empty()) {
				try {
					if (_cc->redisConfig->clusterMode) {
						auto tx = _cluster->transaction(_myAddressStr, true, false);
						for (std::string k : deletes) {
							tx.del(k);
						}
						tx.exec();
					}
					else {
						auto tx = _redis->transaction(true, false);
						for (std::string k : deletes) {
							tx.del(k);
						}
						tx.exec();
					}
				}
				catch (sw::redis::Error& e) {
					// ignore
				}
			}
		}

		char qbuf[2048];
		sprintf(
			qbuf,
			"SELECT nm.device_id, nm.network_id, nm.authorized, nm.active_bridge, nm.ip_assignments, "
			"nm.no_auto_assign_ips, "
			"nm.sso_exempt, (EXTRACT(EPOCH FROM nm.authentication_expiry_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"(EXTRACT(EPOCH FROM nm.creation_time AT TIME ZONE 'UTC')*1000)::bigint, nm.identity, "
			"(EXTRACT(EPOCH FROM nm.last_authorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"(EXTRACT(EPOCH FROM nm.last_deauthorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"nm.remote_trace_level, nm.remote_trace_target, nm.revision, nm.capabilities, nm.tags, "
			"nm.frontend "
			"FROM network_memberships_ctl nm "
			"INNER JOIN networks_ctl n "
			"  ON nm.network_id = n.id "
			"WHERE n.controller_id = '%s'",
			_myAddressStr.c_str());

		auto c = _pool->borrow();
		pqxx::work w(*c->c);
		fprintf(stderr, "Load members from psql...\n");
		auto stream = pqxx::stream_from::query(w, qbuf);
		std::tuple<
			std::string	  // device ID
			,
			std::string	  // network ID
			,
			bool   // authorized
			,
			std::optional<bool>	  // active_bridge
			,
			std::optional<std::string>	 // ip_assignments
			,
			std::optional<bool>	  // no_auto_assign_ips
			,
			std::optional<bool>	  // sso_exempt
			,
			std::optional<uint64_t>	  // authentication_expiry_time
			,
			std::optional<uint64_t>	  // creation_time
			,
			std::optional<std::string>	 // identity
			,
			std::optional<uint64_t>	  // last_authorized_time
			,
			std::optional<uint64_t>	  // last_deauthorized_time
			,
			std::optional<int32_t>	 // remote_trace_level
			,
			std::optional<std::string>	 // remote_trace_target
			,
			std::optional<uint64_t>	  // revision
			,
			std::optional<std::string>	 // capabilities
			,
			std::optional<std::string>	 // tags
			,
			std::string	  // frontend
			>
			row;

		auto tmp = std::chrono::high_resolution_clock::now();
		uint64_t count = 0;
		uint64_t total = 0;
		while (stream >> row) {
			auto start = std::chrono::high_resolution_clock::now();
			json empty;
			json config;

			initMember(config);

			memberId = std::get<0>(row);
			networkId = std::get<1>(row);
			bool authorized = std::get<2>(row);
			std::optional<bool> active_bridge = std::get<3>(row);
			std::string ip_assignments = std::get<4>(row).value_or("");
			std::optional<bool> no_auto_assign_ips = std::get<5>(row);
			std::optional<bool> sso_exempt = std::get<6>(row);
			std::optional<uint64_t> authentication_expiry_time = std::get<7>(row);
			std::optional<uint64_t> creation_time = std::get<8>(row);
			std::optional<std::string> identity = std::get<9>(row);
			std::optional<uint64_t> last_authorized_time = std::get<10>(row);
			std::optional<uint64_t> last_deauthorized_time = std::get<11>(row);
			std::optional<int32_t> remote_trace_level = std::get<12>(row);
			std::optional<std::string> remote_trace_target = std::get<13>(row);
			std::optional<uint64_t> revision = std::get<14>(row);
			std::optional<std::string> capabilities = std::get<15>(row);
			std::optional<std::string> tags = std::get<16>(row);

			networkMembers.insert(std::pair<std::string, std::string>(setKeyBase + networkId, memberId));

			config["objtype"] = "member";
			config["id"] = memberId;
			config["address"] = identity.value_or("");
			config["nwid"] = networkId;
			config["authorized"] = authorized;
			config["activeBridge"] = active_bridge.value_or(false);
			config["ipAssignments"] = json::array();
			if (ip_assignments != "{}") {
				std::string tmp = ip_assignments.substr(1, ip_assignments.length() - 2);
				std::vector<std::string> addrs = split(tmp, ',');
				for (auto it = addrs.begin(); it != addrs.end(); ++it) {
					config["ipAssignments"].push_back(*it);
				}
			}
			config["capabilities"] = json::parse(capabilities.value_or("[]"));
			config["creationTime"] = creation_time.value_or(0);
			config["lastAuthorizedTime"] = last_authorized_time.value_or(0);
			config["lastDeauthorizedTime"] = last_deauthorized_time.value_or(0);
			config["noAutoAssignIPs"] = no_auto_assign_ips.value_or(false);
			config["remoteTraceLevel"] = remote_trace_level.value_or(0);
			config["remoteTraceTarget"] = remote_trace_target.value_or(nullptr);
			config["revision"] = revision.value_or(0);
			config["ssoExempt"] = sso_exempt.value_or(false);
			config["authenticationExpiryTime"] = authentication_expiry_time.value_or(0);
			config["tags"] = json::parse(tags.value_or("[]"));
			config["frontend"] = std::get<17>(row);

			Metrics::member_count++;

			_memberChanged(empty, config, false);

			memberId = "";
			networkId = "";

			auto end = std::chrono::high_resolution_clock::now();
			auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
			total += dur.count();
			++count;
			if (count > 0 && count % 10000 == 0) {
				fprintf(stderr, "Averaging %llu us per member\n", (total / count));
			}
		}
		if (count > 0) {
			fprintf(stderr, "Took %llu us per member to load\n", (total / count));
		}

		stream.complete();

		w.commit();
		_pool->unborrow(c);
		fprintf(stderr, "done.\n");

		if (_listenerMode == LISTENER_MODE_REDIS)
			if (! networkMembers.empty()) {
				if (_redisMemberStatus) {
					fprintf(stderr, "Load member data into redis...\n");
					if (_cc->redisConfig->clusterMode) {
						auto tx = _cluster->transaction(_myAddressStr, true, false);
						uint64_t count = 0;
						for (auto it : networkMembers) {
							tx.sadd(it.first, it.second);
							if (++count % 30000 == 0) {
								tx.exec();
								tx = _cluster->transaction(_myAddressStr, true, false);
							}
						}
						tx.exec();
					}
					else {
						auto tx = _redis->transaction(true, false);
						uint64_t count = 0;
						for (auto it : networkMembers) {
							tx.sadd(it.first, it.second);
							if (++count % 30000 == 0) {
								tx.exec();
								tx = _redis->transaction(true, false);
							}
						}
						tx.exec();
					}
					fprintf(stderr, "done.\n");
				}
			}

		fprintf(stderr, "Done loading members...\n");

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(
					stderr, "[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S, _timestr(),
					(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	}
	catch (sw::redis::Error& e) {
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		fprintf(stderr, "ERROR: Error initializing members (redis): %s\n", e.what());
		exit(-1);
	}
	catch (std::exception& e) {
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		fprintf(stderr, "ERROR: Error initializing member: %s-%s %s\n", networkId.c_str(), memberId.c_str(), e.what());
		exit(-1);
	}
}

void CentralDB::heartbeat()
{
	char publicId[1024];
	char hostnameTmp[1024];
	_myId.toString(false, publicId);
	if (gethostname(hostnameTmp, sizeof(hostnameTmp)) != 0) {
		hostnameTmp[0] = (char)0;
	}
	else {
		for (int i = 0; i < (int)sizeof(hostnameTmp); ++i) {
			if ((hostnameTmp[i] == '.') || (hostnameTmp[i] == 0)) {
				hostnameTmp[i] = (char)0;
				break;
			}
		}
	}
	const char* controllerId = _myAddressStr.c_str();
	const char* publicIdentity = publicId;
	const char* hostname = hostnameTmp;

	while (_run == 1) {
		auto provider = opentelemetry::trace::Provider::GetTracerProvider();
		auto tracer = provider->GetTracer("CentralDB");
		auto span = tracer->StartSpan("CentralDB::heartbeat");
		auto scope = tracer->WithActiveSpan(span);

		// fprintf(stderr, "%s: heartbeat\n", controllerId);
		auto c = _pool->borrow();
		int64_t ts = OSUtils::now();

		if (c->c) {
			std::string major = std::to_string(ZEROTIER_ONE_VERSION_MAJOR);
			std::string minor = std::to_string(ZEROTIER_ONE_VERSION_MINOR);
			std::string rev = std::to_string(ZEROTIER_ONE_VERSION_REVISION);
			std::string version = major + "." + minor + "." + rev;
			std::string versionStr = "v" + version;

			try {
				pqxx::work w { *c->c };
				w.exec(
					 "INSERT INTO controllers_ctl (id, hostname, last_heartbeat, public_identity, version) VALUES "
					 "($1, $2, TO_TIMESTAMP($3::double precision/1000), $4, $5) "
					 "ON CONFLICT (id) DO UPDATE SET hostname = EXCLUDED.hostname, last_heartbeat = "
					 "EXCLUDED.last_heartbeat, "
					 "public_identity = EXCLUDED.public_identity, version = EXCLUDED.version",
					 pqxx::params { controllerId, hostname, ts, publicIdentity, versionStr })
					.no_rows();
				w.commit();
			}
			catch (std::exception& e) {
				fprintf(stderr, "%s: Heartbeat update failed: %s\n", controllerId, e.what());
				span->End();
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				continue;
			}
		}
		_pool->unborrow(c);

		try {
			if (_listenerMode == LISTENER_MODE_REDIS && _redisMemberStatus) {
				if (_cc->redisConfig->clusterMode) {
					_cluster->zadd("controllers", "controllerId", ts);
				}
				else {
					_redis->zadd("controllers", "controllerId", ts);
				}
			}
		}
		catch (sw::redis::Error& e) {
			fprintf(stderr, "ERROR: Redis error in heartbeat thread: %s\n", e.what());
		}

		span->End();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
	fprintf(stderr, "Exited heartbeat thread\n");
}

void CentralDB::commitThread()
{
	fprintf(stderr, "%s: commitThread start\n", _myAddressStr.c_str());
	_queueItem qitem;
	while (_commitQueue.get(qitem) & (_run == 1)) {
		auto provider = opentelemetry::trace::Provider::GetTracerProvider();
		auto tracer = provider->GetTracer("CentralDB");

		auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
		OtelCarrier<std::map<std::string, std::string> > carrier(qitem.traceContext);
		auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
		auto new_context = propagator->Extract(carrier, current_ctx);
		auto remote_span = opentelemetry::trace::GetSpan(new_context);
		auto remote_scope = tracer->WithActiveSpan(remote_span);

		{
			auto span = tracer->StartSpan("CentralDB::commitThread");
			auto scope = tracer->WithActiveSpan(span);

			fprintf(stderr, "commitThread tick\n");
			if (! qitem.jsonData.is_object()) {
				fprintf(stderr, "not an object\n");
				continue;
			}

			std::shared_ptr<PostgresConnection> c;
			try {
				c = _pool->borrow();
			}
			catch (std::exception& e) {
				fprintf(stderr, "ERROR: %s\n", e.what());
				continue;
			}

			if (! c) {
				fprintf(stderr, "Error getting database connection\n");
				continue;
			}

			Metrics::pgsql_commit_ticks++;
			try {
				nlohmann::json& config = (qitem.jsonData);
				const std::string objtype = config["objtype"];
				if (objtype == "member") {
					auto mspan = tracer->StartSpan("CentralDB::commitThread::member");
					auto mscope = tracer->WithActiveSpan(mspan);

					// fprintf(stderr, "%s: commitThread: member\n", _myAddressStr.c_str());
					std::string memberId;
					std::string networkId;

					try {
						pqxx::work w(*c->c);

						memberId = config["id"];
						networkId = config["nwid"];

						std::string target = "NULL";
						if (! config["remoteTraceTarget"].is_null()) {
							target = config["remoteTraceTarget"];
						}

						// get network and the frontend it is assigned to
						// if network does not exist, skip member update
						pqxx::row nwrow =
							w.exec(
								 "SELECT COUNT(id), frontend FROM networks_ctl WHERE id = $1 GROUP BY frontend",
								 pqxx::params { networkId })
								.one_row();
						int nwcount = nwrow[0].as<int>();
						std::string frontend = nwrow[1].as<std::string>();

						if (nwcount != 1) {
							fprintf(stderr, "network %s does not exist.  skipping member upsert\n", networkId.c_str());
							w.abort();
							_pool->unborrow(c);
							continue;
						}

						pqxx::row mrow =
							w.exec(
								 "SELECT COUNT(device_id) FROM network_memberships_ctl WHERE device_id = $1 "
								 "AND network_id = $2",
								 pqxx::params { memberId, networkId })
								.one_row();
						int membercount = mrow[0].as<int>();
						bool isNewMember = (membercount == 0);

						std::string change_source;
						if (! config["change_source"].is_null()) {
							change_source = config["change_source"];
						}
						else {
							change_source = "controller";
						}
						if (! isNewMember && change_source != "controller" && frontend != change_source) {
							// if it is not a new member and the change source is not the controller and doesn't match
							// the frontend, don't apply the change.
							continue;
						}

						std::vector<std::string> ipAssignments;
						if (config["ipAssignments"].is_array()) {
							for (auto& ip : config["ipAssignments"]) {
								if (ip.is_string()) {
									ipAssignments.push_back(ip.get<std::string>());
								}
							}
						}

						int64_t vMajor = OSUtils::jsonUInt(config["vMajor"], 0);
						int64_t vMinor = OSUtils::jsonUInt(config["vMinor"], 0);
						int64_t vRev = OSUtils::jsonUInt(config["vRev"], 0);
						int64_t vProto = OSUtils::jsonUInt(config["vProto"], 0);
						if (vMajor < 0)
							vMajor = 0;
						if (vMinor < 0)
							vMinor = 0;
						if (vRev < 0)
							vRev = 0;
						if (vProto < 0)
							vProto = 0;

						pqxx::result res =
							w.exec(
								 "INSERT INTO network_memberships_ctl (device_id, network_id, authorized, "
								 "active_bridge, "
								 "ip_assignments, "
								 "no_auto_assign_ips, sso_exempt, authentication_expiry_time, capabilities, "
								 "creation_time, "
								 "identity, last_authorized_time, last_deauthorized_time, "
								 "remote_trace_level, remote_trace_target, revision, tags, version_major, "
								 "version_minor, "
								 "version_revision, version_protocol) "
								 "VALUES ($1, $2, $3, $4, $5, $6, $7, TO_TIMESTAMP($8::double precision/1000), $9, "
								 "TO_TIMESTAMP($10::double precision/1000), $11, TO_TIMESTAMP($12::double "
								 "precision/1000), "
								 "TO_TIMESTAMP($13::double precision/1000), $14, $15, $16, $17, $18, $19, $20, $21) "
								 "ON CONFLICT (device_id, network_id) DO UPDATE SET "
								 "authorized = EXCLUDED.authorized, active_bridge = EXCLUDED.active_bridge, "
								 "ip_assignments = EXCLUDED.ip_assignments, no_auto_assign_ips = "
								 "EXCLUDED.no_auto_assign_ips, "
								 "sso_exempt = EXCLUDED.sso_exempt, authentication_expiry_time = "
								 "EXCLUDED.authentication_expiry_time, "
								 "capabilities = EXCLUDED.capabilities, creation_time = EXCLUDED.creation_time, "
								 "identity = EXCLUDED.identity, last_authorized_time = EXCLUDED.last_authorized_time, "
								 "last_deauthorized_time = EXCLUDED.last_deauthorized_time, "
								 "remote_trace_level = EXCLUDED.remote_trace_level, remote_trace_target = "
								 "EXCLUDED.remote_trace_target, "
								 "revision = EXCLUDED.revision, tags = EXCLUDED.tags, version_major = "
								 "EXCLUDED.version_major, "
								 "version_minor = EXCLUDED.version_minor, version_revision = "
								 "EXCLUDED.version_revision, "
								 "version_protocol = EXCLUDED.version_protocol",
								 pqxx::params { memberId,
												networkId,
												OSUtils::jsonBool(config["authorized"], false),
												OSUtils::jsonBool(config["activeBridge"], false),
												ipAssignments,
												OSUtils::jsonBool(config["noAutoAssignIps"], false),
												OSUtils::jsonBool(config["ssoExempt"], false),
												OSUtils::jsonInt(config["authenticationExpiryTime"], 0),
												OSUtils::jsonDump(config["capabilities"], -1),
												OSUtils::jsonInt(config["creationTime"], OSUtils::now()),
												OSUtils::jsonString(config["identity"], ""),
												OSUtils::jsonInt(config["lastAuthorizedTime"], 0),
												OSUtils::jsonInt(config["lastDeauthorizedTime"], 0),
												OSUtils::jsonInt(config["remoteTraceLevel"], 0),
												target,
												OSUtils::jsonInt(config["revision"], 0),
												OSUtils::jsonDump(config["tags"], -1),
												vMajor,
												vMinor,
												vRev,
												vProto })
								.no_rows();

						w.commit();

						if (_listenerMode == LISTENER_MODE_PUBSUB) {
							// Publish change to pubsub stream

							if (config["change_source"].is_null() || config["change_source"] == "controller") {
								nlohmann::json oldMember;
								nlohmann::json newMember = config;
								if (! isNewMember) {
									oldMember = _getNetworkMember(w, networkId, memberId);
								}
								_changeNotifier->notifyMemberChange(oldMember, newMember, frontend);
							}
						}

						if (_smee != NULL && isNewMember) {
							// TODO: Smee Notifications for New Members
							// pqxx::row row = w.exec_params1(
							// 	"SELECT "
							// 	"	count(h.hook_id) "
							// 	"FROM "
							// 	"	ztc_hook h "
							// 	"	INNER JOIN ztc_org o ON o.org_id = h.org_id "
							// 	"   INNER JOIN ztc_network n ON n.owner_id = o.owner_id "
							// 	" WHERE "
							// 	"n.id = $1 ",
							// 	networkId);
							// int64_t hookCount = row[0].as<int64_t>();
							// if (hookCount > 0) {
							// 	notifyNewMember(networkId, memberId);
							// }
						}

						const uint64_t nwidInt = OSUtils::jsonIntHex(config["nwid"], 0ULL);
						const uint64_t memberidInt = OSUtils::jsonIntHex(config["id"], 0ULL);
						if (nwidInt && memberidInt) {
							nlohmann::json nwOrig;
							nlohmann::json memOrig;

							nlohmann::json memNew(config);

							get(nwidInt, nwOrig, memberidInt, memOrig);

							_memberChanged(memOrig, memNew, qitem.notifyListeners);
						}
						else {
							fprintf(
								stderr, "%s: Can't notify of change.  Error parsing nwid or memberid: %llu-%llu\n",
								_myAddressStr.c_str(), (unsigned long long)nwidInt, (unsigned long long)memberidInt);
						}
					}
					catch (std::exception& e) {
						fprintf(
							stderr, "%s ERROR: Error updating member %s-%s: %s\n", _myAddressStr.c_str(),
							networkId.c_str(), memberId.c_str(), e.what());
						mspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
					}
				}
				else if (objtype == "network") {
					auto nspan = tracer->StartSpan("CentralDB::commitThread::network");
					auto nscope = tracer->WithActiveSpan(nspan);

					try {
						// fprintf(stderr, "%s: commitThread: network\n", _myAddressStr.c_str());
						pqxx::work w(*c->c);

						std::string id = config["id"];

						pqxx::row nwrow =
							w.exec("SELECT COUNT(id) frontend FROM networks_ctl WHERE id = $1", pqxx::params { id })
								.one_row();
						int nwcount = nwrow[0].as<int>();
						bool isNewNetwork = (nwcount == 0);
						std::string frontend = "";

						if (! isNewNetwork) {
							pqxx::row nwrow =
								w.exec("SELECT frontend FROM networks_ctl WHERE id = $1", pqxx::params { id })
									.one_row();
							frontend = nwrow[0].as<std::string>();
						}

						std::string change_source;
						if (! config["change_source"].is_null()) {
							change_source = config["change_source"];
						}

						if (! isNewNetwork && change_source != "controller" && frontend != change_source) {
							// if it is not a new network and the change source is not the controller and doesn't match
							// the frontend, don't apply the change.
							continue;
						}

						pqxx::result res = w.exec(
							"INSERT INTO networks_ctl (id, name, configuration, controller_id, revision, frontend) "
							"VALUES ($1, $2, $3, $4, $5, $6) "
							"ON CONFLICT (id) DO UPDATE SET "
							"name = EXCLUDED.name, configuration = EXCLUDED.configuration, revision = "
							"EXCLUDED.revision+1, "
							"frontend = EXCLUDED.frontend",
							pqxx::params { id, OSUtils::jsonString(config["name"], ""), OSUtils::jsonDump(config, -1),
										   _myAddressStr, ((uint64_t)config["revision"]), change_source });

						w.commit();

						if (_listenerMode == LISTENER_MODE_PUBSUB) {
							// Publish change to pubsub stream
							if (config["change_source"].is_null() || config["change_source"] == "controller") {
								nlohmann::json oldNetwork;
								nlohmann::json newNetwork = config;
								if (! isNewNetwork) {
									oldNetwork = _getNetwork(w, id);
								}
								_changeNotifier->notifyNetworkChange(oldNetwork, newNetwork, frontend);
							}
						}

						const uint64_t nwidInt = OSUtils::jsonIntHex(config["id"], 0ULL);
						if (nwidInt) {
							nlohmann::json nwOrig;
							nlohmann::json nwNew(config);

							get(nwidInt, nwOrig);

							_networkChanged(nwOrig, nwNew, qitem.notifyListeners);
						}
						else {
							fprintf(
								stderr, "%s: Can't notify network changed: %llu\n", _myAddressStr.c_str(),
								(unsigned long long)nwidInt);
						}
					}
					catch (std::exception& e) {
						nspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
						fprintf(stderr, "%s ERROR: Error updating network: %s\n", _myAddressStr.c_str(), e.what());
					}
					if (_listenerMode == LISTENER_MODE_REDIS && _redisMemberStatus) {
						try {
							std::string id = config["id"];
							std::string controllerId = _myAddressStr.c_str();
							std::string key = "networks:{" + controllerId + "}";
							if (_cc->redisConfig->clusterMode) {
								_cluster->sadd(key, id);
							}
							else {
								_redis->sadd(key, id);
							}
						}
						catch (sw::redis::Error& e) {
							nspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
							fprintf(stderr, "ERROR: Error adding network to Redis: %s\n", e.what());
						}
					}
				}
				else if (objtype == "_delete_network") {
					auto dspan = tracer->StartSpan("CentralDB::commitThread::_delete_network");
					auto dscope = tracer->WithActiveSpan(dspan);

					// fprintf(stderr, "%s: commitThread: delete network\n", _myAddressStr.c_str());
					try {
						pqxx::work w(*c->c);
						std::string networkId = config["id"];
						w.exec("DELETE FROM network_memberships_ctl WHERE network_id = $1", pqxx::params { networkId });
						w.exec("DELETE FROM networks_ctl WHERE id = $1", pqxx::params { networkId });

						w.commit();

						uint64_t nwidInt = OSUtils::jsonIntHex(config["nwid"], 0ULL);
						json oldConfig;
						get(nwidInt, oldConfig);
						json empty;
						_networkChanged(oldConfig, empty, qitem.notifyListeners);
					}
					catch (std::exception& e) {
						dspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
						fprintf(stderr, "%s ERROR: Error deleting network: %s\n", _myAddressStr.c_str(), e.what());
					}
					if (_listenerMode == LISTENER_MODE_REDIS && _redisMemberStatus) {
						try {
							std::string id = config["id"];
							std::string controllerId = _myAddressStr.c_str();
							std::string key = "networks:{" + controllerId + "}";
							if (_cc->redisConfig->clusterMode) {
								_cluster->srem(key, id);
								_cluster->del("network-nodes-online:{" + controllerId + "}:" + id);
							}
							else {
								_redis->srem(key, id);
								_redis->del("network-nodes-online:{" + controllerId + "}:" + id);
							}
						}
						catch (sw::redis::Error& e) {
							dspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
							fprintf(stderr, "ERROR: Error adding network to Redis: %s\n", e.what());
						}
					}
				}
				else if (objtype == "_delete_member") {
					auto mspan = tracer->StartSpan("CentralDB::commitThread::_delete_member");
					auto mscope = tracer->WithActiveSpan(mspan);

					// fprintf(stderr, "%s commitThread: delete member\n", _myAddressStr.c_str());
					try {
						pqxx::work w(*c->c);

						std::string memberId = config["id"];
						std::string networkId = config["nwid"];

						pqxx::result res =
							w.exec(
								 "DELETE FROM network_memberships_ctl WHERE device_id = $1 AND network_id = $2",
								 pqxx::params { memberId, networkId })
								.no_rows();

						w.commit();

						uint64_t nwidInt = OSUtils::jsonIntHex(config["nwid"], 0ULL);
						uint64_t memberidInt = OSUtils::jsonIntHex(config["id"], 0ULL);

						nlohmann::json networkConfig;
						nlohmann::json oldConfig;

						get(nwidInt, networkConfig, memberidInt, oldConfig);
						json empty;
						_memberChanged(oldConfig, empty, qitem.notifyListeners);
					}
					catch (std::exception& e) {
						mspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
						fprintf(stderr, "%s ERROR: Error deleting member: %s\n", _myAddressStr.c_str(), e.what());
					}
					if (_listenerMode == LISTENER_MODE_REDIS && _redisMemberStatus) {
						try {
							std::string memberId = config["id"];
							std::string networkId = config["nwid"];
							std::string controllerId = _myAddressStr.c_str();
							std::string key = "network-nodes-all:{" + controllerId + "}:" + networkId;
							if (_cc->redisConfig->clusterMode) {
								_cluster->srem(key, memberId);
								_cluster->del("member:{" + controllerId + "}:" + networkId + ":" + memberId);
							}
							else {
								_redis->srem(key, memberId);
								_redis->del("member:{" + controllerId + "}:" + networkId + ":" + memberId);
							}
						}
						catch (sw::redis::Error& e) {
							mspan->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
							fprintf(stderr, "ERROR: Error deleting member from Redis: %s\n", e.what());
						}
					}
				}
				else {
					fprintf(stderr, "%s ERROR: unknown objtype\n", _myAddressStr.c_str());
				}
			}
			catch (std::exception& e) {
				span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
				fprintf(stderr, "%s ERROR: Error getting objtype: %s\n", _myAddressStr.c_str(), e.what());
			}
			_pool->unborrow(c);
			c.reset();
		}
	}

	fprintf(stderr, "%s commitThread finished\n", _myAddressStr.c_str());
}

void CentralDB::notifyNewMember(const std::string& networkID, const std::string& memberID)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("CentralDB");
	auto span = tracer->StartSpan("CentralDB::notifyNewMember");
	auto scope = tracer->WithActiveSpan(span);

	rustybits::smee_client_notify_network_joined(_smee, networkID.c_str(), memberID.c_str());
}

void CentralDB::onlineNotificationThread()
{
	waitForReady();
	while (_run == 1) {
		{
			auto provider = opentelemetry::trace::Provider::GetTracerProvider();
			auto tracer = provider->GetTracer("CentralDB");
			auto span = tracer->StartSpan("CentralDB::onlineNotificationThread");
			auto scope = tracer->WithActiveSpan(span);

			try {
				std::unordered_map<std::pair<uint64_t, uint64_t>, NodeOnlineRecord, _PairHasher> lastOnline;
				{
					std::lock_guard<std::mutex> l(_lastOnline_l);
					lastOnline.swap(_lastOnline);
				}

				uint64_t updateCount = 0;
				auto c = _pool->borrow();
				pqxx::work w(*c->c);
				for (auto i = lastOnline.begin(); i != lastOnline.end(); ++i) {
					updateCount += 1;
					uint64_t nwid_i = i->first.first;
					char nwidTmp[64];
					char memTmp[64];
					char ipTmp[64];
					OSUtils::ztsnprintf(nwidTmp, sizeof(nwidTmp), "%.16llx", nwid_i);
					OSUtils::ztsnprintf(memTmp, sizeof(memTmp), "%.10llx", i->first.second);
					nlohmann::json network, member;

					if (! get(nwid_i, network, i->first.second, member)) {
						continue;	// skip non existent networks/members
					}

					std::string networkId(nwidTmp);
					std::string memberId(memTmp);

					try {
						// check if the member exists first.
						//
						// exec_params1 will throw pqxx::unexpected_rows if not exactly one row is returned.  If that's
						// the case, skip this record and move on.
						pqxx::row r =
							w.exec(
								 "SELECT device_id, network_id FROM network_memberships_ctl WHERE network_id = "
								 "$1 AND device_id "
								 "= $2",
								 pqxx::params { networkId, memberId })
								.one_row();
					}
					catch (pqxx::unexpected_rows& e) {
						continue;
					}

					int64_t ts = i->second.lastSeen;
					std::string ipAddr = i->second.physicalAddress.toIpString(ipTmp);
					std::string timestamp = std::to_string(ts);
					std::string osArch = i->second.osArch;
					std::vector<std::string> osArchSplit = split(osArch, '/');
					std::string os = "unknown";
					std::string arch = "unknown";
					std::string frontend = OSUtils::jsonString(network["frontend"], "");

					int vMajor = OSUtils::jsonInt(member["vMajor"], 0);
					int vMinor = OSUtils::jsonInt(member["vMinor"], 0);
					int vRev = OSUtils::jsonInt(member["vRev"], 0);
					std::string version;
					if (vMajor <= 0 && vMinor <= 0 && vRev <= 0) {
						vMajor = 0;
						vMinor = 0;
						vRev = 0;
						version = "unknown";
					}
					else {
						version =
							"v" + std::to_string(vMajor) + "." + std::to_string(vMinor) + "." + std::to_string(vRev);
					}
					if (osArchSplit.size() == 2) {
						os = osArchSplit[0];
						arch = osArchSplit[1];
					}

					_statusWriter->updateNodeStatus(
						networkId, memberId, os, arch, version, i->second.physicalAddress, ts, frontend);
				}
				_statusWriter->writePending();
				w.commit();
				_pool->unborrow(c);
			}
			catch (std::exception& e) {
				fprintf(stderr, "%s: error in onlinenotification thread: %s\n", _myAddressStr.c_str(), e.what());
			}
		}

		std::this_thread::sleep_for(std::chrono::seconds(10));
	}
}

nlohmann::json CentralDB::_getNetworkMember(pqxx::work& tx, const std::string networkID, const std::string memberID)
{
	nlohmann::json out;

	try {
		pqxx::row row =
			tx.exec(
				  "SELECT nm.device_id, nm.network_id, nm.authorized, nm.active_bridge, nm.ip_assignments, "
				  "nm.no_auto_assign_ips, "
				  "nm.sso_exempt, (EXTRACT(EPOCH FROM nm.authentication_expiry_time AT TIME ZONE 'UTC')*1000)::bigint, "
				  "(EXTRACT(EPOCH FROM nm.creation_time AT TIME ZONE 'UTC')*1000)::bigint, nm.identity, "
				  "(EXTRACT(EPOCH FROM nm.last_authorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
				  "(EXTRACT(EPOCH FROM nm.last_deauthorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
				  "nm.remote_trace_level, nm.remote_trace_target, nm.revision, nm.capabilities, nm.tags, "
				  "nm.frontend "
				  "FROM network_memberships_ctl nm "
				  "INNER JOIN networks_ctl n "
				  "  ON nm.network_id = n.id "
				  "WHERE nm.network_id = $1 AND nm.device_id = $2",
				  pqxx::params { networkID, memberID })
				.one_row();

		bool authorized = row[2].as<bool>();
		std::optional<bool> active_bridge =
			row[3].is_null() ? std::optional<bool>() : std::optional<bool>(row[3].as<bool>());
		std::string ip_assignments = row[4].is_null() ? "{}" : row[4].as<std::string>();
		std::optional<bool> no_auto_assign_ips =
			row[5].is_null() ? std::optional<bool>() : std::optional<bool>(row[5].as<bool>());
		std::optional<bool> sso_exempt =
			row[6].is_null() ? std::optional<bool>() : std::optional<bool>(row[6].as<bool>());
		std::optional<uint64_t> authentication_expiry_time =
			row[7].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[7].as<uint64_t>());
		std::optional<uint64_t> creation_time =
			row[8].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[8].as<uint64_t>());
		std::optional<std::string> identity =
			row[9].is_null() ? std::optional<std::string>() : std::optional<std::string>(row[9].as<std::string>());
		std::optional<uint64_t> last_authorized_time =
			row[10].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[10].as<uint64_t>());
		std::optional<uint64_t> last_deauthorized_time =
			row[11].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[11].as<uint64_t>());
		std::optional<int32_t> remote_trace_level =
			row[12].is_null() ? std::optional<int32_t>() : std::optional<int32_t>(row[12].as<int32_t>());
		std::optional<std::string> remote_trace_target =
			row[13].is_null() ? std::optional<std::string>() : std::optional<std::string>(row[13].as<std::string>());
		std::optional<uint64_t> revision =
			row[14].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[14].as<uint64_t>());
		std::optional<std::string> capabilities =
			row[15].is_null() ? std::optional<std::string>() : std::optional<std::string>(row[15].as<std::string>());
		std::optional<std::string> tags =
			row[16].is_null() ? std::optional<std::string>() : std::optional<std::string>(row[16].as<std::string>());
		std::string frontend = row[17].is_null() ? "" : row[17].as<std::string>();

		out["objtype"] = "member";
		out["id"] = memberID;
		out["nwid"] = networkID;
		out["address"] = identity.value_or("");
		out["authorized"] = authorized;
		out["activeBridge"] = active_bridge.value_or(false);
		out["ipAssignments"] = json::array();
		if (ip_assignments != "{}" && ip_assignments != "[]") {
			std::string tmp = ip_assignments.substr(1, ip_assignments.length() - 2);
			std::vector<std::string> addrs = split(tmp, ',');
			for (auto it = addrs.begin(); it != addrs.end(); ++it) {
				out["ipAssignments"].push_back(*it);
			}
		}
		out["capabilities"] = json::parse(capabilities.value_or("[]"));
		out["creationTime"] = creation_time.value_or(0);
		out["lastAuthorizedTime"] = last_authorized_time.value_or(0);
		out["lastDeauthorizedTime"] = last_deauthorized_time.value_or(0);
		out["noAutoAssignIps"] = no_auto_assign_ips.value_or(false);
		out["remoteTraceLevel"] = remote_trace_level.value_or(0);
		out["remoteTraceTarget"] = remote_trace_target.value_or(nullptr);
		out["revision"] = revision.value_or(0);
		out["ssoExempt"] = sso_exempt.value_or(false);
		out["authenticationExpiryTime"] = authentication_expiry_time.value_or(0);
		out["tags"] = json::parse(tags.value_or("[]"));
		out["frontend"] = frontend;
	}
	catch (std::exception& e) {
		fprintf(
			stderr, "ERROR: Error getting network member %s-%s: %s\n", networkID.c_str(), memberID.c_str(), e.what());
		return nlohmann::json();
	}

	return out;
}

nlohmann::json CentralDB::_getNetwork(pqxx::work& tx, const std::string networkID)
{
	nlohmann::json out;

	try {
		std::optional<std::string> name;
		std::string cfg;
		std::optional<uint64_t> creation_time;
		std::optional<uint64_t> last_modified;
		std::optional<uint64_t> revision;
		std::string frontend;

		pqxx::row row = tx.exec(
							  "SELECT id, name, configuration , (EXTRACT(EPOCH FROM creation_time AT TIME ZONE "
							  "'UTC')*1000)::bigint, "
							  "(EXTRACT(EPOCH FROM last_modified AT TIME ZONE 'UTC')*1000)::bigint, revision, frontend "
							  "FROM networks_ctl WHERE id = $1",
							  pqxx::params { networkID })
							.one_row();

		cfg = row[2].as<std::string>();
		creation_time = row[3].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[3].as<uint64_t>());
		last_modified = row[4].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[4].as<uint64_t>());
		revision = row[5].is_null() ? std::optional<uint64_t>() : std::optional<uint64_t>(row[5].as<uint64_t>());
		frontend = row[6].is_null() ? "" : row[6].as<std::string>();

		nlohmann::json cfgtmp = nlohmann::json::parse(cfg);
		if (! cfgtmp.is_object()) {
			fprintf(stderr, "ERROR: Network %s configuration is not a JSON object\n", networkID.c_str());
			return nlohmann::json();
		}

		out["objtype"] = "network";
		out["id"] = row[0].as<std::string>();
		out["name"] = row[1].is_null() ? "" : row[1].as<std::string>();
		out["creationTime"] = creation_time.value_or(0);
		out["lastModified"] = last_modified.value_or(0);
		out["revision"] = revision.value_or(0);
		out["capabilities"] = cfgtmp["capabilities"].is_array() ? cfgtmp["capabilities"] : json::array();
		out["enableBroadcast"] = cfgtmp["enableBroadcast"].is_boolean() ? cfgtmp["enableBroadcast"].get<bool>() : false;
		out["mtu"] = cfgtmp["mtu"].is_number() ? cfgtmp["mtu"].get<int32_t>() : 2800;
		out["multicastLimit"] = cfgtmp["multicastLimit"].is_number() ? cfgtmp["multicastLimit"].get<int32_t>() : 64;
		out["private"] = cfgtmp["private"].is_boolean() ? cfgtmp["private"].get<bool>() : true;
		out["remoteTraceLevel"] =
			cfgtmp["remoteTraceLevel"].is_number() ? cfgtmp["remoteTraceLevel"].get<int32_t>() : 0;
		out["remoteTraceTarget"] =
			cfgtmp["remoteTraceTarget"].is_string() ? cfgtmp["remoteTraceTarget"].get<std::string>() : "";
		out["revision"] = revision.value_or(0);
		out["rules"] = cfgtmp["rules"].is_array() ? cfgtmp["rules"] : json::array();
		out["tags"] = cfgtmp["tags"].is_array() ? cfgtmp["tags"] : json::array();
		if (cfgtmp["v4AssignMode"].is_object()) {
			out["v4AssignMode"] = cfgtmp["v4AssignMode"];
		}
		else {
			out["v4AssignMode"] = json::object();
			out["v4AssignMode"]["zt"] = true;
		}
		if (cfgtmp["v6AssignMode"].is_object()) {
			out["v6AssignMode"] = cfgtmp["v6AssignMode"];
		}
		else {
			out["v6AssignMode"] = json::object();
			out["v6AssignMode"]["zt"] = true;
			out["v6AssignMode"]["6plane"] = true;
			out["v6AssignMode"]["rfc4193"] = false;
		}
		out["ssoEnabled"] = cfgtmp["ssoEnabled"].is_boolean() ? cfgtmp["ssoEnabled"].get<bool>() : false;
		out["objtype"] = "network";
		out["routes"] = cfgtmp["routes"].is_array() ? cfgtmp["routes"] : json::array();
		out["clientId"] = cfgtmp["clientId"].is_string() ? cfgtmp["clientId"].get<std::string>() : "";
		out["authorizationEndpoint"] =
			cfgtmp["authorizationEndpoint"].is_string() ? cfgtmp["authorizationEndpoint"].get<std::string>() : nullptr;
		out["provider"] = cfgtmp["ssoProvider"].is_string() ? cfgtmp["ssoProvider"].get<std::string>() : "";
		if (! cfgtmp["dns"].is_object()) {
			cfgtmp["dns"] = json::object();
			cfgtmp["dns"]["domain"] = "";
			cfgtmp["dns"]["servers"] = json::array();
		}
		else {
			out["dns"] = cfgtmp["dns"];
		}
		out["ipAssignmentPools"] = cfgtmp["ipAssignmentPools"].is_array() ? cfgtmp["ipAssignmentPools"] : json::array();
		out["frontend"] = row[6].as<std::string>();
	}
	catch (std::exception& e) {
		fprintf(stderr, "ERROR: Error getting network %s: %s\n", networkID.c_str(), e.what());
		return nlohmann::json();
	}
	return out;
}

#endif	 // ZT_CONTROLLER_USE_LIBPQ
