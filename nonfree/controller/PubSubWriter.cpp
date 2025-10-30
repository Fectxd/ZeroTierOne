#include "PubSubWriter.hpp"

#include "../../osdep/OSUtils.hpp"
#include "CtlUtil.hpp"
#include "OtelCarrier.hpp"
#include "member.pb.h"
#include "member_status.pb.h"
#include "network.pb.h"
#include "opentelemetry/context/propagation/global_propagator.h"

#include <chrono>
#include <google/cloud/options.h>
#include <google/cloud/pubsub/message.h>
#include <google/cloud/pubsub/publisher.h>
#include <google/cloud/pubsub/topic.h>
#include <opentelemetry/trace/provider.h>

namespace pubsub = ::google::cloud::pubsub;

namespace ZeroTier {

pbmessages::NetworkChange*
networkChangeFromJson(std::string controllerID, const nlohmann::json& oldNetwork, const nlohmann::json& newNetwork);
pbmessages::MemberChange*
memberChangeFromJson(std::string controllerID, const nlohmann::json& oldMember, const nlohmann::json& newMember);

PubSubWriter::PubSubWriter(std::string project, std::string topic, std::string controller_id)
	: _controller_id(controller_id)
	, _project(project)
	, _topic(topic)
{
	fprintf(
		stderr, "PubSubWriter for controller %s project %s topic %s\n", controller_id.c_str(), project.c_str(),
		topic.c_str());
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	// If PUBSUB_EMULATOR_HOST is set, create the topic if it doesn't exist
	const char* emulatorHost = std::getenv("PUBSUB_EMULATOR_HOST");
	if (emulatorHost != nullptr) {
		create_gcp_pubsub_topic_if_needed(project, topic);
	}

	auto options =
		::google::cloud::Options {}
			.set<pubsub::RetryPolicyOption>(pubsub::LimitedTimeRetryPolicy(std::chrono::seconds(5)).clone())
			.set<pubsub::BackoffPolicyOption>(
				pubsub::ExponentialBackoffPolicy(std::chrono::milliseconds(100), std::chrono::seconds(2), 1.3).clone())
			.set<pubsub::MessageOrderingOption>(true);
	auto publisher = pubsub::MakePublisherConnection(pubsub::Topic(project, topic), std::move(options));
	_publisher = std::make_shared<pubsub::Publisher>(std::move(publisher));
}

PubSubWriter::~PubSubWriter()
{
}

bool PubSubWriter::publishMessage(
	const std::string& payload,
	const std::string& frontend,
	const std::string& orderingKey)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("PubSubWriter");
	auto span = tracer->StartSpan("PubSubWriter::publishMessage");
	auto scope = tracer->WithActiveSpan(span);

	fprintf(stderr, "Publishing message to %s\n", _topic.c_str());
	std::vector<std::pair<std::string, std::string> > attributes;
	attributes.emplace_back("controller_id", _controller_id);

	std::map<std::string, std::string> attrs_map;
	OtelCarrier<std::map<std::string, std::string> > carrier(attrs_map);
	auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();
	auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
	propagator->Inject(carrier, current_ctx);

	for (const auto& kv : attrs_map) {
		fprintf(stderr, "Attributes injected: %s=%s\n", kv.first.c_str(), kv.second.c_str());
		attributes.emplace_back(kv.first, kv.second);
	}

	if (! frontend.empty()) {
		attributes.emplace_back("frontend", frontend);
	}

	auto msg_tmp = pubsub::MessageBuilder {}.SetData(payload).SetAttributes(attributes);
	if (! orderingKey.empty()) {
		msg_tmp.SetOrderingKey(orderingKey);
	}
	auto msg = std::move(msg_tmp).Build();
	auto message_id = _publisher->Publish(std::move(msg)).get();
	if (! message_id) {
		fprintf(stderr, "Failed to publish message: %s\n", std::move(message_id).status().message().c_str());
		return false;
	}

	fprintf(stderr, "Published message to %s\n", _topic.c_str());
	return true;
}

bool PubSubWriter::publishNetworkChange(
	const nlohmann::json& oldNetwork,
	const nlohmann::json& newNetwork,
	const std::string& frontend)
{
	fprintf(stderr, "Publishing network change\n");
	pbmessages::NetworkChange* nc = networkChangeFromJson(_controller_id, oldNetwork, newNetwork);

	std::string networkID;
	if (nc->has_new_()) {
		networkID = nc->new_().network_id();
	}
	else if (nc->has_old()) {
		networkID = nc->old().network_id();
	}

	std::string payload;
	if (! nc->SerializeToString(&payload)) {
		fprintf(stderr, "Failed to serialize NetworkChange protobuf message\n");
		delete nc;
		return false;
	}
	delete nc;
	return publishMessage(payload, frontend, networkID);
}

bool PubSubWriter::publishMemberChange(
	const nlohmann::json& oldMember,
	const nlohmann::json& newMember,
	const std::string& frontend)
{
	fprintf(stderr, "Publishing member change\n");
	pbmessages::MemberChange* mc = memberChangeFromJson(_controller_id, oldMember, newMember);
	std::string memberID;
	if (mc->has_new_()) {
		memberID = mc->new_().network_id() + "-" + mc->new_().device_id();
	}
	else if (mc->has_old()) {
		memberID = mc->old().network_id() + "-" + mc->old().device_id();
	}

	std::string payload;
	if (! mc->SerializeToString(&payload)) {
		fprintf(stderr, "Failed to serialize MemberChange protobuf message\n");
		delete mc;
		return false;
	}

	delete mc;
	return publishMessage(payload, frontend, memberID);
}

bool PubSubWriter::publishStatusChange(
	std::string frontend,
	std::string network_id,
	std::string node_id,
	std::string os,
	std::string arch,
	std::string version,
	int64_t last_seen)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("PubSubWriter");
	auto span = tracer->StartSpan("PubSubWriter::publishStatusChange");
	auto scope = tracer->WithActiveSpan(span);

	pbmessages::MemberStatus_MemberStatusMetadata* metadata = new pbmessages::MemberStatus_MemberStatusMetadata();
	metadata->set_controller_id(_controller_id);
	metadata->set_trace_id("");	  // TODO: generate a trace ID

	pbmessages::MemberStatus ms;
	ms.set_network_id(network_id);
	ms.set_member_id(node_id);
	ms.set_os(os);
	ms.set_arch(arch);
	ms.set_version(version);
	ms.set_timestamp(last_seen);
	ms.set_allocated_metadata(metadata);

	std::string payload;
	if (! ms.SerializeToString(&payload)) {
		fprintf(stderr, "Failed to serialize StatusChange protobuf message\n");
		return false;
	}

	return publishMessage(payload, "", "");
}

pbmessages::NetworkChange_Network* networkFromJson(const nlohmann::json& j)
{
	if (! j.is_object()) {
		return nullptr;
	}

	pbmessages::NetworkChange_Network* n = new pbmessages::NetworkChange_Network();
	try {
		n->set_network_id(OSUtils::jsonString(j["id"], ""));
		n->set_name(OSUtils::jsonString(j["name"], ""));
		n->set_capabilities(OSUtils::jsonDump(j.value("capabilities", "[]"), -1));
		n->set_creation_time(OSUtils::jsonInt(j["creationTime"], 0));
		n->set_enable_broadcast(OSUtils::jsonBool(j["enableBroadcast"], false));

		for (const auto& p : j["ipAssignmentPools"]) {
			if (p.is_object()) {
				auto pool = n->add_assignment_pools();
				pool->set_start_ip(OSUtils::jsonString(p["ipRangeStart"], ""));
				pool->set_end_ip(OSUtils::jsonString(p["ipRangeEnd"], ""));
			}
		}

		n->set_mtu(OSUtils::jsonInt(j["mtu"], 2800));
		n->set_multicast_limit(OSUtils::jsonInt(j["multicastLimit"], 32));
		n->set_is_private(OSUtils::jsonBool(j["private"], true));
		n->set_remote_trace_level(OSUtils::jsonInt(j["remoteTraceLevel"], 0));
		n->set_remote_trace_target(OSUtils::jsonString(j["remoteTraceTarget"], ""));
		n->set_revision(OSUtils::jsonInt(j["revision"], 0));

		for (const auto& p : j["routes"]) {
			if (p.is_object()) {
				auto r = n->add_routes();
				r->set_target(OSUtils::jsonString(p["target"], ""));
				r->set_via(OSUtils::jsonString(p["via"], ""));
			}
		}
		std::string rules;
		if (j["rules"].is_array()) {
			rules = OSUtils::jsonDump(j["rules"], -1);
		}
		else {
			rules = "[]";
		}
		n->set_rules(rules);

		std::string tags;
		if (j["tags"].is_array()) {
			tags = OSUtils::jsonDump(j["tags"], -1);
		}
		else {
			tags = "[]";
		}
		n->set_tags(tags);

		pbmessages::NetworkChange_IPV4AssignMode* v4am = new pbmessages::NetworkChange_IPV4AssignMode();
		if (j["v4AssignMode"].is_object()) {
			nlohmann::json am = j["v4AssignMode"];
			v4am->set_zt(OSUtils::jsonBool(am["zt"], false));
		}
		n->set_allocated_ipv4_assign_mode(v4am);

		pbmessages::NetworkChange_IPV6AssignMode* v6am = new pbmessages::NetworkChange_IPV6AssignMode();
		if (j["v6AssignMode"].is_object()) {
			nlohmann::json am = j["v6AssignMode"];
			v6am->set_zt(OSUtils::jsonBool(am["zt"], false));
			v6am->set_six_plane(OSUtils::jsonBool(am["6plane"], false));
			v6am->set_rfc4193(OSUtils::jsonBool(am["rfc4193"], false));
		}
		n->set_allocated_ipv6_assign_mode(v6am);

		nlohmann::json jdns = j["dns"];
		if (jdns.is_object()) {
			pbmessages::NetworkChange_DNS* dns = new pbmessages::NetworkChange_DNS();
			dns->set_domain(jdns.value("domain", ""));
			for (const auto& s : jdns["servers"]) {
				if (s.is_string()) {
					auto server = dns->add_nameservers();
					*server = s;
				}
			}
			n->set_allocated_dns(dns);
		}

		n->set_sso_enabled(OSUtils::jsonBool(j["ssoEnabled"], false));
		nlohmann::json ssocfg = j["ssoConfig"];
		if (ssocfg.is_object()) {
			n->set_sso_provider(OSUtils::jsonString(ssocfg["provider"], ""));
			n->set_sso_client_id(OSUtils::jsonString(ssocfg["clientId"], ""));
			n->set_sso_authorization_endpoint(OSUtils::jsonString(ssocfg["authorizationEndpoint"], ""));
			n->set_sso_issuer(OSUtils::jsonString(ssocfg["issuer"], ""));
			n->set_sso_provider(OSUtils::jsonString(ssocfg["provider"], ""));
		}

		n->set_rules_source(OSUtils::jsonString(j["rulesSource"], ""));
	}
	catch (const std::exception& e) {
		fprintf(stderr, "Exception parsing network JSON: %s\n", e.what());
		delete n;
		return nullptr;
	}

	return n;
}

pbmessages::NetworkChange*
networkChangeFromJson(std::string controllerID, const nlohmann::json& oldNetwork, const nlohmann::json& newNetwork)
{
	pbmessages::NetworkChange* nc = new pbmessages::NetworkChange();

	nc->set_allocated_old(networkFromJson(oldNetwork));
	nc->set_allocated_new_(networkFromJson(newNetwork));
	nc->set_change_source(pbmessages::NetworkChange_ChangeSource::NetworkChange_ChangeSource_CONTROLLER);

	pbmessages::NetworkChange_NetworkChangeMetadata* metadata = new pbmessages::NetworkChange_NetworkChangeMetadata();
	metadata->set_controller_id(controllerID);
	metadata->set_trace_id("");	  // TODO: generate a trace ID
	nc->set_allocated_metadata(metadata);

	return nc;
}

pbmessages::MemberChange_Member* memberFromJson(const nlohmann::json& j)
{
	if (! j.is_object()) {
		fprintf(stderr, "memberFromJson: JSON is not an object\n");
		return nullptr;
	}

	fprintf(stderr, "memberFromJSON: %s\n", j.dump().c_str());

	pbmessages::MemberChange_Member* m = new pbmessages::MemberChange_Member();
	try {
		m->set_network_id(OSUtils::jsonString(j["nwid"], ""));
		m->set_device_id(OSUtils::jsonString(j["id"], ""));
		m->set_identity(OSUtils::jsonString(j["identity"], ""));
		m->set_authorized(OSUtils::jsonBool(j["authorized"], false));
		if (j["ipAssignments"].is_array()) {
			for (const auto& addr : j["ipAssignments"]) {
				if (addr.is_string()) {
					auto a = m->add_ip_assignments();
					std::string address = addr.get<std::string>();
					*a = address;
				}
			}
		}
		m->set_active_bridge(OSUtils::jsonBool(j["activeBridge"], false));
		if (j["tags"].is_array()) {
			nlohmann::json tags = j["tags"];
			std::string tagsStr = OSUtils::jsonDump(tags, -1);
			m->set_tags(tagsStr);
		}
		else {
			nlohmann::json tags = nlohmann::json::array();
			std::string tagsStr = OSUtils::jsonDump(tags, -1);
			m->set_tags(tagsStr);
		}
		if (j["capabilities"].is_array()) {
			nlohmann::json caps = j["capabilities"];
			std::string capsStr = OSUtils::jsonDump(caps, -1);
			m->set_capabilities(capsStr);
		}
		else {
			nlohmann::json caps = nlohmann::json::array();
			std::string capsStr = OSUtils::jsonDump(caps, -1);
			m->set_capabilities(capsStr);
		}
		m->set_creation_time(OSUtils::jsonInt(j["creationTime"], 0));
		m->set_no_auto_assign_ips(OSUtils::jsonBool(j["noAutoAssignIps"], false));
		m->set_revision(OSUtils::jsonInt(j["revision"], 0));
		m->set_last_authorized_time(OSUtils::jsonInt(j["lastAuthorizedTime"], 0));
		m->set_last_deauthorized_time(OSUtils::jsonInt(j["lastDeauthorizedTime"], 0));
		m->set_last_authorized_credential_type(OSUtils::jsonString(j["lastAuthorizedCredentialType"], ""));
		m->set_last_authorized_credential(OSUtils::jsonString(j["lastAuthorizedCredential"], ""));
		m->set_version_major(OSUtils::jsonInt(j["versionMajor"], 0));
		m->set_version_minor(OSUtils::jsonInt(j["versionMinor"], 0));
		m->set_version_rev(OSUtils::jsonInt(j["versionRev"], 0));
		m->set_version_protocol(OSUtils::jsonInt(j["versionProtocol"], 0));
		m->set_remote_trace_level(OSUtils::jsonInt(j["remoteTraceLevel"], 0));
		m->set_remote_trace_target(OSUtils::jsonString(j["remoteTraceTarget"], ""));
		m->set_sso_exempt(OSUtils::jsonBool(j["ssoExempt"], false));
		m->set_auth_expiry_time(OSUtils::jsonInt(j["authExpiryTime"], 0));
	}
	catch (const std::exception& e) {
		fprintf(stderr, "Exception parsing member JSON: %s\n", e.what());
		delete m;
		return nullptr;
	}
	fprintf(stderr, "memberFromJSON complete\n");
	return m;
}

pbmessages::MemberChange*
memberChangeFromJson(std::string controllerID, const nlohmann::json& oldMember, const nlohmann::json& newMember)
{
	fprintf(stderr, "memberrChangeFromJson: old: %s\n", oldMember.dump().c_str());
	fprintf(stderr, "memberrChangeFromJson: new: %s\n", newMember.dump().c_str());
	pbmessages::MemberChange* mc = new pbmessages::MemberChange();
	pbmessages::MemberChange_Member* om = memberFromJson(oldMember);
	if (om != nullptr) {
		mc->set_allocated_old(om);
	}
	pbmessages::MemberChange_Member* nm = memberFromJson(newMember);
	if (nm != nullptr) {
		mc->set_allocated_new_(nm);
	}
	mc->set_change_source(pbmessages::MemberChange_ChangeSource::MemberChange_ChangeSource_CONTROLLER);

	pbmessages::MemberChange_MemberChangeMetadata* metadata = new pbmessages::MemberChange_MemberChangeMetadata();
	metadata->set_controller_id(controllerID);
	metadata->set_trace_id("");	  // TODO: generate a trace ID
	mc->set_allocated_metadata(metadata);

	return mc;
}

}	// namespace ZeroTier