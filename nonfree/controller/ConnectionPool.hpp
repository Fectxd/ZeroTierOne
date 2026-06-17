/* (c) ZeroTier, Inc.
 * See LICENSE.txt in nonfree/
 */

#ifndef ZT_CONNECTION_POOL_H_
#define ZT_CONNECTION_POOL_H_

#ifndef _DEBUG
#define _DEBUG(x)
#endif

#include "../../node/Metrics.hpp"
#include "opentelemetry/trace/provider.h"

#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <set>

namespace ZeroTier {

struct ConnectionUnavailable : std::exception {
	char const* what() const throw()
	{
		return "Unable to allocate connection";
	};
};

class Connection {
  public:
	virtual ~Connection() {};

	/**
	 * @return false if this connection is known to be dead/unusable, so the pool
	 * can discard and replace it rather than handing it out again.  Defaults to
	 * true for connection types that don't implement a health check.
	 */
	virtual bool alive() const
	{
		return true;
	}
};

class ConnectionFactory {
  public:
	virtual ~ConnectionFactory() {};
	virtual std::shared_ptr<Connection> create() = 0;
};

struct ConnectionPoolStats {
	size_t pool_size;
	size_t borrowed_size;
};

template <class T> class ConnectionPool {
  public:
	ConnectionPool(size_t max_pool_size, size_t min_pool_size, std::shared_ptr<ConnectionFactory> factory) : m_maxPoolSize(max_pool_size), m_minPoolSize(min_pool_size), m_factory(factory)
	{
		Metrics::max_pool_size += max_pool_size;
		Metrics::min_pool_size += min_pool_size;
		while (m_pool.size() < m_minPoolSize) {
			m_pool.push_back(m_factory->create());
			Metrics::pool_avail++;
		}
	};

	ConnectionPoolStats get_stats()
	{
		std::unique_lock<std::mutex> lock(m_poolMutex);

		ConnectionPoolStats stats;
		stats.pool_size = m_pool.size();
		stats.borrowed_size = m_borrowed.size();

		return stats;
	};

	~ConnectionPool() {};

	/**
	 * Borrow
	 *
	 * Borrow a connection for temporary use
	 *
	 * When done, either (a) call unborrow() to return it, or (b) (if it's bad) just let it go out of scope.  This will cause it to automatically be replaced.
	 * @retval a shared_ptr to the connection object
	 */
	std::shared_ptr<T> borrow()
	{
		auto provider = opentelemetry::trace::Provider::GetTracerProvider();
		auto tracer = provider->GetTracer("connection_pool");
		auto span = tracer->StartSpan("connection_pool::borrow");
		auto scope = tracer->WithActiveSpan(span);

		std::unique_lock<std::mutex> l(m_poolMutex);

		// Discard any dead connections sitting idle in the pool so we never hand
		// one out.  This forces the size checks below to replenish with fresh
		// connections, which is what lets the pool recover after the DB server
		// drops every connection (e.g. an AlloyDB maintenance restart).
		for (auto it = m_pool.begin(); it != m_pool.end();) {
			if (! (*it)->alive()) {
				it = m_pool.erase(it);
				Metrics::pool_avail--;
			}
			else {
				++it;
			}
		}

		while ((m_pool.size() + m_borrowed.size()) < m_minPoolSize) {
			std::shared_ptr<Connection> conn = m_factory->create();
			m_pool.push_back(conn);
			Metrics::pool_avail++;
		}

		if (m_pool.size() == 0) {
			if ((m_pool.size() + m_borrowed.size()) < m_maxPoolSize) {
				try {
					std::shared_ptr<Connection> conn = m_factory->create();
					m_borrowed.insert(conn);
					Metrics::pool_in_use++;
					return std::static_pointer_cast<T>(conn);
				}
				catch (std::exception& e) {
					span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
					Metrics::pool_errors++;
					throw ConnectionUnavailable();
				}
			}
			else {
				for (auto it = m_borrowed.begin(); it != m_borrowed.end(); ++it) {
					if ((*it).unique()) {
						// This connection has been abandoned! Destroy it and create a new connection
						try {
							// If we are able to create a new connection, return it
							_DEBUG("Creating new connection to replace discarded connection");
							std::shared_ptr<Connection> conn = m_factory->create();
							m_borrowed.erase(it);
							m_borrowed.insert(conn);
							return std::static_pointer_cast<T>(conn);
						}
						catch (std::exception& e) {
							span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
							// Error creating a replacement connection
							Metrics::pool_errors++;
							throw ConnectionUnavailable();
						}
					}
				}

				span->SetStatus(opentelemetry::trace::StatusCode::kError, "No available connections in pool");
				// Nothing available
				Metrics::pool_errors++;
				throw ConnectionUnavailable();
			}
		}

		// Take one off the front
		std::shared_ptr<Connection> conn = m_pool.front();
		m_pool.pop_front();
		Metrics::pool_avail--;
		// Add it to the borrowed list
		m_borrowed.insert(conn);
		Metrics::pool_in_use++;
		return std::static_pointer_cast<T>(conn);
	};

	/**
	 * Unborrow a connection
	 *
	 * Only call this if you are returning a working connection.  If the connection was bad, just let it go out of scope (so the connection manager can replace it).
	 * @param the connection
	 */
	void unborrow(std::shared_ptr<T> conn)
	{
		auto provider = opentelemetry::trace::Provider::GetTracerProvider();
		auto tracer = provider->GetTracer("connection_pool");
		auto span = tracer->StartSpan("connection_pool::unborrow");
		auto scope = tracer->WithActiveSpan(span);

		// Lock
		std::unique_lock<std::mutex> lock(m_poolMutex);
		m_borrowed.erase(conn);
		Metrics::pool_in_use--;
		// Only return live connections to the pool.  A dead one (e.g. the caller
		// hit an error because the DB dropped it) is let go here so its slot is
		// replaced with a fresh connection on the next borrow().
		if (conn->alive() && (m_pool.size() + m_borrowed.size()) < m_maxPoolSize) {
			Metrics::pool_avail++;
			m_pool.push_back(conn);
		}
	};

  protected:
	size_t m_maxPoolSize;
	size_t m_minPoolSize;
	std::shared_ptr<ConnectionFactory> m_factory;
	std::deque<std::shared_ptr<Connection> > m_pool;
	std::set<std::shared_ptr<Connection> > m_borrowed;
	std::mutex m_poolMutex;
};

/**
 * RAII guard for a borrowed connection.
 *
 * Borrows from the pool on construction and returns it on destruction, so the
 * connection can never be leaked on an exception path (borrow() throws rather than
 * returning null, and unborrow() must run on every path or the pool leaks a slot).
 * Call unborrow() to return it early; the destructor is then a no-op.
 */
template <class T> class PooledConnection {
  public:
	explicit PooledConnection(const std::shared_ptr<ConnectionPool<T> >& pool) : m_pool(pool), m_conn(pool->borrow())
	{
	}

	~PooledConnection()
	{ unborrow(); }

	PooledConnection(const PooledConnection&) = delete;
	PooledConnection& operator=(const PooledConnection&) = delete;

	const std::shared_ptr<T>& get() const
	{ return m_conn; }

	T* operator->() const
	{ return m_conn.get(); }

	/**
	 * Return the connection to the pool now.  Idempotent.
	 */
	void unborrow()
	{
		if (m_conn) {
			m_pool->unborrow(m_conn);
			m_conn.reset();
		}
	}

  private:
	std::shared_ptr<ConnectionPool<T> > m_pool;
	std::shared_ptr<T> m_conn;
};

}	// namespace ZeroTier

#endif
