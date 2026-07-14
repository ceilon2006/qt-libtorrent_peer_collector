QT += widgets core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = libtorrent_peer_collector_qt
TEMPLATE = app

SOURCES += libtorrent_peer_collector_qt.cpp

CONFIG += link_pkgconfig
PKGCONFIG += libtorrent-rasterbar
