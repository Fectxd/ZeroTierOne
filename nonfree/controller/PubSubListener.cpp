#ifdef ZT_CONTROLLER_USE_LIBPQ
#include "PubSubListener.hpp"

#include "ControllerConfig.hpp"
#include "CtlUtil.hpp"
#include "DB.hpp"
#include "OtelCarrier.hpp"
#include "member.pb.h"
#include "network.pb.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/trace/tracer.h"
#include "rustybits.h"

#include <google/cloud/opentelemetry_options.h>
#include <google/cloud/pubsub/admin/subscription_admin_client.h>
#include <google/cloud/pubsub/admin/subscription_admin_connection.h>
#include <google/cloud/pubsub/admin/topic_admin_client.h>
#include <google/cloud/pubsub/message.h>
#include <google/cloud/pubsub/subscriber.h>
#include <google/cloud/pubsub/subscription.h>
#include <google/cloud/pubsub/topic.h>
#include <nlohmann/json.hpp>

namespace pubsub = ::google::cloud::pubsub;
namespace pubsub_admin = ::google::cloud::pubsub_admin;

namespace ZeroTier {

nlohmann::json toJson(const pbmessages::NetworkChange_Network& nc, pbmessages::NetworkChange_ChangeSource source);
nlohmann::json toJson(const pbmessages::MemberChange_Member& mc, pbmessages::MemberChange_ChangeSource source);

PubSubListener::PubSubListener(std::string controller_id, std::string project, std::string topic)
	: _controller_id(controller_id)
	, _project(project)
	, _topic(topic)
	, _subscription_id()
	, _run(false)
	, _adminClient(pubsub_admin::MakeSubscriptionAdminConnection())
	, _subscription(nullptr)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	_subscription_id = "sub-" + controller_id + "-" + topic;   // + "-" + random_hex_string(8);
	_subscription = new pubsub::Subscription(_project, _subscription_id);
	fprintf(
		stderr, "PubSubListener for controller %s project %s topic %s subscription %s\n", controller_id.c_str(),
		project.c_str(), topic.c_str(), _subscription_id.c_str());

	// If PUBSUB_EMULATOR_HOST is set, create the topic if it doesn't exist
	const char* emulatorHost = std::getenv("PUBSUB_EMULATOR_HOST");
	if (emulatorHost != nullptr) {
		create_gcp_pubsub_topic_if_needed(project, topic);
		create_gcp_pubsub_subscription_if_needed(_project, _subscription_id, _topic, _controller_id);
	}

	_subscriber = std::make_shared<pubsub::Subscriber>(
		pubsub::MakeSubscriberConnection(*_subscription),
		google::cloud::Options {}.set<google::cloud::OpenTelemetryTracingOption>(true));

	_run = true;
	_subscriberThread = std::thread(&PubSubListener::subscribe, this);
}

PubSubListener::~PubSubListener()
{
	_run = false;
	if (_subscriberThread.joinable()) {
		_subscriberThread.join();
	}

	if (_subscription) {
		delete _subscription;
		_subscription = nullptr;
	}
}

void PubSubListener::subscribe()
{
	while (_run) {
		try {
			fprintf(stderr, "Starting new subscription session\n");
			auto session = _subscriber->Subscribe([this](pubsub::Message const& m, pubsub::AckHandler h) {
				auto provider = opentelemetry::trace::Provider::GetTracerProvider();
				auto tracer = provider->GetTracer("PubSubListener");

				auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
				auto attrs = m.attributes();
				std::map<std::string, std::string> attrs_map;
				for (auto const& kv : m.attributes()) {
					fprintf(stderr, "Message attribute: %s=%s\n", kv.first.c_str(), kv.second.c_str());
					attrs_map.emplace(kv.first, kv.second);
				}

				OtelCarrier<std::map<std::string, std::string> > carrier(attrs_map);

				auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
				auto new_context = propagator->Extract(carrier, current_ctx);
				auto remote_span = opentelemetry::trace::GetSpan(new_context);
				auto remote_scope = tracer->WithActiveSpan(remote_span);

				{
					auto span = tracer->StartSpan("PubSubListener::onMessage");
					auto scope = tracer->WithActiveSpan(span);
					span->SetAttribute("message_id", m.message_id());
					span->SetAttribute("ordering_key", m.ordering_key());

					fprintf(stderr, "Received message %s\n", m.message_id().c_str());
					if (onNotification(m.data())) {
						std::move(h).ack();
						span->SetStatus(opentelemetry::trace::StatusCode::kOk);
						return true;
					}
					else {
						span->SetStatus(opentelemetry::trace::StatusCode::kError, "onNotification failed");
						return false;
					}
				}
			});

			auto result = session.wait_for(std::chrono::seconds(10));
			if (result == std::future_status::timeout) {
				session.cancel();
				continue;
			}

			if (! session.valid()) {
				fprintf(stderr, "Subscription session no longer valid\n");
				continue;
			}
		}
		catch (google::cloud::Status const& status) {
			fprintf(stderr, "Subscription terminated with status: %s\n", status.message().c_str());
		}
	}
}

PubSubNetworkListener::PubSubNetworkListener(std::string controller_id, std::string project, std::string topic, DB* db)
	: PubSubListener(controller_id, project, topic)
	, _db(db)
{
}

PubSubNetworkListener::~PubSubNetworkListener()
{
}

bool PubSubNetworkListener::onNotification(const std::string& payload)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("PubSubNetworkListener");
	auto span = tracer->StartSpan("PubSubNetworkListener::onNotification");
	auto scope = tracer->WithActiveSpan(span);

	pbmessages::NetworkChange nc;
	if (! nc.ParseFromString(payload)) {
		fprintf(stderr, "Failed to parse NetworkChange protobuf message\n");
		span->SetAttribute("error", "Failed to parse NetworkChange protobuf message");
		span->SetStatus(opentelemetry::trace::StatusCode::kError, "Failed to parse protobuf");
		return false;
	}
	fprintf(stderr, "PubSubNetworkListener: parsed protobuf message. %s\n", nc.DebugString().c_str());
	fprintf(stderr, "Network notification received\n");

	try {
		nlohmann::json oldConfig, newConfig;

		if (nc.has_old()) {
			fprintf(stderr, "has old network config\n");
			oldConfig = toJson(nc.old(), nc.change_source());
		}

		if (nc.has_new_()) {
			fprintf(stderr, "has new network config\n");
			newConfig = toJson(nc.new_(), nc.change_source());
		}

		if (! nc.has_old() && ! nc.has_new_()) {
			fprintf(stderr, "NetworkChange message has no old or new network config\n");
			span->SetAttribute("error", "NetworkChange message has no old or new network config");
			span->SetStatus(opentelemetry::trace::StatusCode::kError, "No old or new config");
			return false;
		}

		if (oldConfig.is_object() && newConfig.is_object()) {
			// network modification
			std::string nwid = oldConfig["id"].get<std::string>();
			span->SetAttribute("action", "network_change");
			span->SetAttribute("network_id", nwid);
			_db->save(newConfig, _db->isReady());
		}
		else if (newConfig.is_object() && ! oldConfig.is_object()) {
			// new network
			std::string nwid = newConfig["id"];
			span->SetAttribute("network_id", nwid);
			span->SetAttribute("action", "new_network");
			_db->save(newConfig, _db->isReady());
		}
		else if (! newConfig.is_object() && oldConfig.is_object()) {
			// network deletion
			std::string nwid = oldConfig["id"];
			span->SetAttribute("action", "delete_network");
			span->SetAttribute("network_id", nwid);

			uint64_t networkId = Utils::hexStrToU64(nwid.c_str());
			if (networkId) {
				_db->eraseNetwork(networkId);
			}
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "PubSubNetworkListener JSON parse error: %s\n", e.what());
		span->SetAttribute("error", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		fprintf(stderr, "payload: %s\n", payload.c_str());
		return false;
	}
	catch (const std::exception& e) {
		fprintf(stderr, "PubSubNetworkListener Exception in PubSubNetworkListener: %s\n", e.what());
		span->SetAttribute("error", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		return false;
	}
	catch (...) {
		fprintf(stderr, "PubSubNetworkListener Unknown exception in PubSubNetworkListener\n");
		span->SetAttribute("error", "Unknown exception in PubSubNetworkListener");
		span->SetStatus(opentelemetry::trace::StatusCode::kError, "Unknown exception");
		return false;
	}
	fprintf(stderr, "PubSubNetworkListener onNotification complete\n");
	return true;
}

PubSubMemberListener::PubSubMemberListener(std::string controller_id, std::string project, std::string topic, DB* db)
	: PubSubListener(controller_id, project, topic)
	, _db(db)
{
}

PubSubMemberListener::~PubSubMemberListener()
{
}

bool PubSubMemberListener::onNotification(const std::string& payload)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("PubSubMemberListener");
	auto span = tracer->StartSpan("PubSubMemberListener::onNotification");
	auto scope = tracer->WithActiveSpan(span);

	pbmessages::MemberChange mc;
	if (! mc.ParseFromString(payload)) {
		fprintf(stderr, "Failed to parse MemberChange protobuf message\n");
		span->SetAttribute("error", "Failed to parse MemberChange protobuf message");
		span->SetStatus(opentelemetry::trace::StatusCode::kError, "Failed to parse protobuf");
		return false;
	}
	fprintf(stderr, "PubSubMemberListener: parsed protobuf message. %s\n", mc.DebugString().c_str());
	fprintf(stderr, "Member notification received");

	try {
		nlohmann::json tmp;
		nlohmann::json oldConfig, newConfig;

		if (mc.has_old()) {
			fprintf(stderr, "has old member config\n");
			oldConfig = toJson(mc.old(), mc.change_source());
		}

		if (mc.has_new_()) {
			fprintf(stderr, "has new member config\n");
			newConfig = toJson(mc.new_(), mc.change_source());
		}

		if (! mc.has_old() && ! mc.has_new_()) {
			fprintf(stderr, "MemberChange message has no old or new member config\n");
			span->SetAttribute("error", "MemberChange message has no old or new member config");
			span->SetStatus(opentelemetry::trace::StatusCode::kError, "No old or new config");
			return false;
		}

		if (oldConfig.is_object() && newConfig.is_object()) {
			// member modification
			std::string memberID = oldConfig["id"].get<std::string>();
			std::string networkID = oldConfig["nwid"].get<std::string>();
			span->SetAttribute("action", "member_change");
			span->SetAttribute("member_id", memberID);
			span->SetAttribute("network_id", networkID);
			_db->save(newConfig, _db->isReady());
		}
		else if (newConfig.is_object() && ! oldConfig.is_object()) {
			// new member
			std::string memberID = newConfig["id"].get<std::string>();
			std::string networkID = newConfig["nwid"].get<std::string>();
			span->SetAttribute("action", "new_member");
			span->SetAttribute("member_id", memberID);
			span->SetAttribute("network_id", networkID);
			_db->save(newConfig, _db->isReady());
		}
		else if (! newConfig.is_object() && oldConfig.is_object()) {
			// member deletion
			std::string memberID = oldConfig["id"].get<std::string>();
			std::string networkID = oldConfig["nwid"].get<std::string>();
			span->SetAttribute("action", "delete_member");
			span->SetAttribute("member_id", memberID);
			span->SetAttribute("network_id", networkID);

			uint64_t networkId = Utils::hexStrToU64(networkID.c_str());
			uint64_t memberId = Utils::hexStrToU64(memberID.c_str());
			if (networkId && memberId) {
				_db->eraseMember(networkId, memberId);
			}
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "PubSubMemberListener JSON parse error: %s\n", e.what());
		span->SetAttribute("error", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		fprintf(stderr, "payload: %s\n", payload.c_str());
		return false;
	}
	catch (const std::exception& e) {
		fprintf(stderr, "PubSubMemberListener Exception in PubSubMemberListener: %s\n", e.what());
		span->SetAttribute("error", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		return false;
	}
	return true;
}

nlohmann::json toJson(const pbmessages::NetworkChange_Network& nc, pbmessages::NetworkChange_ChangeSource source)
{
	nlohmann::json out;

	out["objtype"] = "network";
	out["id"] = nc.network_id();
	out["name"] = nc.name();

	try {
		std::string caps = nc.capabilities();
		if (caps.length() == 0) {
			out["capabilities"] = nlohmann::json::array();
		}
		else if (caps == "null") {
			out["capabilities"] = nlohmann::json::array();
		}
		else {
			out["capabilities"] = OSUtils::jsonParse(caps);
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "toJson Network capabilities JSON parse error: %s\n", e.what());
		out["capabilities"] = nlohmann::json::array();
	}

	out["mtu"] = nc.mtu();
	out["multicastLimit"] = nc.multicast_limit();
	out["private"] = nc.is_private();
	out["remoteTraceLevel"] = nc.remote_trace_level();
	if (nc.has_remote_trace_target()) {
		out["remoteTraceTarget"] = nc.remote_trace_target();
	}
	else {
		out["remoteTraceTarget"] = "";
	}

	try {
		std::string rules = nc.rules();
		if (rules.length() == 0) {
			out["rules"] = nlohmann::json::array();
		}
		else if (rules == "null") {
			out["rules"] = nlohmann::json::array();
		}
		else {
			out["rules"] = OSUtils::jsonParse(rules);
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "toJson Network rules JSON parse error: %s\n", e.what());
		out["rules"] = nlohmann::json::array();
	}

	out["rulesSource"] = nc.rules_source();

	try {
		std::string tags = nc.tags();
		if (tags.length() == 0) {
			out["tags"] = nlohmann::json::array();
		}
		else if (tags == "[]") {
			out["tags"] = nlohmann::json::array();
		}
		else {
			out["tags"] = OSUtils::jsonParse(tags);
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "toJson Network tags JSON parse error: %s\n", e.what());
		out["tags"] = nlohmann::json::array();
	}

	if (nc.has_ipv4_assign_mode()) {
		nlohmann::json ipv4mode;
		ipv4mode["zt"] = nc.ipv4_assign_mode().zt();
		out["v4AssignMode"] = ipv4mode;
	}
	else {
		nlohmann::json ipv4mode = nlohmann::json::object();
		out["zt"] = false;
		out["v4AssignMode"] = ipv4mode;
	}

	if (nc.has_ipv6_assign_mode()) {
		nlohmann::json ipv6mode;
		ipv6mode["6plane"] = nc.ipv6_assign_mode().six_plane();
		ipv6mode["rfc4193"] = nc.ipv6_assign_mode().rfc4193();
		ipv6mode["zt"] = nc.ipv6_assign_mode().zt();
		out["v6AssignMode"] = ipv6mode;
	}
	else {
		nlohmann::json ipv6mode = nlohmann::json::object();
		ipv6mode["6plane"] = false;
		ipv6mode["rfc4193"] = false;
		ipv6mode["zt"] = false;
		out["v6AssignMode"] = ipv6mode;
	}

	if (nc.assignment_pools_size() > 0) {
		nlohmann::json pools = nlohmann::json::array();
		for (const auto& p : nc.assignment_pools()) {
			nlohmann::json pool;
			pool["ipRangeStart"] = p.start_ip();
			pool["ipRangeEnd"] = p.end_ip();
			pools.push_back(pool);
		}
		out["ipAssignmentPools"] = pools;
	}
	else {
		out["ipAssignmentPools"] = nlohmann::json::array();
	}

	if (nc.routes_size() > 0) {
		nlohmann::json routes = nlohmann::json::array();
		for (const auto& r : nc.routes()) {
			nlohmann::json route;
			std::string target = r.target();
			if (target.length() > 0) {
				route["target"] = r.target();
				if (r.has_via()) {
					route["via"] = r.via();
				}
				else {
					route["via"] = nullptr;
				}
				routes.push_back(route);
			}
		}
		out["routes"] = routes;
	}

	if (nc.has_dns()) {
		nlohmann::json dns;
		if (nc.dns().nameservers_size() > 0) {
			nlohmann::json servers = nlohmann::json::array();
			for (const auto& s : nc.dns().nameservers()) {
				servers.push_back(s);
			}
			dns["servers"] = servers;
		}
		else {
			dns["servers"] = nlohmann::json::array();
		}
		dns["domain"] = nc.dns().domain();

		out["dns"] = dns;
	}

	out["ssoEnabled"] = nc.sso_enabled();
	nlohmann::json sso;
	if (nc.sso_enabled()) {
		sso = nlohmann::json::object();
		if (nc.has_sso_client_id()) {
			sso["ssoClientId"] = nc.sso_client_id();
		}

		if (nc.has_sso_authorization_endpoint()) {
			sso["ssoAuthorizationEndpoint"] = nc.sso_authorization_endpoint();
		}

		if (nc.has_sso_issuer()) {
			sso["ssoIssuer"] = nc.sso_issuer();
		}

		if (nc.has_sso_provider()) {
			sso["ssoProvider"] = nc.sso_provider();
		}
	}
	out["ssoConfig"] = sso;
	switch (source) {
		case pbmessages::NetworkChange_ChangeSource_CV1:
			out["change_source"] = "cv1";
			break;
		case pbmessages::NetworkChange_ChangeSource_CV2:
			out["change_source"] = "cv2";
			break;
		case pbmessages::NetworkChange_ChangeSource_CONTROLLER:
			out["change_source"] = "controller";
			break;
		default:
			out["change_source"] = "unknown";
			break;
	}

	return out;
}

nlohmann::json toJson(const pbmessages::MemberChange_Member& mc, pbmessages::MemberChange_ChangeSource source)
{
	nlohmann::json out;
	out["objtype"] = "member";
	out["id"] = mc.device_id();
	out["nwid"] = mc.network_id();
	if (mc.has_remote_trace_target()) {
		out["remoteTraceTarget"] = mc.remote_trace_target();
	}
	else {
		out["remoteTraceTarget"] = "";
	}
	out["authorized"] = mc.authorized();
	out["activeBridge"] = mc.active_bridge();

	auto ipAssignments = mc.ip_assignments();
	if (ipAssignments.size() > 0) {
		nlohmann::json assignments = nlohmann::json::array();
		for (const auto& ip : ipAssignments) {
			assignments.push_back(ip);
		}
		out["ipAssignments"] = assignments;
	}

	out["noAutoAssignIps"] = mc.no_auto_assign_ips();
	out["ssoExempt"] = mc.sso_exempt();
	out["authenticationExpiryTime"] = mc.auth_expiry_time();

	try {
		std::string caps = mc.capabilities();
		if (caps.length() == 0) {
			out["capabilities"] = nlohmann::json::array();
		}
		else if (caps == "null") {
			out["capabilities"] = nlohmann::json::array();
		}
		else {
			out["capabilities"] = OSUtils::jsonParse(caps);
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "MemberChange member capabilities JSON parse error: %s\n", e.what());
		fprintf(stderr, "capabilities: %s\n", mc.capabilities().c_str());
		out["capabilities"] = nlohmann::json::array();
	}

	out["creationTime"] = mc.creation_time();
	out["identity"] = mc.identity();
	out["lastAuthorizedTime"] = mc.last_authorized_time();
	out["lastDeauthorizedTime"] = mc.last_deauthorized_time();
	out["remoteTraceLevel"] = mc.remote_trace_level();
	out["revision"] = mc.revision();

	try {
		std::string tags = mc.tags();
		if (tags.length() == 0) {
			out["tags"] = nlohmann::json::array();
		}
		else if (tags == "null") {
			out["tags"] = nlohmann::json::array();
		}
		else {
			out["tags"] = OSUtils::jsonParse(tags);
		}
	}
	catch (const nlohmann::json::parse_error& e) {
		fprintf(stderr, "MemberChange member tags JSON parse error: %s\n", e.what());
		fprintf(stderr, "tags: %s\n", mc.tags().c_str());
		out["tags"] = nlohmann::json::array();
	}

	out["versionMajor"] = mc.version_major();
	out["versionMinor"] = mc.version_minor();
	out["versionRev"] = mc.version_rev();
	out["versionProtocol"] = mc.version_protocol();
	switch (source) {
		case pbmessages::MemberChange_ChangeSource_CV1:
			out["change_source"] = "cv1";
			break;
		case pbmessages::MemberChange_ChangeSource_CV2:
			out["change_source"] = "cv2";
			break;
		case pbmessages::MemberChange_ChangeSource_CONTROLLER:
			out["change_source"] = "controller";
			break;
		default:
			out["change_source"] = "unknown";
			break;
	}

	return out;
}

}	// namespace ZeroTier

#endif	 // ZT_CONTROLLER_USE_LIBPQ