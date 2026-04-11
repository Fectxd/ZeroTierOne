#ifdef ZT_CONTROLLER_USE_LIBPQ

#ifndef ZT_CONTROLLER_PUBSUBLISTENER_HPP
#define ZT_CONTROLLER_PUBSUBLISTENER_HPP

#include "ConnectionPool.hpp"
#include "NotificationListener.hpp"
#include "PostgreSQL.hpp"
#include "rustybits.h"

#include <google/cloud/pubsub/admin/subscription_admin_client.h>
#include <google/cloud/pubsub/subscriber.h>
#include <memory>
#include <string>
#include <thread>

namespace ZeroTier {
class DB;

/**
 * Base class for GCP PubSub listeners
 */
class PubSubListener : public NotificationListener {
  public:
	PubSubListener(std::string controller_id, std::string project, std::string topic);
	virtual ~PubSubListener();

	virtual bool onNotification(const std::string& payload) = 0;

  protected:
	std::string _controller_id;
	std::string _project;
	std::string _topic;
	std::string _subscription_id;

  private:
	void subscribe();
	bool _run = false;
	std::mutex _sessionMutex;
	google::cloud::future<google::cloud::Status> _session;
	bool _hasSession = false;
	std::atomic<std::chrono::steady_clock::time_point> _lastMessageTime;
	google::cloud::pubsub_admin::SubscriptionAdminClient _adminClient;
	google::cloud::pubsub::Subscription* _subscription;
	std::shared_ptr<google::cloud::pubsub::Subscriber> _subscriber;
	std::thread _subscriberThread;
};

/**
 * Listener for network notifications via GCP PubSub
 */
class PubSubNetworkListener : public PubSubListener {
  public:
	PubSubNetworkListener(std::string controller_id, std::string project, std::string topic, DB* db);
	virtual ~PubSubNetworkListener();

	virtual bool onNotification(const std::string& payload) override;

  private:
	DB* _db;
};

/**
 * Listener for member notifications via GCP PubSub
 */
class PubSubMemberListener : public PubSubListener {
  public:
	PubSubMemberListener(std::string controller_id, std::string project, std::string topic, DB* db);
	virtual ~PubSubMemberListener();

	virtual bool onNotification(const std::string& payload) override;

  private:
	DB* _db;
};

/**
 * Listener for SSO auth update notifications via GCP PubSub
 */
class PubSubSSOListener : public PubSubListener {
  public:
	PubSubSSOListener(
		std::string controller_id,
		std::string project,
		std::string topic,
		std::shared_ptr<ConnectionPool<PostgresConnection> > pool);
	virtual ~PubSubSSOListener();

	virtual bool onNotification(const std::string& payload) override;

  private:
	std::shared_ptr<ConnectionPool<PostgresConnection> > _pool;
};

}	// namespace ZeroTier

#endif	 // ZT_CONTROLLER_PUBSUBLISTENER_HPP
#endif	 // ZT_CONTROLLER_USE_LIBPQ