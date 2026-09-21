/*
 * libtorrent_peer_collector_qt.cpp
 *
 * Native C++ / Qt Widgets GUI application for collecting peers seen by a
 * real libtorrent-rasterbar session.
 *
 * Based on the Python logic from:
 *   libtorrent_seen_peer_collector_v2_nodeprecated.py
 *
 * It uses modern libtorrent settings through settings_pack/apply settings style:
 *   - enable_dht
 *   - enable_lsd
 *   - enable_upnp
 *   - enable_natpmp
 *   - listen_interfaces
 *   - dht_bootstrap_nodes
 *
 * It collects peers from:
 *   - trackers
 *   - DHT
 *   - PEX, after connecting to peers
 *   - LSD/local discovery
 *   - connected peers
 *
 * Build with CMake:
 *   cp CMakeLists_libtorrent_peer_collector_qt.txt CMakeLists.txt
 *   cmake -S . -B build
 *   cmake --build build
 *
 * Build with qmake:
 *   qmake6 libtorrent_peer_collector_qt.pro
 *   make
 */

#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QUrlQuery>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <QtGui/QClipboard>
#include <QtGui/QFont>
#include <QtGui/QCloseEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <QtCore/QEventLoop>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lt = libtorrent;

/* ------------------------------------------------------------------------- */
/* Utility helpers                                                            */
/* ------------------------------------------------------------------------- */

static QString ipPortToText(const lt::tcp::endpoint &ep)
{
	auto address = ep.address();

	if (!address.is_v4()) {
		return QString();
	}

	/*
	 * Some Boost/ASIO versions provide address.to_string(error_code),
	 * but Kubuntu 26.04 / Boost here provides only address.to_string().
	 */
	std::string ip;

	try {
		ip = address.to_string();
	} catch (...) {
		return QString();
	}

	return QString::fromStdString(ip) + ":" + QString::number(ep.port());
}

static bool ipv4PortToSortKey(const QString &peer, quint32 *ip_out, quint16 *port_out)
{
	QStringList parts;
	QString host;
	QString port_text;
	bool ok;
	quint32 a;
	quint32 b;
	quint32 c;
	quint32 d;
	quint32 port;

	int colon = peer.lastIndexOf(':');
	if (colon <= 0) {
		return false;
	}

	host = peer.left(colon);
	port_text = peer.mid(colon + 1);

	parts = host.split('.');
	if (parts.size() != 4) {
		return false;
	}

	a = parts[0].toUInt(&ok);
	if (!ok || a > 255) return false;

	b = parts[1].toUInt(&ok);
	if (!ok || b > 255) return false;

	c = parts[2].toUInt(&ok);
	if (!ok || c > 255) return false;

	d = parts[3].toUInt(&ok);
	if (!ok || d > 255) return false;

	port = port_text.toUInt(&ok);
	if (!ok || port > 65535) return false;

	if (ip_out != nullptr) {
		*ip_out = (a << 24) | (b << 16) | (c << 8) | d;
	}

	if (port_out != nullptr) {
		*port_out = (quint16)port;
	}

	return true;
}


static QStringList extractIpv4PortsFromText(const QString &text)
{
	QStringList out;

	/*
	 * Collect peer endpoints from alert text.
	 * This catches endpoints that may appear in connect/disconnect/error alerts,
	 * including peers that were seen but did not remain in get_peer_info().
	 */
	QRegularExpression re(
	    "\\b((?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\\."
	    "(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\\."
	    "(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\\."
	    "(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])):(\\d{1,5})\\b");

	QRegularExpressionMatchIterator it = re.globalMatch(text);

	while (it.hasNext()) {
		QRegularExpressionMatch m = it.next();
		QString ip = m.captured(1);
		QString port_text = m.captured(2);
		bool ok = false;
		uint port = port_text.toUInt(&ok);

		if (ok && port <= 65535) {
			out << ip + ":" + QString::number(port);
		}
	}

	out.removeDuplicates();
	return out;
}

static QStringList sortedPeerList(const std::set<QString> &peers)
{
	QStringList list;

	for (const QString &p : peers) {
		list << p;
	}

	std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
		quint32 aip = 0;
		quint32 bip = 0;
		quint16 aport = 0;
		quint16 bport = 0;

		if (!ipv4PortToSortKey(a, &aip, &aport)) {
			return a < b;
		}

		if (!ipv4PortToSortKey(b, &bip, &bport)) {
			return a < b;
		}

		if (aip != bip) {
			return aip < bip;
		}

		return aport < bport;
	});

	return list;
}

static QString sourceSetToText(const std::set<QString> &sources)
{
	QStringList list;

	for (const QString &s : sources) {
		list << s;
	}

	list.sort();

	return list.join(", ");
}

static QString formatPeerLineWithComment(const QString &peer,
                                             const QString &comment,
                                             int comment_column)
{
	/*
	 * Automatic comment alignment.
	 *
	 * comment_column is calculated from the current peer list:
	 *
	 *   comment_column = max_length(IP:port) + 5
	 *   spaces         = comment_column - length(current IP:port)
	 *
	 * This makes every # start at the same character position.
	 */
	int peer_len = peer.length();
	int spaces = comment_column - peer_len;

	if (spaces < 2) {
		spaces = 2;
	}

	return peer + QString(spaces, QLatin1Char(' ')) + "# " + comment;
}

static int calculatePeerCommentColumn(const QStringList &sorted_peers)
{
	int max_peer_len = 0;

	for (const QString &p : sorted_peers) {
		if (p.length() > max_peer_len) {
			max_peer_len = p.length();
		}
	}

	return max_peer_len + 5;
}

static QString peerIpOnly(const QString &peer)
{
	int colon = peer.indexOf(':');

	if (colon <= 0) {
		return peer.trimmed();
	}

	return peer.left(colon).trimmed();
}

static QString buildPeerComment(const QString &peer,
                                const std::map<QString, std::set<QString>> &peer_sources,
                                const std::map<QString, QString> &ip_info_cache,
                                bool include_source_comments,
                                bool include_ipinfo_comments)
{
	QStringList parts;

	if (include_source_comments) {
		auto it = peer_sources.find(peer);

		if (it != peer_sources.end() && !it->second.empty()) {
			parts << sourceSetToText(it->second);
		} else {
			parts << "seen";
		}
	}

	if (include_ipinfo_comments) {
		QString ip = peerIpOnly(peer);
		auto it = ip_info_cache.find(ip);

		if (it != ip_info_cache.end() && !it->second.isEmpty()) {
			parts << it->second;
		} else {
			parts << "ipinfo=pending";
		}
	}

	return parts.join("; ");
}

static QStringList buildPeerOutputLines(const std::set<QString> &peers,
                                        const std::map<QString, std::set<QString>> &peer_sources,
                                        const std::map<QString, QString> &ip_info_cache,
                                        bool include_source_comments,
                                        bool include_ipinfo_comments)
{
	QStringList out;
	QStringList list = sortedPeerList(peers);
	int comment_column = calculatePeerCommentColumn(list);
	bool include_any_comment = include_source_comments || include_ipinfo_comments;

	for (const QString &p : list) {
		if (include_any_comment) {
			QString comment = buildPeerComment(
			    p,
			    peer_sources,
			    ip_info_cache,
			    include_source_comments,
			    include_ipinfo_comments);

			out << formatPeerLineWithComment(p, comment, comment_column);
		} else {
			out << p;
		}
	}

	return out;
}


static int countPeersWithSource(const std::map<QString, std::set<QString>> &peer_sources,
                                const QString &source)
{
	int count = 0;

	for (const auto &item : peer_sources) {
		if (item.second.find(source) != item.second.end()) {
			count++;
		}
	}

	return count;
}

static int countPeersWithSourcePrefix(const std::map<QString, std::set<QString>> &peer_sources,
                                      const QString &prefix)
{
	int count = 0;

	for (const auto &item : peer_sources) {
		bool matched = false;

		for (const QString &source : item.second) {
			if (source.startsWith(prefix)) {
				matched = true;
				break;
			}
		}

		if (matched) {
			count++;
		}
	}

	return count;
}

static int countPeersWithoutKnownSource(const std::set<QString> &peers,
                                        const std::map<QString, std::set<QString>> &peer_sources)
{
	int count = 0;

	for (const QString &peer : peers) {
		auto it = peer_sources.find(peer);

		if (it == peer_sources.end() || it->second.empty()) {
			count++;
		}
	}

	return count;
}

static QString buildPeerSummaryText(const std::set<QString> &peers,
                                    const std::map<QString, std::set<QString>> &peer_sources)
{
	int connected = countPeersWithSource(peer_sources, "connected/get_peer_info");
	int tracker_direct = countPeersWithSource(peer_sources, "tracker-direct-http");
	int tracker_alert = countPeersWithSource(peer_sources, "tracker-alert");
	int dht_alert = countPeersWithSource(peer_sources, "dht-alert");
	int pex_alert = countPeersWithSource(peer_sources, "pex-alert");
	int peer_alert = countPeersWithSource(peer_sources, "peer-alert");
	int any_alert = countPeersWithSourcePrefix(peer_sources, "");
	int unknown = countPeersWithoutKnownSource(peers, peer_sources);

	/*
	 * any_alert above counts all sources, not only alert sources. Recalculate alert-only below.
	 */
	any_alert = 0;
	for (const auto &item : peer_sources) {
		bool matched = false;

		for (const QString &source : item.second) {
			if (source.endsWith("-alert") || source == "alert") {
				matched = true;
				break;
			}
		}

		if (matched) {
			any_alert++;
		}
	}

	return QString(
	    "Peer summary:\n"
	    "  Total unique peers       : %1\n"
	    "  Connected/get_peer_info  : %2\n"
	    "  Direct HTTP tracker      : %3\n"
	    "  Tracker alert            : %4\n"
	    "  DHT alert                : %5\n"
	    "  PEX alert                : %6\n"
	    "  Other peer alert         : %7\n"
	    "  Any alert source         : %8\n"
	    "  Unknown/no source tag    : %9\n")
	    .arg(peers.size())
	    .arg(connected)
	    .arg(tracker_direct)
	    .arg(tracker_alert)
	    .arg(dht_alert)
	    .arg(pex_alert)
	    .arg(peer_alert)
	    .arg(any_alert)
	    .arg(unknown);
}

static bool savePeersToFile(const QString &path,
                            const std::set<QString> &peers,
                            const std::map<QString, std::set<QString>> &peer_sources,
                            const std::map<QString, QString> &ip_info_cache,
                            bool include_source_comments,
                            bool include_ipinfo_comments,
                            QString *error)
{
	QFileInfo info(path);
	QDir dir = info.absoluteDir();

	if (!dir.exists()) {
		if (!dir.mkpath(".")) {
			if (error != nullptr) {
				*error = "Could not create directory: " + dir.absolutePath();
			}
			return false;
		}
	}

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
		if (error != nullptr) {
			*error = f.errorString();
		}
		return false;
	}

	QTextStream out(&f);
	QStringList list = buildPeerOutputLines(
	    peers,
	    peer_sources,
	    ip_info_cache,
	    include_source_comments,
	    include_ipinfo_comments);

	for (const QString &p : list) {
		out << p << "\n";
	}

	return true;
}


/* ------------------------------------------------------------------------- */
/* Direct HTTP/HTTPS tracker announce helpers                                 */
/* ------------------------------------------------------------------------- */

static QByteArray urlEncodeBytes(const QByteArray &input)
{
	QByteArray out;
	const char hex[] = "0123456789ABCDEF";

	for (unsigned char c : input) {
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') {
			out.append((char)c);
		} else {
			out.append('%');
			out.append(hex[(c >> 4) & 0x0f]);
			out.append(hex[c & 0x0f]);
		}
	}

	return out;
}

static QByteArray makePeerId()
{
	QByteArray peer_id("-QTPC01-");

	while (peer_id.size() < 20) {
		quint32 r = QRandomGenerator::global()->generate();
		QByteArray part = QByteArray::number(r, 16);
		peer_id.append(part);
	}

	peer_id.truncate(20);
	return peer_id;
}

static QByteArray sha1HashToBytes(const lt::sha1_hash &h)
{
	QByteArray out;

	for (int i = 0; i < 20; i++) {
		out.append((char)h[i]);
	}

	return out;
}

static bool parseBencodedByteString(const QByteArray &data,
                                    int *pos,
                                    QByteArray *value)
{
	int p;
	int len;

	if (pos == nullptr || value == nullptr) {
		return false;
	}

	p = *pos;
	if (p < 0 || p >= data.size() || data[p] < '0' || data[p] > '9') {
		return false;
	}

	len = 0;

	while (p < data.size() && data[p] >= '0' && data[p] <= '9') {
		len = len * 10 + (data[p] - '0');
		p++;
	}

	if (p >= data.size() || data[p] != ':') {
		return false;
	}

	p++;

	if (len < 0 || p + len > data.size()) {
		return false;
	}

	*value = data.mid(p, len);
	*pos = p + len;

	return true;
}

static QStringList parseCompactIpv4Peers(const QByteArray &compact)
{
	QStringList peers;

	for (int i = 0; i + 5 < compact.size(); i += 6) {
		unsigned char a = (unsigned char)compact[i + 0];
		unsigned char b = (unsigned char)compact[i + 1];
		unsigned char c = (unsigned char)compact[i + 2];
		unsigned char d = (unsigned char)compact[i + 3];
		unsigned int port =
		    ((unsigned char)compact[i + 4] << 8) |
		    ((unsigned char)compact[i + 5]);

		if (port == 0) {
			continue;
		}

		peers << QString("%1.%2.%3.%4:%5")
		             .arg((unsigned)a)
		             .arg((unsigned)b)
		             .arg((unsigned)c)
		             .arg((unsigned)d)
		             .arg(port);
	}

	peers.removeDuplicates();
	return peers;
}

static QString trackerFailureReason(const QByteArray &data)
{
	int key = data.indexOf("14:failure reason");
	int pos;
	QByteArray value;

	if (key < 0) {
		return QString();
	}

	pos = key + (int)strlen("14:failure reason");

	if (parseBencodedByteString(data, &pos, &value)) {
		return QString::fromUtf8(value);
	}

	return QString();
}

static QStringList parseTrackerResponsePeers(const QByteArray &data)
{
	QStringList peers;
	int key;
	int pos;
	QByteArray value;

	/*
	 * Most trackers return compact IPv4 peers as:
	 *   d...5:peers<length>:<6-byte records>...e
	 *
	 * Each record is:
	 *   4 bytes IPv4 + 2 bytes big-endian port
	 */
	key = data.indexOf("5:peers");
	if (key < 0) {
		return peers;
	}

	pos = key + 7;

	if (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
		if (parseBencodedByteString(data, &pos, &value)) {
			peers = parseCompactIpv4Peers(value);
		}
	}

	return peers;
}

static QByteArray buildTrackerAnnounceUrl(const QString &tracker_url,
                                          const lt::torrent_info &ti,
                                          int listen_port,
                                          const QByteArray &peer_id)
{
	QByteArray url = tracker_url.toUtf8();
	QByteArray info_hash_bytes = sha1HashToBytes(ti.info_hash());
	qint64 left = 0;

	try {
		left = (qint64)ti.total_size();
	} catch (...) {
		left = 0;
	}

	url += (url.contains('?') ? '&' : '?');
	url += "info_hash=" + urlEncodeBytes(info_hash_bytes);
	url += "&peer_id=" + urlEncodeBytes(peer_id);
	url += "&port=" + QByteArray::number(listen_port);
	url += "&uploaded=0";
	url += "&downloaded=0";
	url += "&left=" + QByteArray::number(left);
	url += "&compact=1";
	url += "&numwant=200";
	url += "&event=started";

	return url;
}

static QStringList directHttpTrackerAnnounce(const QString &tracker_url,
                                             const lt::torrent_info &ti,
                                             int listen_port,
                                             QStringList *log_lines)
{
	QStringList peers;
	QUrl parsed(tracker_url);

	if (parsed.scheme() != "http" && parsed.scheme() != "https") {
		if (log_lines != nullptr) {
			*log_lines << "Direct tracker announce skipped non-HTTP tracker: " + tracker_url;
		}
		return peers;
	}

	QByteArray peer_id = makePeerId();
	QByteArray full_url = buildTrackerAnnounceUrl(tracker_url, ti, listen_port, peer_id);

	QNetworkAccessManager manager;
	QNetworkRequest request(QUrl::fromEncoded(full_url));
	QNetworkReply *reply = manager.get(request);

	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);

	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

	timeout.start(15000);
	loop.exec();

	if (timeout.isActive()) {
		timeout.stop();
	} else {
		reply->abort();
		reply->deleteLater();
		if (log_lines != nullptr) {
			*log_lines << "Direct tracker announce timeout: " + tracker_url;
		}
		return peers;
	}

	QByteArray data = reply->readAll();
	QNetworkReply::NetworkError net_error = reply->error();
	QString error_string = reply->errorString();
	reply->deleteLater();

	if (net_error != QNetworkReply::NoError) {
		if (log_lines != nullptr) {
			*log_lines << "Direct tracker announce error: " + tracker_url + " : " + error_string;
		}
		return peers;
	}

	QString failure = trackerFailureReason(data);
	if (!failure.isEmpty()) {
		if (log_lines != nullptr) {
			*log_lines << "Tracker failure: " + tracker_url + " : " + failure;
		}
		return peers;
	}

	peers = parseTrackerResponsePeers(data);

	if (log_lines != nullptr) {
		*log_lines << QString("Direct tracker announce: %1 -> %2 compact IPv4 peers")
		                  .arg(tracker_url)
		                  .arg(peers.size());
	}

	return peers;
}

static int directAnnounceAllHttpTrackers(const std::shared_ptr<lt::torrent_info> &ti,
                                         int listen_port,
                                         std::set<QString> &peers,
                                         std::map<QString, std::set<QString>> &peer_sources,
                                         QStringList *log_lines)
{
	int added = 0;

	if (!ti) {
		return 0;
	}

	std::vector<lt::announce_entry> trackers = ti->trackers();

	for (const lt::announce_entry &ae : trackers) {
		QString tracker_url = QString::fromStdString(ae.url);
		QStringList found = directHttpTrackerAnnounce(tracker_url, *ti, listen_port, log_lines);

		for (const QString &p : found) {
			if (peers.insert(p).second) {
				added++;
			}

			peer_sources[p].insert("tracker-direct-http");
		}
	}

	return added;
}



/* ------------------------------------------------------------------------- */
/* IPinfo lookup helpers                                                      */
/* ------------------------------------------------------------------------- */

static QString jsonString(const QJsonObject &obj, const QString &key)
{
	QJsonValue v = obj.value(key);

	if (v.isString()) {
		return v.toString().trimmed();
	}

	if (v.isDouble()) {
		return QString::number(v.toDouble(), 'f', 6).remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"));
	}

	return QString();
}

static QString parseIpInfoJson(const QByteArray &data, QString *error)
{
	QJsonParseError parse_error;
	QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);

	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		if (error != nullptr) {
			*error = "bad JSON";
		}
		return QString();
	}

	QJsonObject obj = doc.object();

	/* Legacy/Core style fields */
	QString country = jsonString(obj, "country");
	QString city = jsonString(obj, "city");
	QString region = jsonString(obj, "region");
	QString org = jsonString(obj, "org");
	QString hostname = jsonString(obj, "hostname");

	/* Lite/newer style fields */
	QString country_name = jsonString(obj, "country_name");
	QString country_code = jsonString(obj, "country_code");

	if (country.isEmpty()) {
		country = country_code;
	}
	if (!country_name.isEmpty()) {
		if (!country.isEmpty() && country != country_name) {
			country = country + "/" + country_name;
		} else {
			country = country_name;
		}
	}

	/* ASN can be object in newer plans/Lite. */
	QString asn;
	QString asn_name;
	QJsonValue asn_value = obj.value("asn");

	if (asn_value.isObject()) {
		QJsonObject asn_obj = asn_value.toObject();
		asn = jsonString(asn_obj, "asn");
		asn_name = jsonString(asn_obj, "name");
		if (asn_name.isEmpty()) {
			asn_name = jsonString(asn_obj, "domain");
		}
	} else if (asn_value.isString()) {
		asn = asn_value.toString().trimmed();
	}

	/* Company object can also exist on some IPinfo plans. */
	QString company_name;
	QJsonValue company_value = obj.value("company");
	if (company_value.isObject()) {
		QJsonObject company_obj = company_value.toObject();
		company_name = jsonString(company_obj, "name");
	}

	if (org.isEmpty()) {
		if (!asn.isEmpty() && !asn_name.isEmpty()) {
			org = asn + " " + asn_name;
		} else if (!asn_name.isEmpty()) {
			org = asn_name;
		} else if (!company_name.isEmpty()) {
			org = company_name;
		} else if (!asn.isEmpty()) {
			org = asn;
		}
	}

	QStringList parts;

	if (!country.isEmpty()) {
		parts << "country=" + country;
	}

	if (!city.isEmpty()) {
		if (!region.isEmpty() && region != city) {
			parts << "city=" + city + "/" + region;
		} else {
			parts << "city=" + city;
		}
	} else if (!region.isEmpty()) {
		parts << "region=" + region;
	}

	if (!org.isEmpty()) {
		parts << "provider=" + org;
	}

	if (!hostname.isEmpty()) {
		parts << "host=" + hostname;
	}

	if (parts.isEmpty()) {
		if (error != nullptr) {
			*error = "no usable fields";
		}
		return QString();
	}

	return "ipinfo: " + parts.join(", ");
}

static QString fetchIpInfoText(const QString &ip,
                               const QString &token,
                               QString *error)
{
	if (ip.isEmpty()) {
		if (error != nullptr) {
			*error = "empty IP";
		}
		return QString();
	}

	QUrl url("https://ipinfo.io/" + ip + "/json");

	if (!token.trimmed().isEmpty()) {
		QUrlQuery query;
		query.addQueryItem("token", token.trimmed());
		url.setQuery(query);
	}

	QNetworkAccessManager manager;
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, "libtorrent-peer-collector-qt/1.0");

	QNetworkReply *reply = manager.get(request);
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);

	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

	timeout.start(4000);
	loop.exec();

	if (timeout.isActive()) {
		timeout.stop();
	} else {
		reply->abort();
		reply->deleteLater();
		if (error != nullptr) {
			*error = "timeout";
		}
		return QString();
	}

	QByteArray data = reply->readAll();
	QNetworkReply::NetworkError net_error = reply->error();
	QString error_string = reply->errorString();
	reply->deleteLater();

	if (net_error != QNetworkReply::NoError) {
		if (error != nullptr) {
			*error = error_string;
		}
		return QString();
	}

	QString parse_error;
	QString info = parseIpInfoJson(data, &parse_error);

	if (info.isEmpty()) {
		if (error != nullptr) {
			*error = parse_error.isEmpty() ? "empty response" : parse_error;
		}
		return QString();
	}

	return info;
}

static int ensureIpInfoForPeers(const std::set<QString> &peers,
                                std::map<QString, QString> &ip_info_cache,
                                const QString &token,
                                int max_new_lookups,
                                QStringList *log_lines)
{
	int lookups = 0;

	if (max_new_lookups <= 0) {
		return 0;
	}

	for (const QString &peer : peers) {
		QString ip = peerIpOnly(peer);

		if (ip.isEmpty()) {
			continue;
		}

		if (ip_info_cache.find(ip) != ip_info_cache.end()) {
			continue;
		}

		QString error;
		QString info = fetchIpInfoText(ip, token, &error);

		if (info.isEmpty()) {
			info = "ipinfo=unavailable";
			if (log_lines != nullptr) {
				*log_lines << "IPinfo lookup failed for " + ip + ": " + error;
			}
		} else {
			if (log_lines != nullptr) {
				*log_lines << "IPinfo lookup: " + ip + " -> " + info;
			}
		}

		ip_info_cache[ip] = info;
		lookups++;

		if (lookups >= max_new_lookups) {
			break;
		}
	}

	return lookups;
}


/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

class PeerCollectorThread : public QThread {
	Q_OBJECT

public:
	PeerCollectorThread(const QString &torrent_path,
	                    const QString &output_path,
	                    int run_time_seconds,
	                    double peer_poll_interval_seconds,
	                    double tracker_reannounce_interval_seconds,
	                    double dht_reannounce_interval_seconds,
	                    int listen_port,
	                    bool no_download,
	                    bool enable_trackers,
	                    bool enable_dht,
	                    bool enable_pex,
	                    bool enable_lsd,
	                    bool include_source_comments,
	                    bool include_ipinfo_comments,
	                    const QString &ipinfo_token,
	                    QObject *parent = nullptr)
	    : QThread(parent),
	      torrentPath(torrent_path),
	      outputPath(output_path),
	      runTimeSeconds(run_time_seconds),
	      pollIntervalMs((int)(peer_poll_interval_seconds * 1000.0)),
	      trackerReannounceMs((int)(tracker_reannounce_interval_seconds * 1000.0)),
	      dhtReannounceMs((int)(dht_reannounce_interval_seconds * 1000.0)),
	      listenPort(listen_port),
	      noDownload(no_download),
	      enableTrackers(enable_trackers),
	      enableDht(enable_dht),
	      enablePex(enable_pex),
	      enableLsd(enable_lsd),
	      includeSourceComments(include_source_comments),
	      includeIpInfoComments(include_ipinfo_comments),
	      ipInfoToken(ipinfo_token),
	      stopRequested(false)
	{
		if (pollIntervalMs < 200) {
			pollIntervalMs = 200;
		}

		/*
		 * Do not force trackers too often. Many trackers rate-limit announces.
		 * DHT can be refreshed more often, but still should not be spammed.
		 */
		if (trackerReannounceMs < 30000) {
			trackerReannounceMs = 30000;
		}

		if (dhtReannounceMs < 30000) {
			dhtReannounceMs = 30000;
		}
	}

	void requestStop()
	{
		stopRequested.store(true);
	}

signals:
	void logMessage(const QString &text);
	void peerCountChanged(int count);
	void peersPreviewChanged(const QString &text);
	void progressChanged(int elapsed, int total);
	void finishedStatus(bool ok, const QString &message);

protected:
	void run() override
	{
		std::set<QString> peers;
		std::map<QString, std::set<QString>> peerSources;
		std::map<QString, QString> ipInfoCache;
		QString save_error;

		try {
			if (!QFileInfo::exists(torrentPath)) {
				emit finishedStatus(false, "Torrent file does not exist: " + torrentPath);
				return;
			}

			QTemporaryDir tempDir;
			if (!tempDir.isValid()) {
				emit finishedStatus(false, "Could not create temporary save directory.");
				return;
			}

			emit logMessage("Torrent     : " + torrentPath);
			emit logMessage("Output      : " + outputPath);
			emit logMessage("Save path   : " + tempDir.path());
			emit logMessage("Run time    : " + QString::number(runTimeSeconds) + " seconds");
			emit logMessage("Peer poll   : " + QString::number(pollIntervalMs / 1000.0, 'f', 1) + " seconds");
			emit logMessage("Tracker int.: " + QString::number(trackerReannounceMs / 1000.0, 'f', 0) + " seconds");
			emit logMessage("DHT int.    : " + QString::number(dhtReannounceMs / 1000.0, 'f', 0) + " seconds");
			emit logMessage("Listen port : " + QString::number(listenPort));
			emit logMessage("No download : " + QString(noDownload ? "true" : "false"));
			emit logMessage("Trackers    : " + QString(enableTrackers ? "enabled" : "disabled"));
			emit logMessage("DHT         : " + QString(enableDht ? "enabled" : "disabled"));
			emit logMessage("PEX         : " + QString(enablePex ? "enabled/requested, requires connected peer" : "disabled/requested"));
			emit logMessage("LSD         : " + QString(enableLsd ? "enabled" : "disabled"));
			emit logMessage("Source tags : " + QString(includeSourceComments ? "enabled" : "disabled"));
			emit logMessage("IPinfo      : " + QString(includeIpInfoComments ? "enabled" : "disabled"));
			emit logMessage("Note        : This libtorrent build may not support direct PEX on/off setting.");
			emit logMessage("");

			lt::settings_pack pack;
			pack.set_bool(lt::settings_pack::enable_dht, enableDht);
			pack.set_bool(lt::settings_pack::enable_lsd, enableLsd);
			pack.set_bool(lt::settings_pack::enable_upnp, true);
			pack.set_bool(lt::settings_pack::enable_natpmp, true);

			/*
			 * Alert mask.
			 *
			 * libtorrent 2.0 defaults alert_mask to error only. The alert
			 * scraping below needs the peer category to see endpoints of peers
			 * that connect/disconnect between polls or fail to connect at all.
			 * tracker/dht/status are for the log; their messages carry counts
			 * and URLs, not peer endpoints.
			 */
			pack.set_int(
			    lt::settings_pack::alert_mask,
			    static_cast<int>(
			        lt::alert_category::error |
			        lt::alert_category::peer |
			        lt::alert_category::tracker |
			        lt::alert_category::dht |
			        lt::alert_category::status));

			/*
			 * PEX compatibility note:
			 * Some libtorrent versions/builds do not expose settings_pack::enable_pex.
			 * In those builds PEX is controlled internally by libtorrent extensions and
			 * cannot be toggled through settings_pack. The GUI checkbox is kept so the
			 * selected mode is visible/logged, but this build may leave PEX at libtorrent's
			 * default behavior.
			 */

			pack.set_str(
			    lt::settings_pack::listen_interfaces,
			    QString("0.0.0.0:%1").arg(listenPort).toStdString());

			pack.set_str(
			    lt::settings_pack::dht_bootstrap_nodes,
			    "router.bittorrent.com:6881,"
			    "router.utorrent.com:6881,"
			    "dht.transmissionbt.com:6881");

#ifdef LIBTORRENT_VERSION_NUM
			emit logMessage("libtorrent  : " + QString::fromLatin1(LIBTORRENT_VERSION));
#endif

			lt::session ses(pack);

			auto ti = std::make_shared<lt::torrent_info>(torrentPath.toStdString());

			lt::add_torrent_params params;
			params.ti = ti;
			params.save_path = tempDir.path().toStdString();
			params.storage_mode = lt::storage_mode_t::storage_mode_sparse;

			lt::torrent_handle h = ses.add_torrent(params);

			try {
				h.resume();
			} catch (...) {
			}

			if (!enableTrackers) {
				try {
					std::vector<lt::announce_entry> empty_trackers;
					h.replace_trackers(empty_trackers);
					emit logMessage("Trackers disabled: tracker list cleared for this torrent.");
				} catch (...) {
					emit logMessage("Warning: could not clear tracker list. This libtorrent build may still announce to trackers.");
				}
			}

			if (noDownload) {
				try {
					std::vector<lt::download_priority_t> file_prios(
					    (std::size_t)ti->num_files(),
					    lt::dont_download);
					h.prioritize_files(file_prios);
				} catch (...) {
					emit logMessage("Could not set file priorities to dont_download.");
				}

				try {
					std::vector<lt::download_priority_t> piece_prios(
					    (std::size_t)ti->num_pieces(),
					    lt::dont_download);
					h.prioritize_pieces(piece_prios);
				} catch (...) {
					emit logMessage("Could not set piece priorities to dont_download.");
				}
			}

			if (enableTrackers) {
				try {
					h.force_reannounce();
				} catch (...) {
				}
			}

			if (enableDht) {
				try {
					h.force_dht_announce();
				} catch (...) {
				}
			}

			if (enableTrackers) {
				QStringList direct_logs;
				int added_direct = directAnnounceAllHttpTrackers(
				    ti,
				    listenPort,
				    peers,
				    peerSources,
				    &direct_logs);

				for (const QString &line : direct_logs) {
					emit logMessage(line);
				}

				if (added_direct > 0) {
					emit logMessage("Direct HTTP tracker added peers: " + QString::number(added_direct));
					emit peerCountChanged((int)peers.size());
					emit peersPreviewChanged(buildPeerOutputLines(peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments).join("\n") + (peers.empty() ? "" : "\n"));
				}
			}

			emit logMessage("Waiting for peers...");
			emit logMessage("Peer list includes connected peers, IPv4:port endpoints found in libtorrent alerts,");
			emit logMessage("and direct compact IPv4 peers returned by HTTP/HTTPS trackers.");
			emit logMessage("Output comments auto-align: # column = longest IP:port length + 5 spaces.");
			if (includeIpInfoComments) {
				emit logMessage("IPinfo lookups are cached. Same IP is looked up only once.");
			}
			emit logMessage("UDP trackers are not directly decoded in this build yet.");
			emit logMessage("");

			QElapsedTimer timer;
			timer.start();

			qint64 last_save_ms = -100000;
			qint64 last_preview_ms = -100000;
			qint64 last_tracker_reannounce_ms = 0;
			qint64 last_dht_reannounce_ms = 0;
			int last_count = 0;
			bool first_poll = true;

			while (!stopRequested.load()) {
				int elapsed = (int)(timer.elapsed() / 1000);

				if (elapsed >= runTimeSeconds) {
					break;
				}

				emit progressChanged(elapsed, runTimeSeconds);

				/*
				 * First cycle must poll immediately.
				 * After that, tracker/DHT announces and peer polling obey their intervals.
				 */
				if (!first_poll) {
					msleep((unsigned long)pollIntervalMs);
				}

				elapsed = (int)(timer.elapsed() / 1000);
				if (elapsed >= runTimeSeconds) {
					break;
				}

				emit progressChanged(elapsed, runTimeSeconds);

				if (first_poll) {
					emit logMessage("Immediate first peer poll...");
				}

				if (enableTrackers &&
				    (first_poll || timer.elapsed() - last_tracker_reannounce_ms >= trackerReannounceMs)) {
					try {
						h.force_reannounce();
						emit logMessage(first_poll ? "Initial tracker reannounce." : "Forced tracker reannounce.");
					} catch (...) {
					}

					QStringList direct_logs;
					int added_direct = directAnnounceAllHttpTrackers(
					    ti,
					    listenPort,
					    peers,
					    peerSources,
					    &direct_logs);

					for (const QString &line : direct_logs) {
						emit logMessage(line);
					}

					if (added_direct > 0) {
						emit logMessage("Direct HTTP tracker added peers: " + QString::number(added_direct));
					}

					last_tracker_reannounce_ms = timer.elapsed();
				}

				if (enableDht &&
				    (first_poll || timer.elapsed() - last_dht_reannounce_ms >= dhtReannounceMs)) {
					try {
						h.force_dht_announce();
						emit logMessage(first_poll ? "Initial DHT announce." : "Forced DHT announce.");
					} catch (...) {
					}

					last_dht_reannounce_ms = timer.elapsed();
				}

				try {
					std::vector<lt::alert *> alerts;
					ses.pop_alerts(&alerts);

					for (lt::alert *a : alerts) {
						QString s = QString::fromStdString(a->message());
						QString lower = s.toLower();

						QString source = "alert";
						if (lower.contains("tracker")) {
							source = "tracker-alert";
						} else if (lower.contains("dht")) {
							source = "dht-alert";
						} else if (lower.contains("pex")) {
							source = "pex-alert";
						} else if (lower.contains("peer")) {
							source = "peer-alert";
						}

						QStringList alert_peers = extractIpv4PortsFromText(s);
						for (const QString &p : alert_peers) {
							peers.insert(p);
							peerSources[p].insert(source);
						}

						if (lower.contains("tracker") ||
						    lower.contains("error") ||
						    lower.contains("dht") ||
						    lower.contains("listen") ||
						    lower.contains("peer")) {
							emit logMessage(s);
						}
					}
				} catch (...) {
				}

				try {
					std::vector<lt::peer_info> info;
					h.get_peer_info(info);

					for (const lt::peer_info &pi : info) {
						QString p = ipPortToText(pi.ip);

						if (!p.isEmpty()) {
							peers.insert(p);
							peerSources[p].insert("connected/get_peer_info");
						}
					}
				} catch (...) {
				}

				if ((int)peers.size() != last_count || first_poll) {
					last_count = (int)peers.size();
					emit logMessage("Seen/known peers: " + QString::number(last_count));
					emit peerCountChanged(last_count);
				}

				if (includeIpInfoComments && !peers.empty()) {
					QStringList ipinfo_logs;
					ensureIpInfoForPeers(peers, ipInfoCache, ipInfoToken, 10, &ipinfo_logs);
					for (const QString &line : ipinfo_logs) {
						emit logMessage(line);
					}
				}

				if (first_poll || timer.elapsed() - last_save_ms >= 10000) {
					if (!savePeersToFile(outputPath, peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments, &save_error)) {
						emit logMessage("Save error: " + save_error);
					}
					last_save_ms = timer.elapsed();
				}

				if (first_poll || timer.elapsed() - last_preview_ms >= 3000) {
					emit peersPreviewChanged(buildPeerOutputLines(peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments).join("\n") + (peers.empty() ? "" : "\n"));
					last_preview_ms = timer.elapsed();
				}

				first_poll = false;
			}

			if (stopRequested.load()) {
				emit logMessage("Stop requested by user.");
			}

			if (includeIpInfoComments && !peers.empty()) {
				QStringList ipinfo_logs;
				emit logMessage("Final IPinfo lookup for remaining peers...");
				ensureIpInfoForPeers(peers, ipInfoCache, ipInfoToken, 1000000, &ipinfo_logs);
				for (const QString &line : ipinfo_logs) {
					emit logMessage(line);
				}
			}

			if (!savePeersToFile(outputPath, peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments, &save_error)) {
				emit finishedStatus(false, "Final save error: " + save_error);
				return;
			}

			emit peersPreviewChanged(buildPeerOutputLines(peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments).join("\n") + (peers.empty() ? "" : "\n"));
			emit progressChanged(runTimeSeconds, runTimeSeconds);

			emit logMessage("");
			emit logMessage(buildPeerSummaryText(peers, peerSources));
			emit logMessage("Done. Unique seen/known peers saved: " + QString::number(peers.size()));
			emit logMessage("Saved to: " + outputPath);
			emit logMessage("");
			emit logMessage("Note:");
			emit logMessage("  PEX peers are only discovered after connecting to peers.");
			emit logMessage("  Running longer usually finds more peers.");

			emit finishedStatus(true, "Done. Unique seen/known peers saved: " + QString::number(peers.size()));

		} catch (const std::exception &e) {
			savePeersToFile(outputPath, peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments, nullptr);
			emit finishedStatus(false, "Exception: " + QString::fromUtf8(e.what()));
		} catch (...) {
			savePeersToFile(outputPath, peers, peerSources, ipInfoCache, includeSourceComments, includeIpInfoComments, nullptr);
			emit finishedStatus(false, "Unknown exception.");
		}
	}

private:
	QString torrentPath;
	QString outputPath;
	int runTimeSeconds;
	int pollIntervalMs;
	int trackerReannounceMs;
	int dhtReannounceMs;
	int listenPort;
	bool noDownload;
	bool enableTrackers;
	bool enableDht;
	bool enablePex;
	bool enableLsd;
	bool includeSourceComments;
	bool includeIpInfoComments;
	QString ipInfoToken;
	std::atomic<bool> stopRequested;
};

/* ------------------------------------------------------------------------- */
/* Main window                                                                */
/* ------------------------------------------------------------------------- */

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow(QWidget *parent = nullptr)
	    : QMainWindow(parent),
	      settings("IRT", "LibtorrentPeerCollectorCppQt"),
	      worker(nullptr)
	{
		QWidget *root;
		QVBoxLayout *main_layout;

		setWindowTitle("Libtorrent Peer Collector - C++ Qt");
		resize(1120, 780);

		root = new QWidget(this);
		setCentralWidget(root);

		main_layout = new QVBoxLayout(root);

		createFileGroup(main_layout);
		createOptionsGroup(main_layout);
		createButtons(main_layout);
		createTabs(main_layout);

		progress = new QProgressBar(this);
		progress->setRange(0, 100);
		progress->setValue(0);
		progress->setTextVisible(true);
		progress->setFormat("Ready");
		main_layout->addWidget(progress);

		progressTextLabel = new QLabel("Elapsed: 00:00:00    Remaining: --:--:--", this);
		main_layout->addWidget(progressTextLabel);

		statusLabel = new QLabel("Ready.", this);
		main_layout->addWidget(statusLabel);

		loadSettings();
	}

	~MainWindow() override
	{
		saveSettings();

		if (worker != nullptr) {
			worker->requestStop();
			worker->wait(3000);
		}
	}

protected:
	void closeEvent(QCloseEvent *event) override
	{
		saveSettings();

		if (worker != nullptr && worker->isRunning()) {
			QMessageBox::StandardButton reply;

			reply = QMessageBox::question(
			    this,
			    "Collection running",
			    "Collection is still running. Stop and close?",
			    QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);

			if (reply != QMessageBox::StandardButton::Yes) {
				event->ignore();
				return;
			}

			worker->requestStop();
			worker->wait(3000);
		}

		event->accept();
	}

private:
	QSettings settings;

	QLineEdit *torrentPathEdit = nullptr;
	QLineEdit *outputPathEdit = nullptr;
	QSpinBox *runTimeSpin = nullptr;
	QDoubleSpinBox *pollSpin = nullptr;
	QDoubleSpinBox *trackerReannounceSpin = nullptr;
	QDoubleSpinBox *dhtReannounceSpin = nullptr;
	QSpinBox *listenPortSpin = nullptr;
	QCheckBox *noDownloadCheck = nullptr;
	QCheckBox *trackersCheck = nullptr;
	QCheckBox *dhtCheck = nullptr;
	QCheckBox *pexCheck = nullptr;
	QCheckBox *lsdCheck = nullptr;
	QCheckBox *sourceCommentsCheck = nullptr;
	QCheckBox *ipInfoCommentsCheck = nullptr;
	QLineEdit *ipInfoTokenEdit = nullptr;
	QPushButton *startButton = nullptr;
	QPushButton *stopButton = nullptr;
	QPlainTextEdit *logText = nullptr;
	QPlainTextEdit *peersText = nullptr;
	QTabWidget *tabs = nullptr;
	QProgressBar *progress = nullptr;
	QLabel *progressTextLabel = nullptr;
	QLabel *statusLabel = nullptr;

	PeerCollectorThread *worker = nullptr;

	void createFileGroup(QVBoxLayout *parent)
	{
		QGroupBox *group;
		QGridLayout *grid;
		QPushButton *browse_torrent;
		QPushButton *browse_output;

		group = new QGroupBox("Files", this);
		grid = new QGridLayout(group);

		torrentPathEdit = new QLineEdit(group);
		outputPathEdit = new QLineEdit(group);

		browse_torrent = new QPushButton("Browse .torrent...", group);
		browse_output = new QPushButton("Choose output peers.txt...", group);

		grid->addWidget(new QLabel("Torrent file:", group), 0, 0);
		grid->addWidget(torrentPathEdit, 0, 1);
		grid->addWidget(browse_torrent, 0, 2);

		grid->addWidget(new QLabel("Output peers.txt:", group), 1, 0);
		grid->addWidget(outputPathEdit, 1, 1);
		grid->addWidget(browse_output, 1, 2);

		parent->addWidget(group);

		connect(browse_torrent, &QPushButton::clicked, this, &MainWindow::browseTorrent);
		connect(browse_output, &QPushButton::clicked, this, &MainWindow::browseOutput);
	}

	void createOptionsGroup(QVBoxLayout *parent)
	{
		QGroupBox *group;
		QFormLayout *form;

		group = new QGroupBox("Options", this);
		form = new QFormLayout(group);

		runTimeSpin = new QSpinBox(group);
		runTimeSpin->setRange(10, 86400);
		runTimeSpin->setValue(300);
		runTimeSpin->setSuffix(" sec");

		pollSpin = new QDoubleSpinBox(group);
		pollSpin->setRange(0.2, 60.0);
		pollSpin->setDecimals(1);
		pollSpin->setSingleStep(0.5);
		pollSpin->setValue(2.0);
		pollSpin->setSuffix(" sec");

		trackerReannounceSpin = new QDoubleSpinBox(group);
		trackerReannounceSpin->setRange(30.0, 7200.0);
		trackerReannounceSpin->setDecimals(0);
		trackerReannounceSpin->setSingleStep(30.0);
		trackerReannounceSpin->setValue(300.0);
		trackerReannounceSpin->setSuffix(" sec");

		dhtReannounceSpin = new QDoubleSpinBox(group);
		dhtReannounceSpin->setRange(30.0, 7200.0);
		dhtReannounceSpin->setDecimals(0);
		dhtReannounceSpin->setSingleStep(30.0);
		dhtReannounceSpin->setValue(120.0);
		dhtReannounceSpin->setSuffix(" sec");

		listenPortSpin = new QSpinBox(group);
		listenPortSpin->setRange(0, 65535);
		listenPortSpin->setValue(6881);

		noDownloadCheck = new QCheckBox("Try to avoid downloading payload pieces", group);
		noDownloadCheck->setChecked(true);

		trackersCheck = new QCheckBox("Trackers", group);
		trackersCheck->setChecked(true);

		dhtCheck = new QCheckBox("DHT", group);
		dhtCheck->setChecked(true);

		pexCheck = new QCheckBox("PEX / Peer Exchange (if libtorrent build supports toggling)", group);
		pexCheck->setChecked(true);

		lsdCheck = new QCheckBox("LSD / Local peer discovery", group);
		lsdCheck->setChecked(true);

		sourceCommentsCheck = new QCheckBox("Print peer source in comments: tracker-direct-http, connected/get_peer_info, etc.", group);
		sourceCommentsCheck->setChecked(true);

		ipInfoCommentsCheck = new QCheckBox("Print IPinfo-style country/provider info in comments", group);
		ipInfoCommentsCheck->setChecked(false);

		ipInfoTokenEdit = new QLineEdit(group);
		ipInfoTokenEdit->setPlaceholderText("Optional IPinfo token, not saved");
		ipInfoTokenEdit->setEchoMode(QLineEdit::Password);
		ipInfoTokenEdit->setToolTip("Optional. Leave blank to try IPinfo without a token. Token is not saved.");

		form->addRow("Run time:", runTimeSpin);
		form->addRow("Peer poll interval:", pollSpin);
		form->addRow("Tracker reannounce interval:", trackerReannounceSpin);
		form->addRow("DHT reannounce interval:", dhtReannounceSpin);
		form->addRow("Listen port:", listenPortSpin);
		form->addRow("", noDownloadCheck);
		form->addRow("Discovery:", trackersCheck);
		form->addRow("", dhtCheck);
		form->addRow("", pexCheck);
		form->addRow("", lsdCheck);
		form->addRow("Comments:", sourceCommentsCheck);
		form->addRow("", ipInfoCommentsCheck);
		form->addRow("IPinfo token:", ipInfoTokenEdit);

		connect(trackersCheck, &QCheckBox::toggled, this, &MainWindow::updatePexAvailability);
		connect(dhtCheck, &QCheckBox::toggled, this, &MainWindow::updatePexAvailability);
		connect(lsdCheck, &QCheckBox::toggled, this, &MainWindow::updatePexAvailability);

		parent->addWidget(group);
	}

	void createButtons(QVBoxLayout *parent)
	{
		QHBoxLayout *row;
		QPushButton *copy_peers;
		QPushButton *open_output;
		QPushButton *clear_log;

		row = new QHBoxLayout();

		startButton = new QPushButton("Start Collection", this);
		stopButton = new QPushButton("Stop", this);
		copy_peers = new QPushButton("Copy Peers", this);
		open_output = new QPushButton("Open Output File", this);
		clear_log = new QPushButton("Clear Log", this);

		startButton->setStyleSheet("font-weight: bold;");
		stopButton->setEnabled(false);

		row->addWidget(startButton);
		row->addWidget(stopButton);
		row->addWidget(copy_peers);
		row->addWidget(open_output);
		row->addWidget(clear_log);
		row->addStretch(1);

		parent->addLayout(row);

		connect(startButton, &QPushButton::clicked, this, &MainWindow::startCollection);
		connect(stopButton, &QPushButton::clicked, this, &MainWindow::stopCollection);
		connect(copy_peers, &QPushButton::clicked, this, &MainWindow::copyPeers);
		connect(open_output, &QPushButton::clicked, this, &MainWindow::openOutputFile);
		connect(clear_log, &QPushButton::clicked, this, [this]() {
			if (logText != nullptr) {
				logText->clear();
			}
		});
	}

	void createTabs(QVBoxLayout *parent)
	{
		tabs = new QTabWidget(this);

		logText = new QPlainTextEdit(tabs);
		peersText = new QPlainTextEdit(tabs);

		QFont monoFont("monospace");
		monoFont.setStyleHint(QFont::Monospace);
		monoFont.setFixedPitch(true);

		logText->setFont(monoFont);
		peersText->setFont(monoFont);

		logText->setReadOnly(true);
		peersText->setReadOnly(true);

		tabs->addTab(logText, "Log");
		tabs->addTab(peersText, "Seen / Known Peers");

		parent->addWidget(tabs, 1);
	}

	void loadSettings()
	{
		torrentPathEdit->setText(settings.value("paths/torrent", "").toString());
		outputPathEdit->setText(settings.value("paths/output", QDir::currentPath() + "/peers.txt").toString());
		runTimeSpin->setValue(settings.value("options/run_time", 300).toInt());
		pollSpin->setValue(settings.value("options/poll_interval", 2.0).toDouble());
		trackerReannounceSpin->setValue(settings.value("options/tracker_reannounce_interval", 300.0).toDouble());
		dhtReannounceSpin->setValue(settings.value("options/dht_reannounce_interval", 120.0).toDouble());
		listenPortSpin->setValue(settings.value("options/listen_port", 6881).toInt());
		noDownloadCheck->setChecked(settings.value("options/no_download", true).toBool());
		trackersCheck->setChecked(settings.value("options/trackers", true).toBool());
		dhtCheck->setChecked(settings.value("options/dht", true).toBool());
		pexCheck->setChecked(settings.value("options/pex", true).toBool());
		lsdCheck->setChecked(settings.value("options/lsd", true).toBool());
		sourceCommentsCheck->setChecked(settings.value("options/source_comments", true).toBool());
		ipInfoCommentsCheck->setChecked(settings.value("options/ipinfo_comments", false).toBool());
		ipInfoTokenEdit->clear();

		updatePexAvailability();
	}

	void saveSettings()
	{
		settings.setValue("paths/torrent", torrentPathEdit->text().trimmed());
		settings.setValue("paths/output", outputPathEdit->text().trimmed());
		settings.setValue("options/run_time", runTimeSpin->value());
		settings.setValue("options/poll_interval", pollSpin->value());
		settings.setValue("options/tracker_reannounce_interval", trackerReannounceSpin->value());
		settings.setValue("options/dht_reannounce_interval", dhtReannounceSpin->value());
		settings.setValue("options/listen_port", listenPortSpin->value());
		updatePexAvailability();

		settings.setValue("options/no_download", noDownloadCheck->isChecked());
		settings.setValue("options/trackers", trackersCheck->isChecked());
		settings.setValue("options/dht", dhtCheck->isChecked());
		settings.setValue("options/pex", pexCheck->isChecked());
		settings.setValue("options/lsd", lsdCheck->isChecked());
		settings.setValue("options/source_comments", sourceCommentsCheck->isChecked());
		settings.setValue("options/ipinfo_comments", ipInfoCommentsCheck->isChecked());
		/* IPinfo token is intentionally not saved. */
		settings.sync();
	}


	void updatePexAvailability()
	{
		bool has_initial_source;

		has_initial_source =
		    (trackersCheck != nullptr && trackersCheck->isChecked()) ||
		    (dhtCheck != nullptr && dhtCheck->isChecked()) ||
		    (lsdCheck != nullptr && lsdCheck->isChecked());

		if (pexCheck == nullptr) {
			return;
		}

		if (!has_initial_source) {
			pexCheck->setChecked(false);
			pexCheck->setEnabled(false);
			pexCheck->setToolTip(
			    "PEX needs at least one initial peer source. "
			    "Enable Trackers, DHT, or LSD first.");
		} else {
			pexCheck->setEnabled(true);
			pexCheck->setToolTip(
			    "PEX can exchange peer lists only after at least one peer is connected.");
		}
	}


	QString formatSeconds(int seconds) const
	{
		if (seconds < 0) {
			seconds = 0;
		}

		int h = seconds / 3600;
		int m = (seconds % 3600) / 60;
		int s = seconds % 60;

		return QString("%1:%2:%3")
		    .arg(h, 2, 10, QLatin1Char('0'))
		    .arg(m, 2, 10, QLatin1Char('0'))
		    .arg(s, 2, 10, QLatin1Char('0'));
	}

private slots:
	void browseTorrent()
	{
		QString path;

		path = QFileDialog::getOpenFileName(
		    this,
		    "Select .torrent file",
		    torrentPathEdit->text().trimmed().isEmpty() ? QDir::homePath() : torrentPathEdit->text().trimmed(),
		    "Torrent files (*.torrent);;All files (*)");

		if (!path.isEmpty()) {
			torrentPathEdit->setText(path);

			if (outputPathEdit->text().trimmed().isEmpty()) {
				outputPathEdit->setText(QFileInfo(path).absolutePath() + "/peers.txt");
			}

			saveSettings();
		}
	}

	void browseOutput()
	{
		QString path;

		path = QFileDialog::getSaveFileName(
		    this,
		    "Choose output peers.txt",
		    outputPathEdit->text().trimmed().isEmpty() ? QDir::currentPath() + "/peers.txt" : outputPathEdit->text().trimmed(),
		    "Text files (*.txt);;All files (*)");

		if (!path.isEmpty()) {
			outputPathEdit->setText(path);
			saveSettings();
		}
	}

	void startCollection()
	{
		QString torrent_path;
		QString output_path;

		torrent_path = torrentPathEdit->text().trimmed();
		output_path = outputPathEdit->text().trimmed();

		if (torrent_path.isEmpty()) {
			QMessageBox::warning(this, "Missing torrent", "Choose a .torrent file.");
			return;
		}

		if (output_path.isEmpty()) {
			QMessageBox::warning(this, "Missing output", "Choose output peers.txt path.");
			return;
		}

		if (!QFileInfo::exists(torrent_path)) {
			QMessageBox::warning(this, "Torrent not found", "Torrent file does not exist:\n" + torrent_path);
			return;
		}

		updatePexAvailability();

		if (!trackersCheck->isChecked() && !dhtCheck->isChecked() && !lsdCheck->isChecked()) {
			QMessageBox::warning(
			    this,
			    "No initial peer source",
			    "Trackers, DHT, and LSD are all disabled.\n\n"
			    "PEX cannot work by itself because it only exchanges peers after "
			    "at least one peer is already connected.\n\n"
			    "Enable Trackers, DHT, or LSD.");
			return;
		}

		saveSettings();

		logText->clear();
		peersText->clear();
		progress->setValue(0);
		progress->setFormat("0%");
		progressTextLabel->setText("Elapsed: 00:00:00    Remaining: --:--:--");
		statusLabel->setText("Starting...");

		worker = new PeerCollectorThread(
		    torrent_path,
		    output_path,
		    runTimeSpin->value(),
		    pollSpin->value(),
		    trackerReannounceSpin->value(),
		    dhtReannounceSpin->value(),
		    listenPortSpin->value(),
		    noDownloadCheck->isChecked(),
		    trackersCheck->isChecked(),
		    dhtCheck->isChecked(),
		    pexCheck->isChecked(),
		    lsdCheck->isChecked(),
		    sourceCommentsCheck->isChecked(),
		    ipInfoCommentsCheck->isChecked(),
		    ipInfoTokenEdit->text().trimmed(),
		    this);

		connect(worker, &PeerCollectorThread::logMessage, this, &MainWindow::appendLog);
		connect(worker, &PeerCollectorThread::peerCountChanged, this, &MainWindow::setPeerCount);
		connect(worker, &PeerCollectorThread::peersPreviewChanged, this, &MainWindow::setPeersText);
		connect(worker, &PeerCollectorThread::progressChanged, this, &MainWindow::setProgress);
		connect(worker, &PeerCollectorThread::finishedStatus, this, &MainWindow::collectionFinished);
		connect(worker, &QThread::finished, worker, &QObject::deleteLater);

		startButton->setEnabled(false);
		stopButton->setText("Stop");
		stopButton->setEnabled(true);

		worker->start();
	}

	void stopCollection()
	{
		if (worker != nullptr) {
			worker->requestStop();

			stopButton->setEnabled(false);
			stopButton->setText("Stopping...");
			statusLabel->setText("Stopping... waiting for worker to finish current poll cycle.");

			appendLog("Stop requested. Waiting for current tracker/DHT/peer poll cycle to finish...");
		}
	}

	void appendLog(const QString &text)
	{
		logText->appendPlainText(text);
	}

	void setPeerCount(int count)
	{
		statusLabel->setText("Running. Unique seen/known peers: " + QString::number(count));
	}

	void setPeersText(const QString &text)
	{
		peersText->setPlainText(text);
	}

	void setProgress(int elapsed, int total)
	{
		int percent;
		int remaining;

		if (total <= 0) {
			progress->setValue(0);
			progress->setFormat("0%");
			if (progressTextLabel != nullptr) {
				progressTextLabel->setText("Elapsed: 00:00:00    Remaining: --:--:--");
			}
			return;
		}

		percent = (elapsed * 100) / total;
		if (percent < 0) percent = 0;
		if (percent > 100) percent = 100;

		remaining = total - elapsed;
		if (remaining < 0) {
			remaining = 0;
		}

		progress->setValue(percent);
		progress->setFormat(QString("%1%").arg(percent));

		if (progressTextLabel != nullptr) {
			progressTextLabel->setText(
			    "Elapsed: " + formatSeconds(elapsed) +
			    "    Remaining: " + formatSeconds(remaining) +
			    "    Total: " + formatSeconds(total));
		}
	}

	void collectionFinished(bool ok, const QString &message)
	{
		startButton->setEnabled(true);
		stopButton->setEnabled(false);
		stopButton->setText("Stop");
		statusLabel->setText(message);

		if (ok) {
			progress->setValue(100);
			progress->setFormat("100%");
			if (progressTextLabel != nullptr) {
				progressTextLabel->setText("Finished.");
			}
			QMessageBox::information(this, "Done", message);
		} else {
			QMessageBox::critical(this, "Collection failed", message);
		}

		worker = nullptr;
	}

	void copyPeers()
	{
		QApplication::clipboard()->setText(peersText->toPlainText());
		statusLabel->setText("Peers copied to clipboard.");
	}

	void openOutputFile()
	{
		QString path;
		QFile f;

		path = outputPathEdit->text().trimmed();

		if (path.isEmpty()) {
			QMessageBox::information(this, "No output path", "No output file selected.");
			return;
		}

		if (!QFileInfo::exists(path)) {
			QMessageBox::information(this, "File not found", "Output file does not exist yet:\n" + path);
			return;
		}

		f.setFileName(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			QMessageBox::warning(this, "Read failed", f.errorString());
			return;
		}

		peersText->setPlainText(QString::fromUtf8(f.readAll()));
		tabs->setCurrentWidget(peersText);
		statusLabel->setText("Loaded output file: " + path);
	}
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	MainWindow win;

	win.show();

	return app.exec();
}

#include "libtorrent_peer_collector_qt.moc"
