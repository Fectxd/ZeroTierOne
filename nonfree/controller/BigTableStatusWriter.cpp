#include "BigTableStatusWriter.hpp"

#include "ControllerConfig.hpp"
#include "PubSubWriter.hpp"

#include <google/cloud/bigtable/mutations.h>
#include <google/cloud/bigtable/row.h>
#include <google/cloud/bigtable/table.h>
#include <opentelemetry/trace/provider.h>

namespace cbt = google::cloud::bigtable;

namespace ZeroTier {

const std::string nodeInfoColumnFamily = "node_info";
const std::string checkInColumnFamily = "check_in";

const std::string osColumn = "os";
const std::string archColumn = "arch";
const std::string versionColumn = "version";
const std::string ipv4Column = "ipv4";
const std::string ipv6Column = "ipv6";
const std::string lastSeenColumn = "last_seen";

BigTableStatusWriter::BigTableStatusWriter(
	const std::string& project_id,
	const std::string& instance_id,
	const std::string& table_id)
	: _project_id(project_id)
	, _instance_id(instance_id)
	, _table_id(table_id)
	, _table(nullptr)
{
	_table = new cbt::Table(cbt::MakeDataConnection(), cbt::TableResource(_project_id, _instance_id, _table_id));
	fprintf(
		stderr, "BigTableStatusWriter for project %s instance %s table %s\n", project_id.c_str(), instance_id.c_str(),
		table_id.c_str());
}

BigTableStatusWriter::~BigTableStatusWriter()
{
	writePending();

	if (_table != nullptr) {
		delete _table;
		_table = nullptr;
	}
}

void BigTableStatusWriter::updateNodeStatus(
	const std::string& network_id,
	const std::string& node_id,
	const std::string& os,
	const std::string& arch,
	const std::string& version,
	const InetAddress& address,
	int64_t last_seen,
	const std::string& frontend)
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("BigTableStatusWriter");
	auto span = tracer->StartSpan("BigTableStatusWriter::updateNodeStatus");
	auto scope = tracer->WithActiveSpan(span);

	std::lock_guard<std::mutex> l(_lock);
	_pending.push_back({ network_id, node_id, os, arch, version, address, last_seen, frontend });
}

size_t BigTableStatusWriter::queueLength() const
{
	std::lock_guard<std::mutex> l(_lock);
	return _pending.size();
}

void BigTableStatusWriter::writePending()
{
	auto provider = opentelemetry::trace::Provider::GetTracerProvider();
	auto tracer = provider->GetTracer("BigTableStatusWriter");
	auto span = tracer->StartSpan("BigTableStatusWriter::writePending");
	auto scope = tracer->WithActiveSpan(span);

	std::vector<PendingStatusEntry> toWrite;
	{
		std::lock_guard<std::mutex> l(_lock);
		toWrite.swap(_pending);
	}
	if (toWrite.empty()) {
		return;
	}

	cbt::BulkMutation bulk;
	for (const auto& entry : toWrite) {
		std::string row_key = entry.network_id + "#" + entry.node_id;

		// read the latest values from BigTable for this row key
		std::map<std::string, std::string> latest_values;
		try {
			auto row = _table->ReadRow(row_key, cbt::Filter::Latest(1));
			if (row->first) {
				for (const auto& cell : row->second.cells()) {
					if (cell.family_name() == nodeInfoColumnFamily) {
						latest_values[cell.column_qualifier()] = cell.value();
					}
				}
			}
		}
		catch (const std::exception& e) {
			fprintf(stderr, "Exception reading from BigTable: %s\n", e.what());
		}

		cbt::SingleRowMutation m(row_key);

		// only update if value has changed
		if (latest_values[osColumn] != entry.os) {
			m.emplace_back(cbt::SetCell(nodeInfoColumnFamily, osColumn, entry.os));
		}
		if (latest_values[archColumn] != entry.arch) {
			m.emplace_back(cbt::SetCell(nodeInfoColumnFamily, archColumn, entry.arch));
		}
		if (latest_values[versionColumn] != entry.version) {
			m.emplace_back(cbt::SetCell(nodeInfoColumnFamily, versionColumn, entry.version));
		}

		char buf[64] = { 0 };
		std::string addressStr = entry.address.toString(buf);
		if (entry.address.ss_family == AF_INET) {
			m.emplace_back(cbt::SetCell(checkInColumnFamily, ipv4Column, std::move(addressStr)));
		}
		else if (entry.address.ss_family == AF_INET6) {
			m.emplace_back(cbt::SetCell(checkInColumnFamily, ipv6Column, std::move(addressStr)));
		}
		int64_t ts = entry.last_seen;
		m.emplace_back(cbt::SetCell(checkInColumnFamily, lastSeenColumn, std::move(ts)));
		bulk.emplace_back(m);
	}

	fprintf(stderr, "Applying %zu mutations to BigTable\n", bulk.size());

	try {
		std::vector<cbt::FailedMutation> failures = _table->BulkApply(std::move(bulk));
		fprintf(stderr, "BigTable write completed with %zu failures\n", failures.size());
		for (auto const& r : failures) {
			// Handle error (log it, retry, etc.)
			std::cerr << "Error writing to BigTable: " << r.status() << "\n";
		}
	}
	catch (const std::exception& e) {
		fprintf(stderr, "Exception writing to BigTable: %s\n", e.what());
		span->SetAttribute("error", e.what());
		span->SetStatus(opentelemetry::trace::StatusCode::kError, e.what());
		return;
	}
}

}	// namespace ZeroTier