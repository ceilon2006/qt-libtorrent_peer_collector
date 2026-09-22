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
 * Build (out-of-tree, into build/):
 *   scripts/linux/build.sh      [release|debug]
 *   scripts\windows\build.bat   [release|debug]
 *
 * rebuild.* cleans first, clean.* removes every generated file, deploy.*
 * copies the executable (plus Qt/libtorrent DLLs on Windows) to deploy/.
 *
 * By hand:
 *   qmake6 libtorrent_peer_collector_qt.pro
 *   make
 *
 * Required packages: see docs/build-requirements.html.
 */

#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QUrlQuery>
#include <QtCore/QJsonValue>
#include <QtCore/QLocale>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QWaitCondition>
#include <QtCore/QPointer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <QtGui/QClipboard>
#include <QtGui/QAction>
#include <QtGui/QFont>
#include <QtGui/QStandardItemModel>
#include <QtGui/QCloseEvent>
#include <QtGui/QShortcut>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QUdpSocket>

#include <QtCore/QEventLoop>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/bitfield.hpp>
#include <libtorrent/extensions.hpp>
#include <libtorrent/peer_connection_handle.hpp>
#include <libtorrent/fingerprint.hpp>
#include <libtorrent/extensions/smart_ban.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/extensions/ut_pex.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lt = libtorrent;

/*
 * Identity for settings and per-user data. CONFIG_FOLDER_NAME is the
 * organization: on Linux ~/.config/<CONFIG_FOLDER_NAME>/<APP_NAME>.conf, on
 * Windows HKCU\Software\<CONFIG_FOLDER_NAME>\<APP_NAME>. Same names as
 * the other tools in this family so they share one folder.
 */
#define CONFIG_FOLDER_NAME      "myutils"
#define APP_NAME                "LibtorrentPeerCollectorQt"

/*
 * Public trackers added to every torrent when the Extra trackers box is
 * left at its default. Large, long-lived, and they answered from this
 * machine on 2026-09-21. Torrents from private-style sites list only their
 * own HTTP trackers; peers of those torrents often announce to public
 * trackers as well, and this is the only way to reach that pool.
 */
#define DEFAULT_EXTRA_TRACKERS \
	"udp://tracker.opentrackr.org:1337/announce\n" \
	"udp://open.stealth.si:80/announce\n" \
	"udp://tracker.torrent.eu.org:451/announce\n" \
	"udp://exodus.desync.com:6969/announce\n"

/* Log tab cap. A 24 h run can log hundreds of thousands of lines. */
#define LOG_MAX_LINES           20000

/*
 * One row of the peers table, sent from the worker to the GUI. ipinfo is the
 * formatted "ipinfo: country=.., city=.., provider=.., host=.." string,
 * "ipinfo=pending", "ipinfo=unavailable", or empty when lookups are off.
 */
struct PeerRow {
	QString endpoint;
	QStringList sources;
	QString ipinfo;
	bool active = false;     /* was connected at some poll during this run */
	int progress_ppm = -1;   /* share of the torrent the peer had when last connected, -1 unknown */
};

using PeerRows = QList<PeerRow>;

Q_DECLARE_METATYPE(PeerRows)

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

/*
 * Addresses that are this machine. Trackers return the announcing peer in
 * their reply, so without this the collector lists itself and libtorrent
 * even tries to connect to it. Seeded with every local interface address;
 * the worker adds the external address libtorrent reports.
 */
static std::set<QString> localInterfaceIpv4Addresses()
{
	std::set<QString> out;

	for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
		if (a.protocol() == QAbstractSocket::IPv4Protocol) {
			out.insert(a.toString());
		}
	}

	return out;
}

/* Drop every peer whose IP is one of ours. Returns the removed endpoints. */
static QStringList pruneSelfPeers(std::set<QString> &peers,
                                  std::map<QString, std::set<QString>> &peer_sources,
                                  const std::set<QString> &self_ips)
{
	QStringList removed;

	for (auto it = peers.begin(); it != peers.end(); ) {
		if (self_ips.find(peerIpOnly(*it)) != self_ips.end()) {
			removed << *it;
			peer_sources.erase(*it);
			it = peers.erase(it);
		} else {
			++it;
		}
	}

	return removed;
}

static PeerRows buildPeerRows(const std::set<QString> &peers,
                              const std::map<QString, std::set<QString>> &peer_sources,
                              const std::map<QString, QString> &ip_info_cache,
                              const std::set<QString> &active_peers,
                              const std::map<QString, int> &peer_progress_ppm,
                              bool include_ipinfo)
{
	PeerRows rows;

	for (const QString &p : sortedPeerList(peers)) {
		PeerRow row;

		row.endpoint = p;
		row.active = active_peers.find(p) != active_peers.end();

		auto pit = peer_progress_ppm.find(p);
		if (pit != peer_progress_ppm.end()) {
			row.progress_ppm = pit->second;
		}

		auto sit = peer_sources.find(p);
		if (sit != peer_sources.end()) {
			for (const QString &src : sit->second) {
				row.sources << src;
			}
			row.sources.sort();
		}

		if (include_ipinfo) {
			auto iit = ip_info_cache.find(peerIpOnly(p));
			row.ipinfo = (iit != ip_info_cache.end() && !iit->second.isEmpty())
			                 ? iit->second
			                 : QString("ipinfo=pending");
		}

		rows << row;
	}

	return rows;
}

/*
 * Split a formatted ipinfo string back into its fields for the table. The
 * values may themselves contain commas (an organisation name), so the split
 * happens only before a known key.
 */
static void parseIpInfoFields(const QString &ipinfo,
                              QString *country,
                              QString *city,
                              QString *provider,
                              QString *host)
{
	static const QRegularExpression splitter(",\\s(?=(?:country|city|region|provider|host)=)");
	QString body = ipinfo;

	*country = QString();
	*city = QString();
	*provider = QString();
	*host = QString();

	if (body.startsWith("ipinfo: ")) {
		body = body.mid(8);
	} else {
		/* ipinfo=pending / ipinfo=unavailable / anything else */
		if (body.startsWith("ipinfo=")) {
			*country = "(" + body.mid(7) + ")";
		}
		return;
	}

	for (const QString &part : body.split(splitter)) {
		int eq = part.indexOf('=');
		if (eq <= 0) {
			continue;
		}

		QString key = part.left(eq);
		QString value = part.mid(eq + 1);

		if (key == "country") {
			*country = value;
		} else if (key == "city") {
			*city = value;
		} else if (key == "region") {
			if (city->isEmpty()) {
				*city = value;
			}
		} else if (key == "provider") {
			*provider = value;
		} else if (key == "host") {
			*host = value;
		}
	}
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
	int tracker_direct_http = countPeersWithSource(peer_sources, "tracker-direct-http");
	int tracker_direct_udp = countPeersWithSource(peer_sources, "tracker-direct-udp");
	int connect_in = countPeersWithSource(peer_sources, "peer-connect-in");
	int connect_out = countPeersWithSource(peer_sources, "peer-connect-out");
	int connect_failed = countPeersWithSource(peer_sources, "peer-connect-failed");
	int disconnected = countPeersWithSource(peer_sources, "peer-disconnected");
	int incoming = countPeersWithSource(peer_sources, "peer-incoming");
	int error_ban_block = countPeersWithSource(peer_sources, "peer-error") +
	                      countPeersWithSource(peer_sources, "peer-banned") +
	                      countPeersWithSource(peer_sources, "peer-blocked");
	int other_alert = countPeersWithSource(peer_sources, "peer-alert");
	int any_alert = countPeersWithSourcePrefix(peer_sources, "peer-");
	int unknown = countPeersWithoutKnownSource(peers, peer_sources);

	return QString(
	    "Peer summary:\n"
	    "  Total unique peers       : %1\n"
	    "  Connected/get_peer_info  : %2\n"
	    "  Direct HTTP tracker      : %3\n"
	    "  Direct UDP tracker       : %4\n"
	    "  Connected in / out       : %5 / %6\n"
	    "  Connect failed           : %7\n"
	    "  Disconnected             : %8\n"
	    "  Incoming (pre-handshake) : %9\n"
	    "  Error/banned/blocked     : %10\n"
	    "  Other peer alert         : %11\n"
	    "  Any peer alert source    : %12\n"
	    "  Unknown/no source tag    : %13\n")
	    .arg(peers.size())
	    .arg(connected)
	    .arg(tracker_direct_http)
	    .arg(tracker_direct_udp)
	    .arg(connect_in)
	    .arg(connect_out)
	    .arg(connect_failed)
	    .arg(disconnected)
	    .arg(incoming)
	    .arg(error_ban_block)
	    .arg(other_alert)
	    .arg(any_alert)
	    .arg(unknown);
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

/*
 * The one peer id for the whole run. It is handed to libtorrent as its full
 * peer id (settings_pack::peer_fingerprint, 20 bytes) and used verbatim by
 * the direct announce, so trackers and peers see a single client. The prefix
 * is libtorrent's own Azureus-style fingerprint, which is what actually
 * speaks to peers; the rest is random.
 */
static QByteArray makePeerId()
{
	QByteArray peer_id = QByteArray::fromStdString(lt::generate_fingerprint(
	    "LT", LIBTORRENT_VERSION_MAJOR, LIBTORRENT_VERSION_MINOR, LIBTORRENT_VERSION_TINY, 0));

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

/*
 * State for the direct announces of one run.
 *
 * A tracker expects one peer id for the lifetime of a "peer", event=started
 * once, plain announces after that, and event=stopped at the end. The first
 * version of this tool sent started with a fresh random id every interval,
 * which trackers count as a new leecher each time.
 */
struct DirectAnnounceContext {
	QByteArray peer_id;
	quint32 key = 0;
	std::set<QString> started;    /* trackers that replied to event=started */
};

/* True when the user pressed Stop. Every wait below polls it. */
static bool announceCancelled(const std::atomic<bool> *cancel)
{
	return cancel != nullptr && cancel->load();
}

/* BEP 3 event names as sent to HTTP trackers; BEP 15 numeric codes for UDP. */
enum DirectAnnounceEvent {
	ANNOUNCE_EVENT_NONE = 0,
	ANNOUNCE_EVENT_COMPLETED = 1,
	ANNOUNCE_EVENT_STARTED = 2,
	ANNOUNCE_EVENT_STOPPED = 3
};

static const char *announceEventName(DirectAnnounceEvent event)
{
	switch (event) {
	case ANNOUNCE_EVENT_STARTED:
		return "started";
	case ANNOUNCE_EVENT_STOPPED:
		return "stopped";
	case ANNOUNCE_EVENT_COMPLETED:
		return "completed";
	default:
		return "";
	}
}

static qint64 torrentLeftBytes(const lt::torrent_info &ti)
{
	try {
		return (qint64)ti.total_size();
	} catch (...) {
		return 0;
	}
}

static QByteArray buildTrackerAnnounceUrl(const QString &tracker_url,
                                          const lt::torrent_info &ti,
                                          int listen_port,
                                          const QByteArray &peer_id,
                                          quint32 key,
                                          DirectAnnounceEvent event)
{
	QByteArray url = tracker_url.toUtf8();
	QByteArray info_hash_bytes = sha1HashToBytes(ti.info_hash());
	const char *event_name = announceEventName(event);

	url += (url.contains('?') ? '&' : '?');
	url += "info_hash=" + urlEncodeBytes(info_hash_bytes);
	url += "&peer_id=" + urlEncodeBytes(peer_id);
	url += "&port=" + QByteArray::number(listen_port);
	url += "&uploaded=0";
	url += "&downloaded=0";
	url += "&left=" + QByteArray::number(torrentLeftBytes(ti));
	url += "&compact=1";
	url += "&numwant=" + QByteArray::number(event == ANNOUNCE_EVENT_STOPPED ? 0 : 200);
	url += "&key=" + QByteArray::number(key, 16);

	if (event_name[0] != '\0') {
		url += "&event=";
		url += event_name;
	}

	return url;
}

/*
 * One HTTP/HTTPS announce. *replied is set when the tracker answered at all,
 * even with a failure reason: that is what decides whether it later needs
 * event=stopped. The wait polls the cancel flag every 200 ms.
 */
static QStringList directHttpTrackerAnnounce(const QString &tracker_url,
                                             const lt::torrent_info &ti,
                                             int listen_port,
                                             const DirectAnnounceContext &ctx,
                                             DirectAnnounceEvent event,
                                             int timeout_ms,
                                             const std::atomic<bool> *cancel,
                                             bool *replied,
                                             QStringList *log_lines)
{
	QStringList peers;
	QByteArray full_url = buildTrackerAnnounceUrl(tracker_url, ti, listen_port, ctx.peer_id, ctx.key, event);
	bool cancelled = false;

	if (replied != nullptr) {
		*replied = false;
	}

	QNetworkAccessManager manager;
	QNetworkRequest request(QUrl::fromEncoded(full_url));

	/*
	 * Some trackers (rutracker's bt*.t-ru.org among them) answer 403 to a
	 * request without a client-looking User-Agent. This announce is made on
	 * behalf of the libtorrent session in this process, so present it as
	 * that libtorrent, the same string the session's own announces carry.
	 */
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString("libtorrent/") + QString::fromLatin1(LIBTORRENT_VERSION));

	QNetworkReply *reply = manager.get(request);

	QEventLoop loop;
	QTimer timeout;
	QTimer cancel_poll;
	timeout.setSingleShot(true);

	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&cancel_poll, &QTimer::timeout, &loop, [&]() {
		if (announceCancelled(cancel)) {
			cancelled = true;
			loop.quit();
		}
	});

	timeout.start(timeout_ms);
	cancel_poll.start(200);
	loop.exec();
	cancel_poll.stop();

	if (cancelled || !timeout.isActive()) {
		timeout.stop();
		reply->abort();
		reply->deleteLater();
		if (log_lines != nullptr && !cancelled) {
			*log_lines << "Direct tracker announce timeout: " + tracker_url;
		}
		return peers;
	}

	timeout.stop();

	QByteArray data = reply->readAll();
	QNetworkReply::NetworkError net_error = reply->error();
	QString error_string = reply->errorString();
	bool got_http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid();
	reply->deleteLater();

	/*
	 * "Replied" means the tracker itself answered, even with an HTTP error
	 * such as 403. A DNS failure or a refused connection is not a reply, so
	 * the tracker is retried with "started" next time and gets no "stopped".
	 */
	if (replied != nullptr) {
		*replied = (net_error == QNetworkReply::NoError) || got_http_status;
	}

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

	if (log_lines != nullptr && event != ANNOUNCE_EVENT_STOPPED) {
		*log_lines << QString("Direct tracker announce: %1 -> %2 compact IPv4 peers")
		                  .arg(tracker_url)
		                  .arg(peers.size());
	}

	return peers;
}

/* ------------------------------------------------------------------------- */
/* Direct UDP tracker announce (BEP 15)                                       */
/* ------------------------------------------------------------------------- */

static quint32 readBigEndian32(const QByteArray &data, int offset)
{
	return ((quint32)(unsigned char)data[offset] << 24) |
	       ((quint32)(unsigned char)data[offset + 1] << 16) |
	       ((quint32)(unsigned char)data[offset + 2] << 8) |
	       ((quint32)(unsigned char)data[offset + 3]);
}

/*
 * Send one request and wait for the reply that carries our transaction id.
 * BEP 15 suggests retrying with 15 * 2^n second timeouts; two attempts of
 * timeout_ms each keep the collector's poll cycle bounded instead.
 * Returns the datagram, or empty on timeout.
 */
static QByteArray udpTrackerExchange(QUdpSocket &sock,
                                     const QHostAddress &addr,
                                     quint16 port,
                                     const QByteArray &request,
                                     quint32 transaction_id,
                                     int timeout_ms,
                                     int attempts,
                                     const std::atomic<bool> *cancel)
{
	for (int attempt = 0; attempt < attempts; attempt++) {
		QElapsedTimer t;

		if (announceCancelled(cancel)) {
			return QByteArray();
		}

		if (sock.writeDatagram(request, addr, port) != request.size()) {
			return QByteArray();
		}

		t.start();

		while (t.elapsed() < timeout_ms) {
			int remaining = timeout_ms - (int)t.elapsed();

			if (announceCancelled(cancel)) {
				return QByteArray();
			}

			/* Wait in short slices so a Stop is noticed within 200 ms. */
			if (remaining > 200) {
				remaining = 200;
			}

			if (!sock.waitForReadyRead(remaining)) {
				continue;
			}

			while (sock.hasPendingDatagrams()) {
				QByteArray d;

				d.resize((int)sock.pendingDatagramSize());
				sock.readDatagram(d.data(), d.size());

				if (d.size() >= 8 && readBigEndian32(d, 4) == transaction_id) {
					return d;
				}
			}
		}
	}

	return QByteArray();
}

static QStringList directUdpTrackerAnnounce(const QString &tracker_url,
                                            const lt::torrent_info &ti,
                                            int listen_port,
                                            const DirectAnnounceContext &ctx,
                                            DirectAnnounceEvent event,
                                            int timeout_ms,
                                            int attempts,
                                            const std::atomic<bool> *cancel,
                                            bool *replied,
                                            QStringList *log_lines)
{
	QStringList peers;

	if (replied != nullptr) {
		*replied = false;
	}
	QUrl parsed(tracker_url);
	QString host = parsed.host();
	int port = parsed.port(-1);
	QHostAddress addr;
	QUdpSocket sock;
	QByteArray request;
	QByteArray reply;
	quint32 tid;
	quint64 connection_id;
	quint32 action;

	if (host.isEmpty() || port <= 0 || port > 65535) {
		if (log_lines != nullptr) {
			*log_lines << "Direct UDP announce: bad tracker URL: " + tracker_url;
		}
		return peers;
	}

	/* Blocking name lookup. This runs in the worker thread. */
	QHostInfo info = QHostInfo::fromName(host);

	for (const QHostAddress &a : info.addresses()) {
		if (a.protocol() == QAbstractSocket::IPv4Protocol) {
			addr = a;
			break;
		}
	}

	if (addr.isNull()) {
		if (log_lines != nullptr) {
			*log_lines << "Direct UDP announce: could not resolve " + host + " (" + info.errorString() + ")";
		}
		return peers;
	}

	if (!sock.bind(QHostAddress::AnyIPv4, 0)) {
		if (log_lines != nullptr) {
			*log_lines << "Direct UDP announce: bind failed: " + sock.errorString();
		}
		return peers;
	}

	/* ---- connect: protocol id, action 0, transaction id ---- */
	tid = QRandomGenerator::global()->generate();
	{
		QDataStream ds(&request, QIODevice::WriteOnly);
		ds.setByteOrder(QDataStream::BigEndian);
		ds << (quint64)0x41727101980ULL << (quint32)0 << tid;
	}

	reply = udpTrackerExchange(sock, addr, (quint16)port, request, tid, timeout_ms, attempts, cancel);

	if (reply.size() < 16 || readBigEndian32(reply, 0) != 0) {
		if (log_lines != nullptr && !announceCancelled(cancel)) {
			*log_lines << "Direct UDP announce: no connect reply from " + tracker_url;
		}
		return peers;
	}

	connection_id = ((quint64)readBigEndian32(reply, 8) << 32) | readBigEndian32(reply, 12);

	/* ---- announce: 98 bytes ---- */
	tid = QRandomGenerator::global()->generate();
	request.clear();
	{
		QByteArray info_hash_bytes = sha1HashToBytes(ti.info_hash());
		QByteArray peer_id = ctx.peer_id;
		QDataStream ds(&request, QIODevice::WriteOnly);

		peer_id.resize(20);
		ds.setByteOrder(QDataStream::BigEndian);
		ds << connection_id << (quint32)1 << tid;
		ds.writeRawData(info_hash_bytes.constData(), 20);
		ds.writeRawData(peer_id.constData(), 20);
		ds << (quint64)0                          /* downloaded */
		   << (quint64)torrentLeftBytes(ti)       /* left */
		   << (quint64)0                          /* uploaded */
		   << (quint32)event
		   << (quint32)0                          /* IP: default */
		   << ctx.key
		   << (qint32)(event == ANNOUNCE_EVENT_STOPPED ? 0 : 200)
		   << (quint16)listen_port;
	}

	reply = udpTrackerExchange(sock, addr, (quint16)port, request, tid, timeout_ms, attempts, cancel);

	if (reply.size() < 8) {
		if (log_lines != nullptr && !announceCancelled(cancel)) {
			*log_lines << "Direct UDP announce timeout: " + tracker_url;
		}
		return peers;
	}

	if (replied != nullptr) {
		*replied = true;
	}

	action = readBigEndian32(reply, 0);

	if (action == 3) {
		if (log_lines != nullptr) {
			*log_lines << "Tracker failure: " + tracker_url + " : " + QString::fromUtf8(reply.mid(8));
		}
		return peers;
	}

	if (action != 1 || reply.size() < 20) {
		if (log_lines != nullptr) {
			*log_lines << "Direct UDP announce: unexpected reply from " + tracker_url;
		}
		return peers;
	}

	peers = parseCompactIpv4Peers(reply.mid(20));

	if (log_lines != nullptr && event != ANNOUNCE_EVENT_STOPPED) {
		*log_lines << QString("Direct UDP announce: %1 -> %2 compact IPv4 peers (seeders %3, leechers %4)")
		                  .arg(tracker_url)
		                  .arg(peers.size())
		                  .arg(readBigEndian32(reply, 16))
		                  .arg(readBigEndian32(reply, 12));
	}

	return peers;
}

/* ------------------------------------------------------------------------- */
/* Direct announce driver                                                     */
/* ------------------------------------------------------------------------- */

/*
 * One tracker's announce, run on its own thread. Each thread owns its
 * QNetworkAccessManager or QUdpSocket, created inside the announce function,
 * so nothing is shared except the read-only context and the cancel flag.
 */
struct TrackerAnnounceJob {
	QString tracker_url;
	QString scheme;
	DirectAnnounceEvent event = ANNOUNCE_EVENT_NONE;
	QStringList found;
	QStringList logs;
	bool replied = false;
};

/*
 * Run every job concurrently and wait for all of them. Trackers are
 * independent, and one dead hostname must not hold up the others: run in
 * sequence, eight trackers took 16 s; in parallel they take as long as the
 * slowest one. Stop is still honoured inside each job through cancel.
 */
static void runTrackerJobsInParallel(std::vector<TrackerAnnounceJob> &jobs,
                                     const lt::torrent_info &ti,
                                     int listen_port,
                                     const DirectAnnounceContext &ctx,
                                     int http_timeout_ms,
                                     int udp_timeout_ms,
                                     int udp_attempts,
                                     const std::atomic<bool> *cancel)
{
	std::vector<QThread *> threads;

	for (TrackerAnnounceJob &job : jobs) {
		QThread *t = QThread::create([&job, &ti, listen_port, &ctx, http_timeout_ms, udp_timeout_ms, udp_attempts, cancel]() {
			if (job.scheme == "http" || job.scheme == "https") {
				job.found = directHttpTrackerAnnounce(job.tracker_url, ti, listen_port, ctx, job.event,
				                                     http_timeout_ms, cancel, &job.replied, &job.logs);
			} else if (job.scheme == "udp") {
				job.found = directUdpTrackerAnnounce(job.tracker_url, ti, listen_port, ctx, job.event,
				                                    udp_timeout_ms, udp_attempts, cancel, &job.replied, &job.logs);
			}
		});

		threads.push_back(t);
		t->start();
	}

	for (QThread *t : threads) {
		t->wait();
		delete t;
	}
}

/*
 * Announce to every tracker in the torrent that speaks HTTP, HTTPS or UDP.
 * The first announce to a tracker sends event=started; later ones send no
 * event. Returns how many peers were new to the set.
 */
static int directAnnounceAllTrackers(const std::shared_ptr<lt::torrent_info> &ti,
                                     int listen_port,
                                     DirectAnnounceContext &ctx,
                                     std::set<QString> &peers,
                                     std::map<QString, std::set<QString>> &peer_sources,
                                     const std::atomic<bool> *cancel,
                                     QStringList *log_lines)
{
	std::vector<TrackerAnnounceJob> jobs;
	QElapsedTimer timer;
	int added = 0;

	if (!ti) {
		return 0;
	}

	timer.start();

	for (const lt::announce_entry &ae : ti->trackers()) {
		TrackerAnnounceJob job;

		job.tracker_url = QString::fromStdString(ae.url);
		job.scheme = QUrl(job.tracker_url).scheme().toLower();

		if (job.scheme != "http" && job.scheme != "https" && job.scheme != "udp") {
			if (log_lines != nullptr) {
				*log_lines << "Direct tracker announce skipped unsupported scheme: " + job.tracker_url;
			}
			continue;
		}

		job.event = (ctx.started.find(job.tracker_url) == ctx.started.end())
		                ? ANNOUNCE_EVENT_STARTED
		                : ANNOUNCE_EVENT_NONE;

		jobs.push_back(job);
	}

	if (jobs.empty() || announceCancelled(cancel)) {
		return 0;
	}

	runTrackerJobsInParallel(jobs, *ti, listen_port, ctx, 15000, 5000, 2, cancel);

	for (const TrackerAnnounceJob &job : jobs) {
		const char *tag = (job.scheme == "udp") ? "tracker-direct-udp" : "tracker-direct-http";

		if (log_lines != nullptr) {
			*log_lines << job.logs;
		}

		/*
		 * Only a tracker that answered has heard "started" from this peer id.
		 * It must not get it again, and it is the only kind worth a "stopped"
		 * at the end. A tracker that timed out is retried with "started" next
		 * interval and never gets a pointless "stopped".
		 */
		if (job.replied) {
			ctx.started.insert(job.tracker_url);
		}

		for (const QString &p : job.found) {
			if (peers.insert(p).second) {
				added++;
			}

			peer_sources[p].insert(tag);
		}
	}

	if (log_lines != nullptr && !announceCancelled(cancel)) {
		*log_lines << QString("Direct announces to %1 tracker(s) took %2 ms.")
		                  .arg(jobs.size())
		                  .arg(timer.elapsed());
	}

	return added;
}

/*
 * Tell every tracker that answered a "started" that this peer is gone. All
 * at once, single attempt, short timeouts: the run is ending and nothing
 * depends on the replies.
 */
static void directStopAllTrackers(const std::shared_ptr<lt::torrent_info> &ti,
                                  int listen_port,
                                  const DirectAnnounceContext &ctx,
                                  QStringList *log_lines)
{
	std::vector<TrackerAnnounceJob> jobs;
	QElapsedTimer timer;
	int sent = 0;

	if (!ti || ctx.started.empty()) {
		return;
	}

	timer.start();

	for (const QString &tracker_url : ctx.started) {
		TrackerAnnounceJob job;

		job.tracker_url = tracker_url;
		job.scheme = QUrl(tracker_url).scheme().toLower();
		job.event = ANNOUNCE_EVENT_STOPPED;
		jobs.push_back(job);
	}

	runTrackerJobsInParallel(jobs, *ti, listen_port, ctx, 2000, 1000, 1, nullptr);

	for (const TrackerAnnounceJob &job : jobs) {
		if (job.replied) {
			sent++;
		}
	}

	if (log_lines != nullptr) {
		*log_lines << QString("Sent event=stopped to %1 of %2 tracker(s) in %3 ms.")
		                  .arg(sent)
		                  .arg(jobs.size())
		                  .arg(timer.elapsed());
	}
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

/*
 * Background IPinfo lookups.
 *
 * Each lookup is a blocking HTTP GET with a 4 second timeout. Doing them in
 * the collector's poll loop stalled peer polling and made Stop wait for the
 * whole batch. This thread owns a queue of IPs and a result cache. The
 * collector enqueues new IPs every poll, takes a snapshot of the cache when
 * it writes output, and drains the log lines. All state is behind one mutex.
 *
 * No signals: the collector forwards the log lines itself so every message
 * to the GUI still comes from one place.
 */
class IpInfoLookupThread : public QThread {
public:
	explicit IpInfoLookupThread(const QString &token)
	    : QThread(nullptr),
	      token(token),
	      stopRequested(false)
	{
	}

	~IpInfoLookupThread() override
	{
		requestStop();
		wait();
	}

	/* Queue every IP from these peers that is neither cached nor already queued. */
	void enqueuePeers(const std::set<QString> &peers)
	{
		QMutexLocker lock(&mutex);
		bool added = false;

		for (const QString &peer : peers) {
			QString ip = peerIpOnly(peer);

			if (ip.isEmpty()) {
				continue;
			}

			if (cache.find(ip) != cache.end() || queued.find(ip) != queued.end()) {
				continue;
			}

			queue.push_back(ip);
			queued.insert(ip);
			added = true;
		}

		if (added) {
			cond.wakeOne();
		}
	}

	std::map<QString, QString> snapshot() const
	{
		QMutexLocker lock(&mutex);

		return cache;
	}

	/* Queued plus in flight. Zero means every enqueued IP is in the cache. */
	int pendingCount() const
	{
		QMutexLocker lock(&mutex);

		return (int)queued.size();
	}

	QStringList takeLogLines()
	{
		QMutexLocker lock(&mutex);
		QStringList out = logLines;

		logLines.clear();

		return out;
	}

	void requestStop()
	{
		QMutexLocker lock(&mutex);

		stopRequested = true;
		cond.wakeAll();
	}

protected:
	void run() override
	{
		while (true) {
			QString ip;
			QString error;
			QString info;
			QString line;

			{
				QMutexLocker lock(&mutex);

				while (queue.empty() && !stopRequested) {
					cond.wait(&mutex);
				}

				if (stopRequested) {
					return;
				}

				ip = queue.front();
				queue.pop_front();
			}

			info = fetchIpInfoText(ip, token, &error);

			if (info.isEmpty()) {
				info = "ipinfo=unavailable";
				line = "IPinfo lookup failed for " + ip + ": " + error;
			} else {
				line = "IPinfo lookup: " + ip + " -> " + info;
			}

			{
				QMutexLocker lock(&mutex);

				cache[ip] = info;
				queued.erase(ip);
				logLines << line;
			}
		}
	}

private:
	QString token;
	mutable QMutex mutex;
	QWaitCondition cond;
	std::deque<QString> queue;
	std::set<QString> queued;
	std::map<QString, QString> cache;
	QStringList logLines;
	bool stopRequested;
};


/* ------------------------------------------------------------------------- */
/* Peer progress plugin                                                       */
/* ------------------------------------------------------------------------- */

/*
 * What every peer told us it has, recorded the moment it says so.
 *
 * Polling get_peer_info every couple of seconds misses peers that complete
 * the handshake and leave within the interval, which with minimal download
 * is most of them. A libtorrent peer plugin sees the handshake, the bitfield
 * and every later "have" for each connection, incoming or outgoing, on
 * libtorrent's network thread. It writes into this book under a mutex; the
 * worker merges it every poll.
 */
struct PeerProgressBook {
	QMutex mutex;
	int num_pieces = 0;
	std::set<QString> handshaked;          /* endpoints that completed the handshake */
	std::map<QString, int> best_ppm;       /* best share of the torrent each reported */
};

class ProgressPeerPlugin : public lt::peer_plugin {
public:
	ProgressPeerPlugin(const QString &endpoint, std::shared_ptr<PeerProgressBook> book)
	    : endpoint(endpoint),
	      book(std::move(book)),
	      have(0)
	{
	}

	bool on_handshake(lt::span<char const>) override
	{
		QMutexLocker lock(&book->mutex);

		book->handshaked.insert(endpoint);
		return true;
	}

	bool on_bitfield(lt::bitfield const &bits) override
	{
		have = bits.count();
		record();
		return false;
	}

	bool on_have_all() override
	{
		have = book->num_pieces;
		record();
		return false;
	}

	bool on_have_none() override
	{
		have = 0;
		record();
		return false;
	}

	bool on_have(lt::piece_index_t) override
	{
		if (have < book->num_pieces) {
			have++;
		}
		record();
		return false;
	}

private:
	void record()
	{
		QMutexLocker lock(&book->mutex);
		int ppm = book->num_pieces > 0 ? (int)((qint64)have * 1000000 / book->num_pieces) : 0;
		auto it = book->best_ppm.find(endpoint);

		book->handshaked.insert(endpoint);

		if (it == book->best_ppm.end() || ppm > it->second) {
			book->best_ppm[endpoint] = ppm;
		}
	}

	QString endpoint;
	std::shared_ptr<PeerProgressBook> book;
	int have;
};

class ProgressTorrentPlugin : public lt::torrent_plugin {
public:
	explicit ProgressTorrentPlugin(std::shared_ptr<PeerProgressBook> book)
	    : book(std::move(book))
	{
	}

	std::shared_ptr<lt::peer_plugin> new_connection(lt::peer_connection_handle const &pc) override
	{
		QString endpoint = ipPortToText(pc.remote());

		if (endpoint.isEmpty()) {
			return nullptr;    /* IPv6: not tracked */
		}

		return std::make_shared<ProgressPeerPlugin>(endpoint, book);
	}

private:
	std::shared_ptr<PeerProgressBook> book;
};

/* ------------------------------------------------------------------------- */
/* ASN prefix lookup (RIPEstat)                                               */
/* ------------------------------------------------------------------------- */

/*
 * One blocking JSON GET, same pattern as fetchIpInfoText. Runs on a worker
 * thread, never on the GUI thread.
 */
static QJsonObject fetchJsonObject(const QUrl &url, int timeout_ms, QString *error)
{
	QNetworkAccessManager manager;
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, "libtorrent-peer-collector-qt/1.0");

	QNetworkReply *reply = manager.get(request);
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);

	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

	timeout.start(timeout_ms);
	loop.exec();

	if (!timeout.isActive()) {
		reply->abort();
		reply->deleteLater();
		if (error != nullptr) {
			*error = "timeout";
		}
		return QJsonObject();
	}

	timeout.stop();

	QByteArray data = reply->readAll();
	QNetworkReply::NetworkError net_error = reply->error();
	QString error_string = reply->errorString();
	reply->deleteLater();

	if (net_error != QNetworkReply::NoError) {
		if (error != nullptr) {
			*error = error_string;
		}
		return QJsonObject();
	}

	QJsonParseError parse_error;
	QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);

	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		if (error != nullptr) {
			*error = "bad JSON";
		}
		return QJsonObject();
	}

	return doc.object();
}

static bool ipv4PrefixSortKey(const QString &prefix, quint32 *ip_out, int *len_out)
{
	int slash = prefix.indexOf('/');
	quint32 ip;
	quint16 dummy_port;
	bool ok;
	int len;

	if (slash <= 0) {
		return false;
	}

	if (!ipv4PortToSortKey(prefix.left(slash) + ":0", &ip, &dummy_port)) {
		return false;
	}

	len = prefix.mid(slash + 1).toInt(&ok);
	if (!ok || len < 0 || len > 32) {
		return false;
	}

	*ip_out = ip;
	*len_out = len;

	return true;
}

/*
 * The prefix pool as text, in the format used for filters:
 *
 *   # AS8580, RU, Nizhniy Novgorod, MTS PJSC
 *   5.227.45.0/24        # AS8580, RU, Nizhniy Novgorod, MTS PJSC, 256
 *   5.227.64.0/19        # AS8580, RU, Nizhniy Novgorod, MTS PJSC, 8,192
 *
 * Header: AS, country, city, holder, empty ones left out. Each line: the
 * CIDR, an aligned # comment repeating the header, and the number of
 * addresses in the prefix with thousands separators.
 */
static QString formatAsnPrefixList(const QString &asn,
                                   const QString &country,
                                   const QString &city,
                                   const QString &holder,
                                   const QStringList &prefixes)
{
	QStringList label_parts;
	QString label;
	QStringList lines;
	QLocale en(QLocale::English, QLocale::UnitedStates);
	int column;

	label_parts << "AS" + asn;
	for (const QString &part : {country, city, holder}) {
		if (!part.trimmed().isEmpty()) {
			label_parts << part.trimmed();
		}
	}
	label = label_parts.join(", ");

	lines << "# " + label;

	column = calculatePeerCommentColumn(prefixes);

	for (const QString &prefix : prefixes) {
		quint32 ip = 0;
		int len = 0;
		QString count;

		if (ipv4PrefixSortKey(prefix, &ip, &len)) {
			count = en.toString((qlonglong)1 << (32 - len));
		}

		lines << formatPeerLineWithComment(prefix, label + (count.isEmpty() ? QString() : ", " + count), column);
	}

	return lines.join("\n") + "\n";
}

/*
 * Resolve an IP to its origin AS and fetch every IPv4 prefix that AS
 * announces, using RIPEstat's public Data API (no key, no login):
 *
 *   network-info        IP -> asns[]
 *   as-overview         AS -> holder (organisation name)
 *   rir-stats-country   AS -> country, only when the caller has none
 *   announced-prefixes  AS -> prefixes[] as seen in BGP (RIS)
 *
 * Country and city normally come from the peer's IPinfo record (the table
 * cells), passed in by the caller. Short requests on a thread of their own;
 * the result is one signal.
 */
class AsnPrefixThread : public QThread {
	Q_OBJECT

public:
	/*
	 * country, city and holder are hints from the peer's IPinfo cells and
	 * may be empty. A given holder wins over RIPEstat's, so the label reads
	 * "MTS PJSC" as IPinfo names it rather than RIPEstat's "SANDY MTS PJSC".
	 */
	AsnPrefixThread(const QString &ip,
	                const QString &country,
	                const QString &city,
	                const QString &holder_hint,
	                QObject *parent = nullptr)
	    : QThread(parent),
	      ip(ip),
	      country(country),
	      city(city),
	      holderHint(holder_hint)
	{
	}

signals:
	void done(const QString &ip,
	          const QString &asn,
	          const QString &holder,
	          const QString &country,
	          const QString &city,
	          const QStringList &prefixes,
	          const QString &error);

protected:
	void run() override
	{
		const QString base = "https://stat.ripe.net/data/";
		const QString app = "&sourceapp=libtorrent-peer-collector-qt";
		QString error;
		QString asn;
		QString holder;
		QString found_country = country;
		QStringList prefixes;
		QJsonObject obj;

		obj = fetchJsonObject(QUrl(base + "network-info/data.json?resource=" + ip + app), 15000, &error);
		if (obj.isEmpty()) {
			emit done(ip, QString(), QString(), country, city, QStringList(), "network-info: " + error);
			return;
		}

		{
			QJsonArray asns = obj.value("data").toObject().value("asns").toArray();

			if (asns.isEmpty()) {
				emit done(ip, QString(), QString(), country, city, QStringList(), "RIPEstat knows no origin AS for " + ip);
				return;
			}

			asn = asns.first().toVariant().toString();
		}

		holder = holderHint.trimmed();

		if (holder.isEmpty()) {
			obj = fetchJsonObject(QUrl(base + "as-overview/data.json?resource=AS" + asn + app), 15000, &error);
			if (!obj.isEmpty()) {
				holder = obj.value("data").toObject().value("holder").toString();
			}
		}

		if (found_country.trimmed().isEmpty()) {
			obj = fetchJsonObject(QUrl(base + "rir-stats-country/data.json?resource=AS" + asn + app), 15000, &error);
			if (!obj.isEmpty()) {
				QJsonArray located = obj.value("data").toObject().value("located_resources").toArray();

				if (!located.isEmpty()) {
					found_country = located.first().toObject().value("location").toString();
				}
			}
		}

		obj = fetchJsonObject(QUrl(base + "announced-prefixes/data.json?resource=AS" + asn + app), 30000, &error);
		if (obj.isEmpty()) {
			emit done(ip, asn, holder, found_country, city, QStringList(), "announced-prefixes: " + error);
			return;
		}

		for (const QJsonValue &v : obj.value("data").toObject().value("prefixes").toArray()) {
			QString prefix = v.toObject().value("prefix").toString();

			if (!prefix.isEmpty() && !prefix.contains(':')) {
				prefixes << prefix;
			}
		}

		prefixes.removeDuplicates();

		std::sort(prefixes.begin(), prefixes.end(), [](const QString &a, const QString &b) {
			quint32 aip = 0, bip = 0;
			int alen = 0, blen = 0;

			if (!ipv4PrefixSortKey(a, &aip, &alen) || !ipv4PrefixSortKey(b, &bip, &blen)) {
				return a < b;
			}

			return aip != bip ? aip < bip : alen < blen;
		});

		emit done(ip, asn, holder, found_country, city, prefixes, QString());
	}

private:
	QString ip;
	QString country;
	QString city;
	QString holderHint;
};

/* ------------------------------------------------------------------------- */
/* Alert classification                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Map a libtorrent alert to a peer endpoint and a source tag by alert type.
 *
 * Returns true when the alert is a peer-level type this collector understands.
 * In that case *endpoint is the IPv4:port text, or empty for IPv6 peers, and
 * *source is the tag to record for it.
 *
 * Tags:
 *   peer-connect-in      incoming connection completed handshake
 *   peer-connect-out     outgoing connection completed handshake
 *   peer-connect-failed  outgoing connection attempt failed (never connected)
 *   peer-disconnected    a connected peer went away, any reason
 *   peer-incoming        raw incoming TCP/uTP connection, before handshake
 *   peer-error           protocol error from a peer
 *   peer-banned          peer was banned
 *   peer-blocked         peer rejected by IP filter/port filter/etc.
 */
static bool classifyPeerAlert(lt::alert *a, QString *endpoint, QString *source)
{
	if (endpoint == nullptr || source == nullptr) {
		return false;
	}

	if (auto *pc = lt::alert_cast<lt::peer_connect_alert>(a)) {
		*endpoint = ipPortToText(pc->endpoint);
		*source = (pc->direction == lt::peer_connect_alert::direction_t::in)
		              ? "peer-connect-in"
		              : "peer-connect-out";
		return true;
	}

	if (auto *pd = lt::alert_cast<lt::peer_disconnected_alert>(a)) {
		*endpoint = ipPortToText(pd->endpoint);

		if (pd->op == lt::operation_t::connect) {
			*source = "peer-connect-failed";
		} else {
			*source = "peer-disconnected";
		}
		return true;
	}

	if (auto *ic = lt::alert_cast<lt::incoming_connection_alert>(a)) {
		*endpoint = ipPortToText(ic->endpoint);
		*source = "peer-incoming";
		return true;
	}

	if (auto *pe = lt::alert_cast<lt::peer_error_alert>(a)) {
		*endpoint = ipPortToText(pe->endpoint);
		*source = "peer-error";
		return true;
	}

	if (auto *pb = lt::alert_cast<lt::peer_ban_alert>(a)) {
		*endpoint = ipPortToText(pb->endpoint);
		*source = "peer-banned";
		return true;
	}

	if (auto *pk = lt::alert_cast<lt::peer_blocked_alert>(a)) {
		*endpoint = ipPortToText(pk->endpoint);
		*source = "peer-blocked";
		return true;
	}

	return false;
}

/*
 * Decide which alerts go to the Log tab.
 *
 * Connect/disconnect alerts are far too frequent to log one by one. Their
 * effect is visible in the peer list and summary instead.
 */
static bool shouldLogAlert(lt::alert *a)
{
	lt::alert_category_t cat = a->category();

	/*
	 * DHT traffic alerts arrive several times a second once the table is
	 * warm and say nothing about this torrent's peers: get_peers queries
	 * from and to other nodes, and announces for other info hashes. Replies
	 * with peers, bootstrap and errors still go through.
	 */
	if (lt::alert_cast<lt::dht_get_peers_alert>(a) != nullptr ||
	    lt::alert_cast<lt::dht_outgoing_get_peers_alert>(a) != nullptr ||
	    lt::alert_cast<lt::dht_announce_alert>(a) != nullptr) {
		return false;
	}

	/*
	 * libtorrent announces once per listen socket, including the loopback
	 * one, which can never reach a tracker. Its "sending announce" and
	 * "skipping tracker announce (unreachable)" lines say nothing useful.
	 * alert_cast is exact-type only, so the base class needs dynamic_cast.
	 */
	if (auto *ta = dynamic_cast<lt::tracker_alert *>(a)) {
		if (ta->local_endpoint.address().is_loopback()) {
			return false;
		}
	}

	if (cat & (lt::alert_category::error |
	           lt::alert_category::tracker |
	           lt::alert_category::dht |
	           lt::alert_category::status)) {
		return true;
	}

	if (lt::alert_cast<lt::peer_error_alert>(a) != nullptr ||
	    lt::alert_cast<lt::peer_ban_alert>(a) != nullptr ||
	    lt::alert_cast<lt::peer_blocked_alert>(a) != nullptr) {
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

class PeerCollectorThread : public QThread {
	Q_OBJECT

public:
	PeerCollectorThread(const QString &torrent_path,
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
	                    bool include_ipinfo_comments,
	                    const QString &ipinfo_token,
	                    const QStringList &extra_trackers,
	                    QObject *parent = nullptr)
	    : QThread(parent),
	      torrentPath(torrent_path),
	      extraTrackers(extra_trackers),
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
	void peerCountChanged(int count, int active);
	void peersChanged(const PeerRows &rows);
	void progressChanged(int elapsed, int total);
	void finishedStatus(bool ok, const QString &message);

protected:
	void run() override
	{
		std::set<QString> peers;
		std::map<QString, std::set<QString>> peerSources;
		std::map<QString, QString> ipInfoCache;

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
			emit logMessage("Save path   : " + tempDir.path());
			emit logMessage("Run time    : " + QString::number(runTimeSeconds) + " seconds");
			emit logMessage("Peer poll   : " + QString::number(pollIntervalMs / 1000.0, 'f', 1) + " seconds");
			emit logMessage("Tracker int.: " + QString::number(trackerReannounceMs / 1000.0, 'f', 0) + " seconds");
			emit logMessage("DHT int.    : " + QString::number(dhtReannounceMs / 1000.0, 'f', 0) + " seconds");
			emit logMessage("Listen port : " + QString::number(listenPort));
			emit logMessage("Download    : " + QString(noDownload ? "minimal (one piece wanted at a time, 1 KB/s cap)" : "normal"));
			emit logMessage("Trackers    : " + QString(enableTrackers ? "enabled" : "disabled"));
			emit logMessage("Extra trk.  : " + QString::number(extraTrackers.size()) + " configured");
			emit logMessage("DHT         : " + QString(enableDht ? "enabled" : "disabled"));
			emit logMessage("PEX         : " + QString(enablePex ? "enabled (ut_pex plugin), needs a connected peer" : "disabled (ut_pex plugin not loaded)"));
			emit logMessage("LSD         : " + QString(enableLsd ? "enabled" : "disabled"));
			emit logMessage("IPinfo      : " + QString(includeIpInfoComments ? "lookups enabled" : "lookups disabled"));
			emit logMessage("");

			/* One identity for libtorrent and for the direct announce. */
			QByteArray runPeerId = makePeerId();

			lt::settings_pack pack;
			pack.set_str(lt::settings_pack::peer_fingerprint, runPeerId.toStdString());
			pack.set_bool(lt::settings_pack::enable_dht, enableDht);
			pack.set_bool(lt::settings_pack::enable_lsd, enableLsd);
			pack.set_bool(lt::settings_pack::enable_upnp, true);
			pack.set_bool(lt::settings_pack::enable_natpmp, true);

			/*
			 * Alert mask.
			 *
			 * libtorrent 2.0 defaults alert_mask to error only.
			 *
			 *   connect  : peer_connect_alert, peer_disconnected_alert.
			 *              These carry the endpoint of every peer libtorrent
			 *              tries, including ones that never connect.
			 *   peer     : incoming_connection_alert, peer_error_alert,
			 *              peer_ban_alert, snubbed/unsnubbed.
			 *   ip_block : peer_blocked_alert.
			 *   tracker/dht/status/error : for the log only. Their messages
			 *              carry URLs and counts, not peer endpoints.
			 */
			pack.set_int(
			    lt::settings_pack::alert_mask,
			    static_cast<int>(
			        lt::alert_category::error |
			        lt::alert_category::piece_progress |
			        lt::alert_category::connect |
			        lt::alert_category::peer |
			        lt::alert_category::ip_block |
			        lt::alert_category::tracker |
			        lt::alert_category::dht |
			        lt::alert_category::status));

			pack.set_str(
			    lt::settings_pack::listen_interfaces,
			    QString("0.0.0.0:%1").arg(listenPort).toStdString());

			/*
			 * On shutdown libtorrent waits for its own event=stopped announces,
			 * 5 s by default. That is what the window close and a quick
			 * restart on the same port wait for; 2 s is plenty for live
			 * trackers and dead ones are not worth waiting for.
			 */
			pack.set_int(lt::settings_pack::stop_tracker_timeout, 2);

			/*
			 * With nothing to download the torrent counts as finished, and
			 * libtorrent then drops seeds as redundant before reading their
			 * bitfield. Keep them so the "Have" column can be filled.
			 */
			pack.set_bool(lt::settings_pack::close_redundant_connections, false);

			pack.set_str(
			    lt::settings_pack::dht_bootstrap_nodes,
			    "router.bittorrent.com:6881,"
			    "router.utorrent.com:6881,"
			    "dht.transmissionbt.com:6881");

#ifdef LIBTORRENT_VERSION_NUM
			emit logMessage("libtorrent  : " + QString::fromLatin1(LIBTORRENT_VERSION));
#endif

			/*
			 * PEX is not a settings_pack key in libtorrent 2.0. It is the
			 * ut_pex torrent plugin, added by default together with
			 * ut_metadata and smart_ban. Build the session without the
			 * default plugins and attach them per torrent, leaving ut_pex
			 * out when the user unchecked PEX.
			 */
			lt::session ses(lt::session_params(pack), lt::session_flags_t{});

			auto ti = std::make_shared<lt::torrent_info>(torrentPath.toStdString());

			/*
			 * Extra trackers go into the torrent_info itself, in a tier after
			 * the torrent's own, so both libtorrent's announces and the direct
			 * announce see them.
			 */
			if (enableTrackers && !extraTrackers.isEmpty()) {
				std::set<std::string> existing;
				int added_trackers = 0;

				for (const lt::announce_entry &ae : ti->trackers()) {
					existing.insert(ae.url);
				}

				for (const QString &t : extraTrackers) {
					std::string url = t.trimmed().toStdString();

					if (url.empty() || existing.find(url) != existing.end()) {
						continue;
					}

					ti->add_tracker(url, 1);
					existing.insert(url);
					added_trackers++;
				}

				emit logMessage("Extra trackers added to torrent: " + QString::number(added_trackers)
				                + " (torrent now has " + QString::number(ti->trackers().size()) + ")");
			}

			lt::add_torrent_params params;
			params.ti = ti;
			params.save_path = tempDir.path().toStdString();
			params.storage_mode = lt::storage_mode_t::storage_mode_sparse;
			params.extensions.push_back(&lt::create_ut_metadata_plugin);
			params.extensions.push_back(&lt::create_smart_ban_plugin);

			auto progressBook = std::make_shared<PeerProgressBook>();
			progressBook->num_pieces = ti->num_pieces();
			params.extensions.push_back([progressBook](lt::torrent_handle const &, lt::client_data_t) {
				return std::static_pointer_cast<lt::torrent_plugin>(std::make_shared<ProgressTorrentPlugin>(progressBook));
			});

			if (enablePex) {
				params.extensions.push_back(&lt::create_ut_pex_plugin);
			}

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

			/*
			 * Minimal download. Marking every piece do-not-download makes the
			 * torrent "finished", and libtorrent opens no outgoing connections
			 * for a finished torrent: real peers are then never contacted and
			 * never report what they have. So exactly one piece stays wanted,
			 * at a 1 KB/s cap, and whenever it completes the next one takes
			 * its place. Peers connect, send their bitfield, and the transfer
			 * stays around 3.6 MB per hour at most.
			 */
			int wantedPiece = -1;

			if (noDownload) {
				try {
					std::vector<lt::download_priority_t> piece_prios(
					    (std::size_t)ti->num_pieces(),
					    lt::dont_download);

					wantedPiece = 0;
					piece_prios[(std::size_t)wantedPiece] = lt::low_priority;
					h.prioritize_pieces(piece_prios);

					lt::settings_pack throttle;
					throttle.set_int(lt::settings_pack::download_rate_limit, 1024);
					ses.apply_settings(throttle);
				} catch (...) {
					emit logMessage("Could not set piece priorities for minimal download.");
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

			/*
			 * The direct HTTP tracker announce happens in the first poll
			 * iteration below (first_poll), together with the forced
			 * libtorrent reannounce. Not here as well.
			 */

			emit logMessage("Waiting for peers...");
			emit logMessage("Peer list includes connected peers, IPv4:port endpoints found in libtorrent alerts,");
			emit logMessage("and direct compact IPv4 peers returned by HTTP, HTTPS and UDP trackers.");
			emit logMessage("Output comments auto-align: # column = longest IP:port length + 5 spaces.");
			if (includeIpInfoComments) {
				emit logMessage("IPinfo lookups run in the background and are cached. Same IP is looked up only once.");
			}
			emit logMessage("Direct announces go to HTTP, HTTPS and UDP (BEP 15) trackers with the same peer id libtorrent uses: "
			                + QString::fromLatin1(runPeerId) + ".");
			emit logMessage("");

			DirectAnnounceContext announceCtx;
			announceCtx.peer_id = runPeerId;
			announceCtx.key = QRandomGenerator::global()->generate();

			std::set<QString> selfIps = localInterfaceIpv4Addresses();
			std::set<QString> selfExcludedLogged;
			std::set<QString> activePeers;    /* ever connected during this run; never cleared */
			std::map<QString, int> peerProgressPpm;  /* last reported share of the torrent, per peer */
			int last_active = -1;

			/*
			 * The direct announce round runs on its own thread so a slow or
			 * dead tracker never stalls polling. The round writes only into
			 * these round-local containers and announceCtx; the poll loop
			 * merges them once the thread has finished. announceCancel is
			 * set by Stop and by the natural end of the run.
			 */
			std::unique_ptr<QThread> announceRound;
			std::set<QString> roundPeers;
			std::map<QString, std::set<QString>> roundSources;
			QStringList roundLogs;
			std::atomic<bool> announceCancel(false);

			/*
			 * Whatever way run() leaves this scope, a round still in flight
			 * is cancelled and joined before the containers it writes to are
			 * destroyed. Declared after them so it is destroyed first.
			 */
			struct AnnounceRoundGuard {
				std::unique_ptr<QThread> &thread;
				std::atomic<bool> &cancel;

				~AnnounceRoundGuard()
				{
					if (thread) {
						cancel.store(true);
						thread->wait();
					}
				}
			} announceRoundGuard{announceRound, announceCancel};

			auto finishAnnounceRound = [&](bool wait_for_it) {
				int added = 0;

				if (!announceRound) {
					return;
				}

				if (wait_for_it) {
					announceRound->wait();
				} else if (!announceRound->isFinished()) {
					return;
				}

				announceRound->wait();
				announceRound.reset();

				for (const QString &line : roundLogs) {
					emit logMessage(line);
				}

				for (const QString &p : roundPeers) {
					if (peers.insert(p).second) {
						added++;
					}
				}

				for (const auto &item : roundSources) {
					peerSources[item.first].insert(item.second.begin(), item.second.end());
				}

				if (added > 0) {
					emit logMessage("Direct tracker announce added peers: " + QString::number(added));
				}
			};

			std::unique_ptr<IpInfoLookupThread> ipInfoLookup;

			if (includeIpInfoComments) {
				ipInfoLookup.reset(new IpInfoLookupThread(ipInfoToken));
				ipInfoLookup->start();
			}

			QElapsedTimer timer;
			timer.start();

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
					/* Sleep in slices so Stop is noticed within 100 ms. */
					QElapsedTimer sleep_timer;

					sleep_timer.start();

					while (!stopRequested.load() && sleep_timer.elapsed() < pollIntervalMs) {
						qint64 left_ms = pollIntervalMs - sleep_timer.elapsed();

						msleep((unsigned long)(left_ms < 100 ? left_ms : 100));
					}
				}

				elapsed = (int)(timer.elapsed() / 1000);
				if (elapsed >= runTimeSeconds) {
					break;
				}

				emit progressChanged(elapsed, runTimeSeconds);

				if (first_poll) {
					emit logMessage("Immediate first peer poll...");
				}

				/* Merge a finished announce round, if any, without waiting. */
				finishAnnounceRound(false);

				if (enableTrackers && !announceRound &&
				    (first_poll || timer.elapsed() - last_tracker_reannounce_ms >= trackerReannounceMs)) {
					try {
						h.force_reannounce();
						emit logMessage(first_poll ? "Initial tracker reannounce." : "Forced tracker reannounce.");
					} catch (...) {
					}

					roundPeers.clear();
					roundSources.clear();
					roundLogs.clear();

					announceRound.reset(QThread::create([&]() {
						directAnnounceAllTrackers(
						    ti,
						    listenPort,
						    announceCtx,
						    roundPeers,
						    roundSources,
						    &announceCancel,
						    &roundLogs);
					}));
					announceRound->start();

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
						QString endpoint;
						QString source;

						if (auto *ext = lt::alert_cast<lt::external_ip_alert>(a)) {
							if (ext->external_address.is_v4()) {
								selfIps.insert(QString::fromStdString(ext->external_address.to_string()));
							}
						}

						if (auto *pf = lt::alert_cast<lt::piece_finished_alert>(a)) {
							if (wantedPiece >= 0 && static_cast<int>(pf->piece_index) == wantedPiece) {
								try {
									wantedPiece = (wantedPiece + 1) % ti->num_pieces();
									h.piece_priority(lt::piece_index_t(wantedPiece), lt::low_priority);
								} catch (...) {
								}
							}
						}

						if (classifyPeerAlert(a, &endpoint, &source)) {
							/*
							 * Known peer-level alert type. Endpoint is empty
							 * for IPv6 peers, which this tool skips.
							 */
							if (!endpoint.isEmpty()) {
								peers.insert(endpoint);
								peerSources[endpoint].insert(source);
							}
						} else if (a->category() & lt::alert_category::peer) {
							/*
							 * Other peer-category alert (snubbed, invalid
							 * request, ...). Scrape the message text. Only
							 * peer-category alerts are scraped so tracker
							 * URLs with raw IPs are never mistaken for peers.
							 */
							QString s = QString::fromStdString(a->message());
							QStringList alert_peers = extractIpv4PortsFromText(s);

							for (const QString &p : alert_peers) {
								peers.insert(p);
								peerSources[p].insert("peer-alert");
							}
						}

						if (shouldLogAlert(a)) {
							emit logMessage(QString::fromStdString(a->message()));
						}
					}
				} catch (...) {
				}

				try {
					std::vector<lt::peer_info> info;
					h.get_peer_info(info);

					for (const lt::peer_info &pi : info) {
						QString p = ipPortToText(pi.ip);
						bool established;
						int progress;

						if (p.isEmpty()) {
							continue;
						}

						/*
						 * get_peer_info also lists connections still being
						 * opened or handshaking. Those are addresses we tried,
						 * not peers we talked to: tag them, but count as
						 * connected only once the handshake is done.
						 */
						established = !(pi.flags & (lt::peer_info::connecting | lt::peer_info::handshake));

						peers.insert(p);
						peerSources[p].insert(established ? "connected/get_peer_info" : "connecting");

						if (!established) {
							continue;
						}

						activePeers.insert(p);

						/* Keep the best value seen; a seed flag means all of it. */
						progress = (pi.flags & lt::peer_info::seed) ? 1000000 : pi.progress_ppm;

						auto pit = peerProgressPpm.find(p);
						if (pit == peerProgressPpm.end() || progress > pit->second) {
							peerProgressPpm[p] = progress;
						}
					}
				} catch (...) {
				}

				/* What the plugin saw since the last poll: every handshake and bitfield. */
				{
					QMutexLocker lock(&progressBook->mutex);

					for (const QString &p : progressBook->handshaked) {
						peers.insert(p);
						peerSources[p].insert("connected/handshake");
						activePeers.insert(p);
					}

					for (const auto &item : progressBook->best_ppm) {
						auto pit = peerProgressPpm.find(item.first);
						if (pit == peerProgressPpm.end() || item.second > pit->second) {
							peerProgressPpm[item.first] = item.second;
						}
					}
				}

				for (const QString &p : pruneSelfPeers(peers, peerSources, selfIps)) {
					if (selfExcludedLogged.insert(p).second) {
						emit logMessage("Excluded own address from peer list: " + p);
					}
				}

				if ((int)peers.size() != last_count || (int)activePeers.size() != last_active || first_poll) {
					last_count = (int)peers.size();
					last_active = (int)activePeers.size();
					emit logMessage("Seen/known peers: " + QString::number(last_count)
					                + ", connected so far: " + QString::number(last_active));
					emit peerCountChanged(last_count, last_active);
				}

				if (ipInfoLookup) {
					ipInfoLookup->enqueuePeers(peers);

					for (const QString &line : ipInfoLookup->takeLogLines()) {
						emit logMessage(line);
					}

					ipInfoCache = ipInfoLookup->snapshot();
				}

				if (first_poll || timer.elapsed() - last_preview_ms >= 3000) {
					emit peersChanged(buildPeerRows(peers, peerSources, ipInfoCache, activePeers, peerProgressPpm, includeIpInfoComments));
					last_preview_ms = timer.elapsed();
				}

				first_poll = false;
			}

			QElapsedTimer shutdown_timer;

			shutdown_timer.start();

			if (stopRequested.load()) {
				emit logMessage("Stop requested by user.");
			}

			/* A round still in flight ends within 200 ms once cancelled. */
			announceCancel.store(true);
			finishAnnounceRound(true);

			if (enableTrackers) {
				QStringList stop_logs;

				directStopAllTrackers(ti, listenPort, announceCtx, &stop_logs);

				for (const QString &line : stop_logs) {
					emit logMessage(line);
				}
			}

			if (ipInfoLookup) {
				int pending;

				ipInfoLookup->enqueuePeers(peers);
				pending = ipInfoLookup->pendingCount();

				/*
				 * A normal end of run waits for the queue to drain so the
				 * file is complete. A user Stop does not: the remaining
				 * peers are written with ipinfo=pending.
				 */
				if (pending > 0 && !stopRequested.load()) {
					emit logMessage("Waiting for " + QString::number(pending) + " remaining IPinfo lookups...");

					while (!stopRequested.load() && ipInfoLookup->pendingCount() > 0) {
						msleep(200);

						for (const QString &line : ipInfoLookup->takeLogLines()) {
							emit logMessage(line);
						}
					}
				}

				pending = ipInfoLookup->pendingCount();
				if (pending > 0) {
					emit logMessage("Stop requested: " + QString::number(pending) + " IPinfo lookups skipped.");
				}

				ipInfoLookup->requestStop();
				ipInfoLookup->wait();

				for (const QString &line : ipInfoLookup->takeLogLines()) {
					emit logMessage(line);
				}

				ipInfoCache = ipInfoLookup->snapshot();
			}

			pruneSelfPeers(peers, peerSources, selfIps);

			emit peersChanged(buildPeerRows(peers, peerSources, ipInfoCache, activePeers, peerProgressPpm, includeIpInfoComments));
			emit progressChanged(runTimeSeconds, runTimeSeconds);

			emit logMessage("");
			emit logMessage(buildPeerSummaryText(peers, peerSources));
			emit logMessage("Done. Unique seen/known peers: " + QString::number(peers.size()));
			emit logMessage("Wrap-up after the poll loop took " + QString::number(shutdown_timer.elapsed()) + " ms.");
			emit logMessage("Copy Peers puts the list on the clipboard, Save Peers... writes it to a file.");
			emit logMessage("");
			emit logMessage("Note:");
			emit logMessage("  PEX peers are only discovered after connecting to peers.");
			emit logMessage("  Running longer usually finds more peers.");

			emit finishedStatus(true, "Done. Unique seen/known peers: " + QString::number(peers.size()));

		} catch (const std::exception &e) {
			/* Whatever was collected stays visible in the peers tab; nothing is connected any more. */
			emit peersChanged(buildPeerRows(peers, peerSources, ipInfoCache, std::set<QString>(), std::map<QString, int>(), includeIpInfoComments));
			emit finishedStatus(false, "Exception: " + QString::fromUtf8(e.what()));
		} catch (...) {
			emit peersChanged(buildPeerRows(peers, peerSources, ipInfoCache, std::set<QString>(), std::map<QString, int>(), includeIpInfoComments));
			emit finishedStatus(false, "Unknown exception.");
		}
	}

private:
	QString torrentPath;
	QStringList extraTrackers;
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
	      settings(CONFIG_FOLDER_NAME, APP_NAME),
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

		/* F1 is the usual help key. It does the same as the Help button. */
		QShortcut *help_shortcut;

		help_shortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
		connect(help_shortcut, &QShortcut::activated, this, [this]() {
			showHelpDialog();
		});

		loadSettings();
	}

	~MainWindow() override
	{
		saveSettings();

		if (!worker.isNull()) {
			worker->requestStop();
			worker->wait(15000);
		}

		for (const QPointer<AsnPrefixThread> &t : asnThreads) {
			if (!t.isNull()) {
				t->wait();
			}
		}
	}

protected:
	/*
	 * A click anywhere in this window outside the peers table clears the
	 * table's selection. Exceptions: the Copy Selected button, which needs
	 * the selection it is about to copy, and anything that is not part of
	 * this window (menus, dialogs), so the right-click menu still acts on
	 * the selected rows. The event is only observed, never consumed.
	 */
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::MouseButtonPress && peersTable != nullptr) {
			QWidget *w = qobject_cast<QWidget *>(obj);

			if (w != nullptr &&
			    w->window() == this &&
			    !peersTable->isAncestorOf(w) && w != peersTable &&
			    w != copySelectedButton &&
			    peersTable->selectionModel()->hasSelection()) {
				peersTable->clearSelection();
			}
		}

		return QMainWindow::eventFilter(obj, event);
	}

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
		}

		/*
		 * The question box ran a nested event loop; the worker may have
		 * finished meanwhile and collectionFinished may have cleared the
		 * pointer, so check again. Then cut the worker off from this window
		 * before stopping it: its final signals are queued, and delivering
		 * them after the window is hidden would open a modal "Done" box on
		 * an invisible parent, which keeps the process alive with nothing on
		 * screen. Stop is honoured within a second or two, so waiting is
		 * cheap and leaves no thread behind.
		 */
		closing = true;

		if (!worker.isNull()) {
			disconnect(worker, nullptr, this, nullptr);
			worker->requestStop();

			if (!worker->wait(15000)) {
				/*
				 * Still running. It is a child of this window, and a QThread
				 * must not be destroyed while running, so reparent it and let
				 * it delete itself when it finishes (deleteLater is still
				 * connected). The destructor below then has nothing to wait
				 * for and the process exits as soon as the thread does.
				 */
				worker->setParent(nullptr);
			}
		}

		event->accept();
	}

private:
	QSettings settings;

	QLineEdit *torrentPathEdit = nullptr;
	QString lastPeersDir;    /* folder of the last Save Peers */
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
	QCheckBox *ipInfoLookupCheck = nullptr;
	QCheckBox *exportSourcesCheck = nullptr;
	QCheckBox *exportIpInfoCheck = nullptr;
	QLineEdit *ipInfoTokenEdit = nullptr;
	QPushButton *startButton = nullptr;
	QPushButton *stopButton = nullptr;
	QPushButton *helpButton = nullptr;
	QPushButton *copySelectedButton = nullptr;

	/*
	 * The help window is not modal, so the collector can be driven while it
	 * is open. Only one is ever built: a second Help click raises this one.
	 */
	QPointer<QDialog> helpDialog;

	/* ASN lookups in flight; joined before the window goes away. */
	QList<QPointer<AsnPrefixThread>> asnThreads;

	/* Set in closeEvent: no dialogs from that point on. */
	bool closing = false;

	QPlainTextEdit *logText = nullptr;
	QTableView *peersTable = nullptr;
	QStandardItemModel *peersModel = nullptr;
	QSortFilterProxyModel *peersProxy = nullptr;
	QPlainTextEdit *extraTrackersEdit = nullptr;

	enum PeerColumn {
		COL_IP = 0,
		COL_PORT,
		COL_ACTIVE,
		COL_SOURCES,
		COL_COUNTRY,
		COL_CITY,
		COL_PROVIDER,
		COL_HOST,
		COL_COUNT
	};
	QTabWidget *tabs = nullptr;
	QProgressBar *progress = nullptr;
	QLabel *progressTextLabel = nullptr;
	QLabel *statusLabel = nullptr;

	/*
	 * QPointer: the worker deletes itself (deleteLater on finished) and that
	 * can run while the window is still alive, for instance during the
	 * event loop turns between closeEvent and the destructor. A raw pointer
	 * then dangled and the destructor's stop/wait ran on freed memory.
	 */
	QPointer<PeerCollectorThread> worker;

	void createFileGroup(QVBoxLayout *parent)
	{
		QGroupBox *group;
		QGridLayout *grid;
		QPushButton *browse_torrent;

		group = new QGroupBox("Torrent", this);
		grid = new QGridLayout(group);

		torrentPathEdit = new QLineEdit(group);
		browse_torrent = new QPushButton("Browse .torrent...", group);

		grid->addWidget(new QLabel("Torrent file:", group), 0, 0);
		grid->addWidget(torrentPathEdit, 0, 1);
		grid->addWidget(browse_torrent, 0, 2);

		parent->addWidget(group);

		connect(browse_torrent, &QPushButton::clicked, this, &MainWindow::browseTorrent);
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
		listenPortSpin->setRange(1, 65535);
		listenPortSpin->setValue(6881);
		listenPortSpin->setToolTip("Ports below 1024 usually need root.");

		noDownloadCheck = new QCheckBox("Minimal download: one piece at a time, 1 KB/s, so peers still connect and report what they have", group);
		noDownloadCheck->setChecked(true);

		trackersCheck = new QCheckBox("Trackers", group);
		trackersCheck->setChecked(true);

		dhtCheck = new QCheckBox("DHT", group);
		dhtCheck->setChecked(true);

		pexCheck = new QCheckBox("PEX / Peer Exchange (ut_pex plugin)", group);
		pexCheck->setChecked(true);

		lsdCheck = new QCheckBox("LSD / Local peer discovery", group);
		lsdCheck->setChecked(true);

		ipInfoLookupCheck = new QCheckBox("Look up country, city, provider and host for each peer at ipinfo.io", group);
		ipInfoLookupCheck->setChecked(false);

		ipInfoTokenEdit = new QLineEdit(group);
		ipInfoTokenEdit->setPlaceholderText("Optional IPinfo token, not saved");
		ipInfoTokenEdit->setEchoMode(QLineEdit::Password);
		ipInfoTokenEdit->setToolTip("Optional. Leave blank to try IPinfo without a token. Token is not saved.");

		extraTrackersEdit = new QPlainTextEdit(group);
		extraTrackersEdit->setPlaceholderText("One tracker URL per line. Added to every torrent, after its own trackers.");
		extraTrackersEdit->setToolTip(
		    "Trackers added to every torrent you load, in a tier after the torrent's own.\n"
		    "Lines starting with # are ignored. Clear the box to add none.");
		extraTrackersEdit->setMaximumHeight(4 * extraTrackersEdit->fontMetrics().lineSpacing() + 12);
		extraTrackersEdit->setLineWrapMode(QPlainTextEdit::NoWrap);

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
		form->addRow("Extra trackers:", extraTrackersEdit);
		form->addRow("IPinfo:", ipInfoLookupCheck);
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
		QPushButton *save_peers;
		QPushButton *clear_log;

		row = new QHBoxLayout();

		startButton = new QPushButton("Start Collection", this);
		stopButton = new QPushButton("Stop", this);
		copy_peers = new QPushButton("Copy Peers", this);
		copySelectedButton = new QPushButton("Copy Selected", this);
		copySelectedButton->setEnabled(false);
		copySelectedButton->setToolTip("Copy the selected rows of the peers table (Ctrl+C in the table).");
		save_peers = new QPushButton("Save Peers...", this);
		clear_log = new QPushButton("Clear Log", this);
		helpButton = new QPushButton("Help", this);

		startButton->setStyleSheet("font-weight: bold;");
		stopButton->setEnabled(false);

		row->addWidget(startButton);
		row->addWidget(stopButton);
		row->addWidget(copy_peers);
		row->addWidget(copySelectedButton);
		row->addWidget(save_peers);
		row->addWidget(clear_log);
		row->addWidget(helpButton);
		row->addSpacing(16);

		/*
		 * What Copy Peers, Copy Selected and Save Peers put after the
		 * endpoint. Separate from the IPinfo lookup switch in Options: that
		 * one decides whether data is fetched, these decide what is written.
		 */
		exportSourcesCheck = new QCheckBox("Sources", this);
		exportSourcesCheck->setChecked(true);
		exportSourcesCheck->setToolTip("Include the source tags in the # comment of copied and saved lines.");
		exportIpInfoCheck = new QCheckBox("IP info", this);
		exportIpInfoCheck->setChecked(true);
		exportIpInfoCheck->setToolTip("Include country, city, provider and host, as shown in the table, in the # comment.");

		row->addWidget(new QLabel("Copy/Save includes:", this));
		row->addWidget(exportSourcesCheck);
		row->addWidget(exportIpInfoCheck);
		row->addStretch(1);

		parent->addLayout(row);

		connect(startButton, &QPushButton::clicked, this, &MainWindow::startCollection);
		connect(stopButton, &QPushButton::clicked, this, &MainWindow::stopCollection);
		connect(copy_peers, &QPushButton::clicked, this, &MainWindow::copyPeers);
		connect(copySelectedButton, &QPushButton::clicked, this, &MainWindow::copySelectedPeers);
		connect(save_peers, &QPushButton::clicked, this, &MainWindow::savePeers);
		connect(clear_log, &QPushButton::clicked, this, [this]() {
			if (logText != nullptr) {
				logText->clear();
			}
		});
		connect(helpButton, &QPushButton::clicked, this, [this]() {
			showHelpDialog();
		});
	}

	/*
	 * One help page. The view is frameless because the tab widget already
	 * draws a border around it. The colours are pinned so a theme that changes
	 * the base colour without the text leaves the page readable.
	 */
	void addHelpPage(QTabWidget *pages, const QString &title, const QString &body)
	{
		QTextBrowser *view;

		view = new QTextBrowser(pages);
		view->setOpenExternalLinks(false);
		view->setFrameShape(QFrame::NoFrame);
		view->setStyleSheet("QTextBrowser {"
		                    "  background-color: #ffffff;"
		                    "  color: #000000;"
		                    "  selection-background-color: #cfe8ff;"
		                    "  selection-color: #000000;"
		                    "}");
		view->document()->setDefaultStyleSheet(
		    "body { color: #000000; }"
		    "h3 { color: #000000; }"
		    "p { color: #000000; }"
		    "li { color: #000000; }"
		    "td { color: #000000; }"
		    "b { color: #000000; }"
		    "i { color: #000000; }"
		    "tt { color: #000000; }"
		    "pre { color: #000000; background-color: #f4f4f4; }");
		view->setHtml("<body>" + body + "</body>");
		pages->addTab(view, title);
	}

	/*
	 * Help is a reference document, not a message: one tab per reason to open
	 * it, rich text, not modal, one instance, its size remembered.
	 */
	void showHelpDialog()
	{
		QDialog *dialog;
		QVBoxLayout *layout;
		QTabWidget *pages;
		QDialogButtonBox *buttons;
		QByteArray geometry;

		if (!helpDialog.isNull()) {
			helpDialog->show();
			helpDialog->raise();
			helpDialog->activateWindow();
			return;
		}

		dialog = new QDialog(this);
		dialog->setWindowTitle("Help");
		dialog->setAttribute(Qt::WA_DeleteOnClose);

		layout = new QVBoxLayout(dialog);
		pages = new QTabWidget(dialog);
		buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);

		addHelpPage(pages, "Overview",
		    "<h3>What the application does</h3>"
		    "<p>Libtorrent Peer Collector opens a <tt>.torrent</tt> file in a real "
		    "libtorrent session for a fixed time and records every IPv4 peer it "
		    "sees: peers returned by trackers, found through DHT, exchanged over "
		    "PEX, discovered on the local network, and every peer libtorrent "
		    "tries to connect to, whether the connection succeeds or not.</p>"
		    "<p>The result is the Seen / Known Peers table: IP, port, where each "
		    "peer was seen and, if enabled, IPinfo location and provider. Click "
		    "a header to sort. Nothing is written to disk on its own: "
		    "<b>Copy Peers</b> puts the list on the clipboard as text and "
		    "<b>Save Peers...</b> writes the same text to a file you pick, both "
		    "in the table's current order.</p>"
		    "<h3>The usual flow</h3>"
		    "<ol>"
		    "<li>Choose a <b>Torrent file</b>.</li>"
		    "<li>Set the <b>Run time</b>. Longer runs find more peers.</li>"
		    "<li>Press <b>Start Collection</b>.</li>"
		    "<li>Watch the <b>Log</b> tab for tracker and DHT activity and the "
		    "<b>Seen / Known Peers</b> tab for the growing list.</li>"
		    "<li>When it is done, or after Stop, press <b>Copy Peers</b> or "
		    "<b>Save Peers...</b>. The list in the tab is complete at that "
		    "point; it refreshes every 3 seconds while running.</li>"
		    "</ol>"
		    "<h3>While it runs</h3>"
		    "<p><b>Stop</b> interrupts whatever the collector is doing within a "
		    "fraction of a second: the poll sleep, a tracker announce in flight, "
		    "and the IPinfo queue. It then tells the trackers that answered "
		    "earlier that this peer is leaving, with short timeouts, and puts the "
		    "final list in the peers tab. "
		    "Pending IPinfo lookups are abandoned and the affected lines say "
		    "<tt>ipinfo=pending</tt>.</p>"
		    "<p>Torrent data is written to a temporary directory that is removed "
		    "when the run ends. With <b>Minimal download</b> "
		    "checked, only one piece is wanted at a time and the download is "
		    "capped at 1 KB/s, so peers connect and report what they have while "
		    "the transfer stays under about 3.6 MB per hour. Marking everything "
		    "do-not-download would make libtorrent treat the torrent as finished "
		    "and stop contacting peers at all.</p>");

		addHelpPage(pages, "Options",
		    "<h3>Timing</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Run time</b></td><td>How long the session stays open. "
		    "10 seconds to 24 hours.</td></tr>"
		    "<tr><td><b>Peer poll interval</b></td><td>How often connected peers "
		    "and libtorrent alerts are read. Minimum 0.2 seconds.</td></tr>"
		    "<tr><td><b>Tracker reannounce interval</b></td><td>How often trackers "
		    "are asked again, both through libtorrent and by this application's "
		    "own direct announce to every HTTP, HTTPS and UDP tracker. The direct "
		    "announce uses the very peer id libtorrent uses, so a tracker sees one "
		    "client, sends <i>started</i> "
		    "once per tracker and <i>stopped</i> when the run ends. Floor of 30 "
		    "seconds: trackers rate-limit announces.</td></tr>"
		    "<tr><td><b>DHT reannounce interval</b></td><td>How often a DHT "
		    "announce is forced. Same 30 second floor.</td></tr>"
		    "</table>"
		    "<h3>Network</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Listen port</b></td><td>TCP/uTP port libtorrent listens "
		    "on and reports to trackers. UPnP and NAT-PMP are always attempted so "
		    "incoming connections can reach it.</td></tr>"
		    "</table>"
		    "<h3>Discovery</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Trackers</b></td><td>Announce to the trackers in the "
		    "torrent. Unchecking clears the tracker list for this run.</td></tr>"
		    "<tr><td><b>DHT</b></td><td>Enable the distributed hash table, "
		    "bootstrapped from the usual public router nodes.</td></tr>"
		    "<tr><td><b>PEX</b></td><td>Peer exchange, the ut_pex plugin. "
		    "Unchecking leaves the plugin out of the session, so no peer lists "
		    "are swapped with connected peers. PEX only works after at least one "
		    "peer is connected, so it needs Trackers, DHT or LSD.</td></tr>"
		    "<tr><td><b>LSD</b></td><td>Local service discovery on the LAN.</td></tr>"
		    "<tr><td><b>Extra trackers</b></td><td>One URL per line, added to "
		    "every torrent you load in a tier after its own trackers, for both "
		    "libtorrent's announces and the direct announce. The default is four "
		    "large public UDP trackers. Torrents from private-style sites list "
		    "only their own HTTP trackers, while many of their peers also "
		    "announce to public ones; this is how to reach that pool. A tracker "
		    "that does not know the torrent answers with 0 seeders and 0 "
		    "leechers, visible in the Log. Lines starting with # are ignored; "
		    "an empty box adds nothing.</td></tr>"
		    "</table>"
		    "<h3>IPinfo</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Look up ...</b></td><td>Fetch country, city, provider "
		    "and hostname for each IP from ipinfo.io and show them in the table. "
		    "Lookups run in a background thread, one at a time, and each IP is "
		    "looked up once per run. A normal end of run waits for the queue to "
		    "drain. Without a token the free tier applies and lookups may be "
		    "refused after a few hundred. When off, the IPinfo columns are "
		    "hidden.</td></tr>"
		    "<tr><td><b>IPinfo token</b></td><td>Optional. Used only for this run "
		    "and never written to settings.</td></tr>"
		    "</table>"
		    "<h3>Copy/Save includes</h3>"
		    "<p>The two boxes next to the buttons decide what follows the "
		    "endpoint on copied and saved lines: <b>Sources</b> adds the source "
		    "tags, <b>IP info</b> adds country, city, provider and host as they "
		    "appear in the table. Both off gives bare <tt>IP:port</tt> lines. "
		    "They can be changed after a run, before copying.</p>"
		    "<p>All other options are remembered between runs.</p>");

		addHelpPage(pages, "Peer Sources",
		    "<h3>Where a peer can come from</h3>"
		    "<p>Every peer carries one or more tags. A peer with several tags was "
		    "seen more than one way.</p>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><tt>connected/get_peer_info</tt></td><td>Had a completed "
		    "connection at the moment of a poll.</td></tr>"
		    "<tr><td><tt>connected/handshake</tt></td><td>Completed the "
		    "BitTorrent handshake, seen directly by a libtorrent plugin, however "
		    "short the connection was. Its bitfield feeds the Have column.</td></tr>"
		    "<tr><td><tt>connecting</tt></td><td>libtorrent was opening a "
		    "connection or handshaking at the moment of a poll; it may never "
		    "have completed.</td></tr>"
		    "<tr><td><tt>tracker-direct-http</tt></td><td>Returned by an HTTP or "
		    "HTTPS tracker to this application's own announce, made alongside "
		    "libtorrent's.</td></tr>"
		    "<tr><td><tt>tracker-direct-udp</tt></td><td>Same, from a UDP tracker "
		    "(BEP 15).</td></tr>"
		    "<tr><td><tt>peer-connect-out</tt></td><td>libtorrent connected to the "
		    "peer and completed the handshake.</td></tr>"
		    "<tr><td><tt>peer-connect-in</tt></td><td>The peer connected to us and "
		    "completed the handshake.</td></tr>"
		    "<tr><td><tt>peer-connect-failed</tt></td><td>libtorrent tried to "
		    "connect and failed. The address came from a tracker, DHT, PEX or LSD "
		    "but the peer was unreachable.</td></tr>"
		    "<tr><td><tt>peer-disconnected</tt></td><td>A connected peer went "
		    "away, for any reason.</td></tr>"
		    "<tr><td><tt>peer-incoming</tt></td><td>An incoming connection "
		    "arrived, before any handshake.</td></tr>"
		    "<tr><td><tt>peer-error</tt></td><td>The peer sent something "
		    "invalid.</td></tr>"
		    "<tr><td><tt>peer-banned</tt></td><td>libtorrent banned the peer for "
		    "sending bad data.</td></tr>"
		    "<tr><td><tt>peer-blocked</tt></td><td>Rejected by libtorrent. This "
		    "application sets no IP filter, so in practice these are hosts "
		    "libtorrent banned on its own: DHT crawlers that connect asking for "
		    "one of the decoy info hashes libtorrent plants in DHT traffic. They "
		    "are not peers of the torrent.</td></tr>"
		    "<tr><td><tt>peer-alert</tt></td><td>Found in the text of some other "
		    "peer alert.</td></tr>"
		    "</table>"
		    "<h3>What is not recorded</h3>"
		    "<ul>"
		    "<li>This machine's own addresses. Trackers return the announcing "
		    "peer in their reply, so every local interface address and the "
		    "external address libtorrent reports are removed from the list.</li>"
		    "<li>IPv6 peers are skipped everywhere.</li>"
		    "<li>Peers a tracker or DHT returned that libtorrent never tried to "
		    "contact. libtorrent reports counts for those, not addresses.</li>"
		    "</ul>"
		    "<p>A peer that appears with only <tt>peer-connect-failed</tt> is "
		    "still a real address that was advertised for this torrent. That is "
		    "usually most of a long run's list.</p>");

		addHelpPage(pages, "Output",
		    "<h3>The table</h3>"
		    "<p>One row per peer. IP and Port sort numerically, the other "
		    "columns alphabetically. Rows can be selected; selection and "
		    "scroll position survive the refresh every 3 seconds. The IPinfo "
		    "columns are hidden when IPinfo lookups are off. Double-click a "
		    "column's right edge in the header to fit it to its content.</p>"
		    "<p><b>Have</b> shows, for every peer that completed a connection "
		    "during this run, the share of the torrent it reported having: 100% "
		    "is a seed. It is the best value the peer reported, captured the "
		    "moment its bitfield arrived, so even a connection that lasted a "
		    "fraction of a second counts. It stays once known. "
		    "A green dot means the peer connected but never sent its bitfield, "
		    "which happens when it drops the connection at once. A red cross, "
		    "with the IP in red, marks a host libtorrent banned or blocked "
		    "(sources <tt>peer-banned</tt>, <tt>peer-blocked</tt>): DHT crawlers "
		    "and senders of bad data, not peers of the torrent. Sorting the "
		    "column puts them last. Peers known "
		    "only from trackers or DHT, or that were only being contacted "
		    "(source <tt>connecting</tt>), leave the cell empty. Connected peers "
		    "also have their IP in bold. The tab title and the status line show "
		    "how many peers have connected so far.</p>"
		    "<p>Clicking anywhere in the window outside the table clears the "
		    "selection.</p>"
		    "<h3>Right-click menu</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Copy selected peers</b> (Ctrl+C)</td><td>Only the "
		    "selected rows, in the same text format as Copy Peers.</td></tr>"
		    "<tr><td><b>Copy all peers</b></td><td>Same as the Copy Peers "
		    "button.</td></tr>"
		    "<tr><td><b>Get ASN IPv4 prefixes for ...</b></td><td>Finds the "
		    "autonomous system that announces the selected peer's IP and lists "
		    "every IPv4 prefix that AS announces in BGP right now: the address "
		    "pool of the peer's network operator, ready for a filter. The list "
		    "opens in its own window with Copy and Save. Format: a header "
		    "comment with AS number, country, city and holder, then one CIDR "
		    "per line with the same comment plus the number of addresses. "
		    "Country, city and the operator name come from the peer's IPinfo "
		    "cells when present (the provider without its AS number), otherwise "
		    "from RIPEstat; the prefixes always come from RIPEstat's public API "
		    "(stat.ripe.net), no key needed.</td></tr>"
		    "</table>"
		    "<h3>Text format of Copy and Save</h3>"
		    "<p>One peer per line in the table's current order. With "
		    "comments enabled every line looks like:</p>"
		    "<pre>1.2.3.4:6881      # peer-connect-out; DE, Berlin, AS3320 Deutsche Telekom, host.example.de</pre>"
		    "<p>The <tt>#</tt> column is aligned: it starts five characters past "
		    "the longest <tt>IP:port</tt> in the list. Source tags come first, "
		    "separated by commas, then a semicolon, then country, city, provider "
		    "and host exactly as the table shows them, empty ones left out. "
		    "<tt>(pending)</tt> means the lookup had not run yet; "
		    "<tt>(unavailable)</tt> means it failed.</p>"
		    "<p>Without comments the list is just the endpoints, ready to feed to "
		    "another tool.</p>"
		    "<h3>Buttons</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>Copy Peers</b></td><td>Copy the Seen / Known Peers tab to "
		    "the clipboard.</td></tr>"
		    "<tr><td><b>Copy Selected</b></td><td>Copy only the selected rows of "
		    "the table, same format. Also Ctrl+C in the table or the right-click "
		    "menu. Enabled while something is selected.</td></tr>"
		    "<tr><td><b>Save Peers...</b></td><td>Write the peers tab, exactly as "
		    "shown, to a file. The dialog opens in the folder used last time.</td></tr>"
		    "<tr><td><b>Clear Log</b></td><td>Empty the Log tab. The peers tab "
		    "is untouched.</td></tr>"
		    "</table>"
		    "<h3>Summary</h3>"
		    "<p>When a run ends the Log tab shows a summary with the unique "
		    "total and a count per source tag. The Log keeps the last "
		    "20,000 lines; older ones drop off the top.</p>");

		addHelpPage(pages, "Shortcuts",
		    "<h3>Keyboard</h3>"
		    "<table cellpadding='3' cellspacing='0'>"
		    "<tr><td><b>F1</b></td><td>Open this help</td></tr>"
		    "<tr><td><b>Esc</b></td><td>Close this help</td></tr>"
		    "<tr><td><b>Ctrl+C</b> in the peers table</td><td>Copy the selected "
		    "peers</td></tr>"
		    "</table>"
		    "<p>The help window is not modal. It can stay open while a "
		    "collection runs.</p>");

		layout->addWidget(pages, 1);
		layout->addWidget(buttons);

		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

		/* Escape, the Close button and the window close all end up here. */
		connect(dialog, &QDialog::finished, this, [this, dialog]() {
			settings.setValue("ui/help_geometry", dialog->saveGeometry());
		});

		geometry = settings.value("ui/help_geometry").toByteArray();
		if (!geometry.isEmpty()) {
			dialog->restoreGeometry(geometry);
		} else {
			dialog->resize(680, 600);
		}

		helpDialog = dialog;
		dialog->show();
	}

	void createTabs(QVBoxLayout *parent)
	{
		tabs = new QTabWidget(this);

		logText = new QPlainTextEdit(tabs);

		QFont monoFont("monospace");
		monoFont.setStyleHint(QFont::Monospace);
		monoFont.setFixedPitch(true);

		logText->setFont(monoFont);
		logText->setReadOnly(true);
		logText->setMaximumBlockCount(LOG_MAX_LINES);

		/*
		 * Peers are a table: sortable by any column, rows selectable, and the
		 * 3 second refresh keeps selection and scroll position. Sorting goes
		 * through a proxy on Qt::UserRole so IPs and ports sort numerically.
		 */
		peersModel = new QStandardItemModel(0, COL_COUNT, this);
		peersModel->setHorizontalHeaderLabels(
		    {"IP", "Port", "Have", "Sources", "Country", "City", "Provider", "Host"});

		peersProxy = new QSortFilterProxyModel(this);
		peersProxy->setSourceModel(peersModel);
		peersProxy->setSortRole(Qt::UserRole);

		peersTable = new QTableView(tabs);
		peersTable->setModel(peersProxy);
		peersTable->setSortingEnabled(true);
		peersTable->sortByColumn(COL_IP, Qt::AscendingOrder);
		peersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		peersTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
		peersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
		peersTable->setAlternatingRowColors(true);
		peersTable->setWordWrap(false);
		peersTable->verticalHeader()->setVisible(false);
		peersTable->verticalHeader()->setDefaultSectionSize(peersTable->fontMetrics().height() + 6);
		peersTable->horizontalHeader()->setStretchLastSection(true);
		peersTable->horizontalHeader()->setHighlightSections(false);
		peersTable->setColumnWidth(COL_IP, 130);
		peersTable->setColumnWidth(COL_PORT, 60);
		peersTable->setColumnWidth(COL_ACTIVE, 55);
		peersTable->setColumnWidth(COL_SOURCES, 260);
		peersTable->setColumnWidth(COL_COUNTRY, 90);
		peersTable->setColumnWidth(COL_CITY, 140);
		peersTable->setColumnWidth(COL_PROVIDER, 220);

		/*
		 * Right-click menu and Ctrl+C on the table. The shortcut is scoped
		 * to the table so Ctrl+C in the log or a text field keeps its
		 * normal meaning.
		 */
		/* Mouse presses anywhere in this window, to clear the selection. */
		qApp->installEventFilter(this);
		peersTable->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(peersTable, &QTableView::customContextMenuRequested, this, &MainWindow::showPeersContextMenu);

		connect(peersTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
			copySelectedButton->setEnabled(peersTable->selectionModel()->hasSelection());
		});

		QShortcut *copy_selected = new QShortcut(QKeySequence::Copy, peersTable);
		copy_selected->setContext(Qt::WidgetShortcut);
		connect(copy_selected, &QShortcut::activated, this, &MainWindow::copySelectedPeers);

		tabs->addTab(logText, "Log");
		tabs->addTab(peersTable, "Seen / Known Peers");

		parent->addWidget(tabs, 1);
	}

	void loadSettings()
	{
		QByteArray window_geometry;
		int last_tab_index;

		window_geometry = settings.value("ui/main_window_geometry").toByteArray();
		if (!window_geometry.isEmpty()) {
			restoreGeometry(window_geometry);
		}

		torrentPathEdit->setText(settings.value("paths/torrent", "").toString());
		lastPeersDir = settings.value("paths/peers_dir", "").toString();
		extraTrackersEdit->setPlainText(settings.value("options/extra_trackers", QString(DEFAULT_EXTRA_TRACKERS)).toString());
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
		ipInfoLookupCheck->setChecked(settings.value("options/ipinfo_lookup", false).toBool());
		exportSourcesCheck->setChecked(settings.value("export/sources", true).toBool());
		exportIpInfoCheck->setChecked(settings.value("export/ipinfo", true).toBool());
		ipInfoTokenEdit->clear();

		last_tab_index = settings.value("ui/last_tab_index", 0).toInt();
		if (tabs != nullptr && last_tab_index >= 0 && last_tab_index < tabs->count()) {
			tabs->setCurrentIndex(last_tab_index);
		}

		updatePexAvailability();
	}

	void saveSettings()
	{
		settings.setValue("paths/torrent", torrentPathEdit->text().trimmed());
		settings.setValue("paths/peers_dir", lastPeersDir);
		settings.setValue("options/extra_trackers", extraTrackersEdit->toPlainText());
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
		settings.setValue("options/ipinfo_lookup", ipInfoLookupCheck->isChecked());
		settings.setValue("export/sources", exportSourcesCheck->isChecked());
		settings.setValue("export/ipinfo", exportIpInfoCheck->isChecked());
		/* IPinfo token is intentionally not saved. */

		settings.setValue("ui/main_window_geometry", saveGeometry());
		settings.setValue("ui/last_tab_index", tabs != nullptr ? tabs->currentIndex() : 0);
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
			saveSettings();
		}
	}

	/* Folder the Save Peers dialog opens in: the last one used, else home. */
	QString peersDialogDir() const
	{
		if (!lastPeersDir.isEmpty() && QDir(lastPeersDir).exists()) {
			return lastPeersDir;
		}

		return QDir::homePath();
	}

	/*
	 * Write the Seen / Known Peers tab as it stands, exactly what Copy Peers
	 * puts on the clipboard. Nothing is written without this button.
	 */
	void savePeers()
	{
		QString path;
		QString text;
		QFile f;

		text = peersAsText();

		if (text.trimmed().isEmpty()) {
			QMessageBox::information(this, "Nothing to save", "The peer list is empty.");
			return;
		}

		path = QFileDialog::getSaveFileName(
		    this,
		    "Save peers",
		    peersDialogDir() + "/peers.txt",
		    "Text files (*.txt);;All files (*)");

		if (path.isEmpty()) {
			return;
		}

		f.setFileName(path);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
			QMessageBox::warning(this, "Save failed", f.errorString());
			return;
		}

		if (!text.endsWith('\n')) {
			text += '\n';
		}

		f.write(text.toUtf8());
		f.close();

		lastPeersDir = QFileInfo(path).absolutePath();
		saveSettings();

		statusLabel->setText(QString("Saved %1 line(s) to %2")
		                         .arg(text.count('\n'))
		                         .arg(path));
	}

	void startCollection()
	{
		QString torrent_path;

		torrent_path = torrentPathEdit->text().trimmed();

		if (torrent_path.isEmpty()) {
			QMessageBox::warning(this, "Missing torrent", "Choose a .torrent file.");
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
		peersModel->removeRows(0, peersModel->rowCount());
		setIpInfoColumnsVisible(ipInfoLookupCheck->isChecked());
		progress->setValue(0);
		progress->setFormat("0%");
		progressTextLabel->setText("Elapsed: 00:00:00    Remaining: --:--:--");
		statusLabel->setText("Starting...");

		worker = new PeerCollectorThread(
		    torrent_path,
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
		    ipInfoLookupCheck->isChecked(),
		    ipInfoTokenEdit->text().trimmed(),
		    trackersCheck->isChecked() ? extraTrackerList() : QStringList(),
		    this);

		connect(worker, &PeerCollectorThread::logMessage, this, &MainWindow::appendLog);
		connect(worker, &PeerCollectorThread::peerCountChanged, this, &MainWindow::setPeerCount);
		connect(worker, &PeerCollectorThread::peersChanged, this, &MainWindow::setPeerRows);
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

	void setPeerCount(int count, int active)
	{
		statusLabel->setText(QString("Running. Unique seen/known peers: %1, connected so far: %2")
		                         .arg(count)
		                         .arg(active));
		tabs->setTabText(tabs->indexOf(peersTable),
		                 QString("Seen / Known Peers (%1, %2 active)").arg(count).arg(active));
	}

	/* Extra trackers box as a clean list: trimmed, no blanks, no # comments. */
	QStringList extraTrackerList() const
	{
		QStringList out;

		for (const QString &line : extraTrackersEdit->toPlainText().split('\n')) {
			QString t = line.trimmed();

			if (!t.isEmpty() && !t.startsWith('#')) {
				out << t;
			}
		}

		return out;
	}

	void setIpInfoColumnsVisible(bool visible)
	{
		peersTable->setColumnHidden(COL_COUNTRY, !visible);
		peersTable->setColumnHidden(COL_CITY, !visible);
		peersTable->setColumnHidden(COL_PROVIDER, !visible);
		peersTable->setColumnHidden(COL_HOST, !visible);
	}

	/*
	 * Rebuild the table from the worker's rows, keeping what the user had:
	 * selected endpoints and the scroll position. The full PeerRow is kept in
	 * Qt::UserRole + 1 of the IP cell so Copy/Save can rebuild the text in
	 * the table's current order.
	 */
	void setPeerRows(const PeerRows &rows)
	{
		QSet<QString> selected;
		int scroll = peersTable->verticalScrollBar()->value();

		for (const QModelIndex &idx : peersTable->selectionModel()->selectedRows(COL_IP)) {
			selected.insert(peersProxy->mapToSource(idx).data(Qt::UserRole + 2).toString());
		}

		peersModel->removeRows(0, peersModel->rowCount());

		for (const PeerRow &row : rows) {
			QString country, city, provider, host;
			quint32 ip_key = 0;
			quint16 port_key = 0;
			QList<QStandardItem *> items;

			ipv4PortToSortKey(row.endpoint, &ip_key, &port_key);
			parseIpInfoFields(row.ipinfo, &country, &city, &provider, &host);

			QStandardItem *ip_item = new QStandardItem(peerIpOnly(row.endpoint));
			ip_item->setData(ip_key, Qt::UserRole);
			ip_item->setData(row.sources.join(", "), Qt::UserRole + 1);
			ip_item->setData(row.endpoint, Qt::UserRole + 2);
			ip_item->setData(row.ipinfo, Qt::UserRole + 3);

			QStandardItem *port_item = new QStandardItem(QString::number(port_key));
			port_item->setData((uint)port_key, Qt::UserRole);
			port_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

			/*
			 * Peers that connected during the run: the share of the torrent
			 * they reported having, and the IP in bold. Peers known only
			 * from trackers or DHT leave the cell empty.
			 */
			/*
			 * Hosts libtorrent banned (DHT snoopers, bad-data senders) or
			 * blocked are not peers of the torrent: red cross, red IP.
			 */
			bool bad = row.sources.contains("peer-blocked") || row.sources.contains("peer-banned");

			QStandardItem *active_item = new QStandardItem();
			active_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
			if (bad) {
				active_item->setText(QString::fromUtf8("\u2715"));
				active_item->setTextAlignment(Qt::AlignCenter);
				active_item->setForeground(QBrush(QColor(0xb0, 0x2a, 0x26)));
				active_item->setData(-2, Qt::UserRole);
				ip_item->setForeground(QBrush(QColor(0xb0, 0x2a, 0x26)));
			} else if (row.progress_ppm > 0) {
				active_item->setText(QString::number(qRound(row.progress_ppm / 10000.0)) + "%");
				active_item->setData(row.progress_ppm, Qt::UserRole);
			} else if (row.active) {
				/* Connected, but the peer never told us what it has. */
				active_item->setText(QString::fromUtf8("\u25CF"));
				active_item->setTextAlignment(Qt::AlignCenter);
				active_item->setForeground(QBrush(QColor(0x2f, 0x7d, 0x4f)));
				active_item->setData(0, Qt::UserRole);
			} else {
				active_item->setData(-1, Qt::UserRole);
			}
			if (row.active) {
				QFont bold = ip_item->font();
				bold.setBold(true);
				ip_item->setFont(bold);
			}

			items << ip_item << port_item << active_item;

			for (const QString &text : {row.sources.join(", "), country, city, provider, host}) {
				QStandardItem *it = new QStandardItem(text);
				it->setData(text.toLower(), Qt::UserRole);
				items << it;
			}

			peersModel->appendRow(items);
		}

		if (!selected.isEmpty()) {
			QItemSelection sel;

			for (int r = 0; r < peersProxy->rowCount(); r++) {
				QModelIndex idx = peersProxy->index(r, COL_IP);

				if (selected.contains(peersProxy->mapToSource(idx).data(Qt::UserRole + 2).toString())) {
					sel.select(idx, peersProxy->index(r, COL_COUNT - 1));
				}
			}

			peersTable->selectionModel()->select(sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
		}

		peersTable->verticalScrollBar()->setValue(scroll);
	}

	/*
	 * The peers as text, one per line in the table's current order, in the
	 * same format the tool has always produced: IP:port, then an aligned
	 * "# sources; country, city, provider, host" comment, each part only when
	 * its Copy/Save checkbox is on. The IPinfo part is the table's cells as
	 * they are shown, so "RU, Moscow, AS8580 MTS PJSC, host", not key=value.
	 */
	QString peersAsText(bool selected_only = false) const
	{
		QStringList endpoints;
		QStringList comments;
		QStringList lines;
		bool with_sources = exportSourcesCheck->isChecked();
		bool with_ipinfo = exportIpInfoCheck->isChecked() && !peersTable->isColumnHidden(COL_COUNTRY);
		int column;

		for (int r = 0; r < peersProxy->rowCount(); r++) {
			QModelIndex src = peersProxy->mapToSource(peersProxy->index(r, COL_IP));
			QStringList parts;

			if (selected_only && !peersTable->selectionModel()->isRowSelected(r, QModelIndex())) {
				continue;
			}

			endpoints << src.data(Qt::UserRole + 2).toString();

			if (with_sources) {
				QString sources = src.data(Qt::UserRole + 1).toString();
				parts << (sources.isEmpty() ? QString("seen") : sources);
			}

			if (with_ipinfo) {
				QStringList fields;

				for (int c : {COL_COUNTRY, COL_CITY, COL_PROVIDER, COL_HOST}) {
					QString v = peersModel->item(src.row(), c)->text();
					if (!v.isEmpty()) {
						fields << v;
					}
				}

				if (!fields.isEmpty()) {
					parts << fields.join(", ");
				}
			}

			comments << parts.join("; ");
		}

		column = calculatePeerCommentColumn(endpoints);

		for (int i = 0; i < endpoints.size(); i++) {
			if (comments[i].isEmpty()) {
				lines << endpoints[i];
			} else {
				lines << formatPeerLineWithComment(endpoints[i], comments[i], column);
			}
		}

		return lines.isEmpty() ? QString() : lines.join("\n") + "\n";
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
		worker = nullptr;

		if (ok) {
			progress->setValue(100);
			progress->setFormat("100%");
			if (progressTextLabel != nullptr) {
				progressTextLabel->setText("Finished.");
			}
		}

		/* A hidden window must not open a modal box: nobody could close it. */
		if (closing || !isVisible()) {
			return;
		}

		if (ok) {
			QMessageBox::information(this, "Done", message);
		} else {
			QMessageBox::critical(this, "Collection failed", message);
		}
	}

	void copyPeers()
	{
		QString text = peersAsText();

		QApplication::clipboard()->setText(text);
		statusLabel->setText(QString("%1 peer(s) copied to clipboard.").arg(text.count('\n')));
	}

	void copySelectedPeers()
	{
		QString text = peersAsText(true);

		if (text.isEmpty()) {
			statusLabel->setText("No peers selected.");
			return;
		}

		QApplication::clipboard()->setText(text);
		statusLabel->setText(QString("%1 selected peer(s) copied to clipboard.").arg(text.count('\n')));
	}

	/* The IP of the current row, or of the first selected row, or empty. */
	QString selectedPeerIp() const
	{
		return selectedPeerCell(COL_IP);
	}

	/* A cell of the current row, or of the first selected row, or empty. */
	QString selectedPeerCell(int column) const
	{
		QModelIndex idx = peersTable->currentIndex();
		QModelIndexList rows = peersTable->selectionModel()->selectedRows(COL_IP);

		if (!rows.isEmpty()) {
			idx = rows.first();
		}

		if (!idx.isValid()) {
			return QString();
		}

		return peersProxy->index(idx.row(), column).data().toString();
	}

	void showPeersContextMenu(const QPoint &pos)
	{
		QMenu menu(this);
		int selected = peersTable->selectionModel()->selectedRows().size();
		QString ip = selectedPeerIp();
		QAction *copy_sel;
		QAction *copy_all;
		QAction *asn;

		copy_sel = menu.addAction(QString("Copy %1 selected peer(s)\tCtrl+C").arg(selected));
		copy_sel->setEnabled(selected > 0);
		copy_all = menu.addAction("Copy all peers");
		copy_all->setEnabled(peersProxy->rowCount() > 0);
		menu.addSeparator();
		asn = menu.addAction(ip.isEmpty() ? QString("Get ASN IPv4 prefixes...")
		                                  : QString("Get ASN IPv4 prefixes for %1...").arg(ip));
		asn->setEnabled(!ip.isEmpty());

		QAction *chosen = menu.exec(peersTable->viewport()->mapToGlobal(pos));

		if (chosen == copy_sel) {
			copySelectedPeers();
		} else if (chosen == copy_all) {
			copyPeers();
		} else if (chosen == asn) {
			/*
			 * Country, city and provider as the table shows them. "(pending)"
			 * is not a country, and the provider's leading "AS1234 " is
			 * dropped so the label reads like the user's filter files.
			 */
			static const QRegularExpression as_prefix("^AS\\d+\\s+");
			QString country = selectedPeerCell(COL_COUNTRY);
			QString city = selectedPeerCell(COL_CITY);
			QString holder = selectedPeerCell(COL_PROVIDER);

			if (country.startsWith('(')) {
				country.clear();
			}

			holder.remove(as_prefix);

			lookupAsnPrefixes(ip, country, city, holder);
		}
	}

	void lookupAsnPrefixes(const QString &ip, const QString &country, const QString &city, const QString &holder)
	{
		AsnPrefixThread *t;

		if (ip.isEmpty()) {
			return;
		}

		t = new AsnPrefixThread(ip, country, city, holder, this);
		asnThreads << t;

		connect(t, &AsnPrefixThread::done, this, &MainWindow::asnPrefixesReady);
		connect(t, &QThread::finished, t, &QObject::deleteLater);

		statusLabel->setText("Looking up origin AS and announced IPv4 prefixes for " + ip + " at RIPEstat...");
		t->start();
	}

	void asnPrefixesReady(const QString &ip,
	                      const QString &asn,
	                      const QString &holder,
	                      const QString &country,
	                      const QString &city,
	                      const QStringList &prefixes,
	                      const QString &error)
	{
		if (closing || !isVisible()) {
			return;
		}

		if (!error.isEmpty()) {
			statusLabel->setText("ASN lookup failed for " + ip + ": " + error);
			QMessageBox::warning(this, "ASN lookup failed", ip + "\n\n" + error);
			return;
		}

		statusLabel->setText(QString("AS%1 announces %2 IPv4 prefix(es).").arg(asn).arg(prefixes.size()));
		showAsnPrefixDialog(ip, asn, holder, country, city, prefixes);
	}

	/*
	 * The prefix pool in the filter format, with Copy and Save. Not modal:
	 * several can be open, one per AS looked up.
	 */
	void showAsnPrefixDialog(const QString &ip,
	                         const QString &asn,
	                         const QString &holder,
	                         const QString &country,
	                         const QString &city,
	                         const QStringList &prefixes)
	{
		QDialog *dialog = new QDialog(this);
		QVBoxLayout *layout = new QVBoxLayout(dialog);
		QLabel *heading = new QLabel(dialog);
		QPlainTextEdit *text = new QPlainTextEdit(dialog);
		QHBoxLayout *buttons = new QHBoxLayout();
		QPushButton *copy = new QPushButton("Copy", dialog);
		QPushButton *save = new QPushButton("Save...", dialog);
		QPushButton *close = new QPushButton("Close", dialog);
		QString body = formatAsnPrefixList(asn, country, city, holder, prefixes);
		QFont mono("monospace");

		mono.setStyleHint(QFont::Monospace);
		mono.setFixedPitch(true);

		dialog->setWindowTitle(QString("AS%1 IPv4 prefixes").arg(asn));
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->resize(720, 560);

		heading->setTextFormat(Qt::RichText);
		heading->setWordWrap(true);
		heading->setText(QString("<b>%1</b> is announced by <b>AS%2</b>%3.<br>"
		                         "%4 IPv4 prefix(es) in BGP right now, from RIPEstat, %5.")
		                     .arg(ip.toHtmlEscaped())
		                     .arg(asn.toHtmlEscaped())
		                     .arg(holder.isEmpty() ? QString() : " (" + holder.toHtmlEscaped() + ")")
		                     .arg(prefixes.size())
		                     .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));

		text->setFont(mono);
		text->setReadOnly(true);
		text->setLineWrapMode(QPlainTextEdit::NoWrap);
		text->setPlainText(body);

		buttons->addWidget(copy);
		buttons->addWidget(save);
		buttons->addStretch(1);
		buttons->addWidget(close);

		layout->addWidget(heading);
		layout->addWidget(text, 1);
		layout->addLayout(buttons);

		connect(close, &QPushButton::clicked, dialog, &QDialog::close);
		connect(copy, &QPushButton::clicked, this, [this, body, asn]() {
			QApplication::clipboard()->setText(body);
			statusLabel->setText(QString("AS%1 prefixes copied to clipboard.").arg(asn));
		});
		connect(save, &QPushButton::clicked, this, [this, dialog, body, asn]() {
			QString path = QFileDialog::getSaveFileName(
			    dialog,
			    "Save prefixes",
			    peersDialogDir() + QString("/AS%1-ipv4-prefixes.txt").arg(asn),
			    "Text files (*.txt);;All files (*)");

			if (path.isEmpty()) {
				return;
			}

			QFile f(path);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
				QMessageBox::warning(dialog, "Save failed", f.errorString());
				return;
			}

			f.write(body.toUtf8());
			f.close();

			lastPeersDir = QFileInfo(path).absolutePath();
			saveSettings();
			statusLabel->setText("Saved prefixes to " + path);
		});

		dialog->show();
	}

};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	/*
	 * Identity for anything that derives a path from the application rather
	 * than being told one, such as QStandardPaths or a bare QSettings(). The
	 * settings member is built from the same two names, so the settings key
	 * and the per-user data folder stay in step and both follow the defines.
	 */
	QCoreApplication::setOrganizationName(CONFIG_FOLDER_NAME);
	QCoreApplication::setApplicationName(APP_NAME);

	/* Worker -> GUI signal payload, delivered through the queued connection. */
	qRegisterMetaType<PeerRows>("PeerRows");

	/*
	 * KDE's native file dialog logs "No node found for item that was just
	 * removed" for files that vanished from the folder it lists. Harmless
	 * KIO chatter, nothing this program did; keep the terminal clean.
	 */
	QLoggingCategory::setFilterRules("kf.kio.widgets.kdirmodel.warning=false");

	MainWindow win;

	win.show();

	return app.exec();
}

#include "libtorrent_peer_collector_qt.moc"
