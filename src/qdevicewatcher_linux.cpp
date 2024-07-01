/******************************************************************************
  QDeviceWatcherPrivate: watching depends on platform
  Copyright (C) 2011-2015 Wang Bin <wbsecg1@gmail.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "qdevicewatcher.h"
#include "qdevicewatcher_p.h"
#ifdef Q_OS_LINUX


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#else

#endif

#include <errno.h>
#include <linux/netlink.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QtCore/QCoreApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtCore/qregexp.h>
#else
#include <QRegExp>
#endif
#if CONFIG_SOCKETNOTIFIER
#include <QtCore/QSocketNotifier>
#elif CONFIG_TCPSOCKET
#include <QtNetwork/QTcpSocket>
#endif

#ifdef VERBOSE_DEBUG_OUTPUT
#include <QtCore/QTime>
#include <QtCore/QFileInfo>
#endif // VERBOSE_DEBUG_OUTPUT

#define UEVENT_BUFFER_SIZE 2048

enum udev_monitor_netlink_group { UDEV_MONITOR_NONE, UDEV_MONITOR_KERNEL, UDEV_MONITOR_UDEV };

QDeviceWatcherPrivate::~QDeviceWatcherPrivate()
{
    stop();
    close(netlink_socket);
    netlink_socket = -1;
}

bool QDeviceWatcherPrivate::start()
{
    if (!init())
        return false;
#if CONFIG_SOCKETNOTIFIER
    socket_notifier->setEnabled(true);
#elif CONFIG_TCPSOCKET
    connect(tcp_socket, SIGNAL(readyRead()), SLOT(parseDeviceInfo()));
#else
    this->QThread::start();
#endif
    return true;
}

bool QDeviceWatcherPrivate::stop()
{
    if (netlink_socket != -1) {
#if CONFIG_SOCKETNOTIFIER
        socket_notifier->setEnabled(false);
#elif CONFIG_TCPSOCKET
        //tcp_socket->close(); //how to restart?
        disconnect(this, SLOT(parseDeviceInfo()));
#else
        this->quit();
#endif
        close(netlink_socket);
        netlink_socket = -1;
    }
    return true;
}

namespace
{

void appendData(const QString &data, QString &dev)
{
    dev.append("@#@" + data);
}


}   // namespace

void QDeviceWatcherPrivate::parseDeviceInfo(int n)
{
    Q_UNUSED(n)

    //! Change includes Bind or Bind is treated ad Add, Unbind
    enum class ActionType {Unknown, Add, Remove, Change, Ignore};
    static const QByteArrayList dataFieldsUsed {
        "SUBSYSTEM", "DEVTYPE", "DEVNAME", "HID_ID", "HID_NAME", "HID_UNIQ", "PRODUCT", "DEVPATH"
    };

#ifdef VERBOSE_DEBUG_OUTPUT
    qDebug().noquote() << QTime::currentTime().toString()
                       << __func__ << "start (" << QFileInfo(__FILE__).fileName() << ")";
#endif // VERBOSE_DEBUG_OUTPUT

    QByteArray data;
#if CONFIG_SOCKETNOTIFIER
    //socket_notifier->setEnabled(false); //for win
    data.resize(UEVENT_BUFFER_SIZE * 2);
    data.fill(0);
    size_t len = read(socket_notifier->socket(), data.data(), UEVENT_BUFFER_SIZE * 2);
    zDebug("read fro socket %d bytes", len);
    data.resize(len);
    //socket_notifier->setEnabled(true); //for win
#elif CONFIG_TCPSOCKET
    data = tcp_socket->readAll();
#endif
    data = data.replace(0, '\n').trimmed(); //In the original line each information is seperated by 0
    if (buffer.isOpen())
        buffer.close();
    buffer.setBuffer(&data);
    buffer.open(QIODevice::ReadOnly);

    ActionType actionType {ActionType::Unknown};
    QString dev;

    while (!buffer.atEnd())
    { //buffer.canReadLine() always false?
        QByteArray line {buffer.readLine().trimmed()};

        // Useful data
#ifdef VERBOSE_DEBUG_OUTPUT
        qDebug().noquote() <<  QTime::currentTime().toString()
                           << "Device line:" << line << "(" << QFileInfo(__FILE__).fileName() << ")";
#endif // VERBOSE_DEBUG_OUTPUT
        if (line.startsWith("ACTION="))
        {
            line = line.sliced(7); // "="

            if (line == "add")
            {
#ifndef USE_BIND_AS_ADD
                actionType = ActionType::Add;
#else
                actionType = ActionType::Ignore;
#endif // USE_BIND_AS_ADD
            }
            else if (line == "remove")
            {
                actionType = ActionType::Remove;
            }
            else if (line == "bind")
            {
#ifdef USE_BIND_AS_ADD
                actionType = ActionType::Add;
#else
                actionType = ActionType::Change;
#endif // USE_BIND_AS_ADD
            }
            else
            {
                actionType = ActionType::Change;
            }
        }
        for (const auto &field : dataFieldsUsed)
        {
            if (line.startsWith(field + "="))
            {
                appendData(line, dev);
            }
        }
    }
#ifdef VERBOSE_DEBUG_OUTPUT
    qDebug().noquote() <<  QTime::currentTime().toString()
                       << "Device info:" << dev << "(" << QFileInfo(__FILE__).fileName() << ")";
#endif // VERBOSE_DEBUG_OUTPUT

    buffer.close();

    QDeviceChangeEvent *event {nullptr};
    if (!dev.isEmpty())
    {
        appendData({}, dev);

        // emit is asynchronous (via the main event loop)
        switch (actionType)
        {
        case ActionType::Add:
            emitDeviceAdded(dev);
            event = new QDeviceChangeEvent(QDeviceChangeEvent::Add, dev);
            break;
        case ActionType::Remove:
            emitDeviceRemoved(dev);
            event = new QDeviceChangeEvent(QDeviceChangeEvent::Remove, dev);
            break;
        case ActionType::Change:
            emitDeviceChanged(dev);
            event = new QDeviceChangeEvent(QDeviceChangeEvent::Change, dev);
            break;
        case ActionType::Unknown:
            qWarning() << "Unknown action" << __FILE__ << __func__;
            break;
        case ActionType::Ignore:
            break;
        default:
            qWarning() << "Unknown action type" << __FILE__ << __func__;
        }
    }

    zDebug("%s %s", qPrintable(action_str), qPrintable(dev));
    if (event)
    {
        if (!event_receivers.isEmpty())
        {
            for (const auto obj : event_receivers)
            {
                // Event queue takes ownership of event
                // There's no memory leak, ignore warning
                QCoreApplication::postEvent(obj, event, Qt::HighEventPriority);
            }
        }
        else
        {
            delete event;
        }
    }

#ifdef VERBOSE_DEBUG_OUTPUT
    qDebug().noquote() << QTime::currentTime().toString()
                       << __func__ << "finish (" << QFileInfo(__FILE__).fileName() << ")\n";
#endif // VERBOSE_DEBUG_OUTPUT
}

#if CONFIG_THREAD
//another thread
void QDeviceWatcherPrivate::run()
{
    QByteArray data;
    //loop only when event happens. because of recv() block the function?
    while (1) {
        //char buf[UEVENT_BUFFER_SIZE*2] = {0};
        //recv(d->netlink_socket, &buf, sizeof(buf), 0);
        data.resize(UEVENT_BUFFER_SIZE * 2);
        data.fill(0);
        size_t len = recv(netlink_socket, data.data(), data.size(), 0);
        zDebug("read fro socket %d bytes", len);
        data.resize(len);
        data = data.replace(0, '\n').trimmed();
        if (buffer.isOpen())
            buffer.close();
        buffer.setBuffer(&data);
        buffer.open(QIODevice::ReadOnly);
        QByteArray line = buffer.readLine();
        while (!line.isNull()) {
            parseLine(line.trimmed());
            line = buffer.readLine();
        }
        buffer.close();
    }
}
#endif //CONFIG_THREAD

/**
 * Create new udev monitor and connect to a specified event
 * source. Valid sources identifiers are "udev" and "kernel".
 *
 * Applications should usually not connect directly to the
 * "kernel" events, because the devices might not be useable
 * at that time, before udev has configured them, and created
 * device nodes.
 *
 * Accessing devices at the same time as udev, might result
 * in unpredictable behavior.
 *
 * The "udev" events are sent out after udev has finished its
 * event processing, all rules have been processed, and needed
 * device nodes are created.
 **/

bool QDeviceWatcherPrivate::init()
{
    struct sockaddr_nl snl;
    const int buffersize = 16 * 1024 * 1024;
    int retval;

    memset(&snl, 0x00, sizeof(struct sockaddr_nl));
    snl.nl_family = AF_NETLINK;
    snl.nl_groups = UDEV_MONITOR_KERNEL;

    netlink_socket = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    //netlink_socket = socket(PF_NETLINK, SOCK_DGRAM|SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT); //SOCK_CLOEXEC may be not available
    if (netlink_socket == -1) {
        qWarning("error getting socket: %s", strerror(errno));
        return false;
    }

    /* set receive buffersize */
    setsockopt(netlink_socket, SOL_SOCKET, SO_RCVBUFFORCE, &buffersize, sizeof(buffersize));
    retval = bind(netlink_socket, (struct sockaddr *) &snl, sizeof(struct sockaddr_nl));
    if (retval < 0) {
        qWarning("bind failed: %s", strerror(errno));
        close(netlink_socket);
        netlink_socket = -1;
        return false;
    } else if (retval == 0) {
        //from eeeeeeeudev-monitor.c
        struct sockaddr_nl _snl;
        socklen_t _addrlen;

        /*
         * get the address the kernel has assigned us
         * it is usually, but not necessarily the pid
         */
        _addrlen = sizeof(struct sockaddr_nl);
        retval = getsockname(netlink_socket, (struct sockaddr *) &_snl, &_addrlen);
        if (retval == 0)
            snl.nl_pid = _snl.nl_pid;
    }

#if CONFIG_SOCKETNOTIFIER
    socket_notifier = new QSocketNotifier(netlink_socket, QSocketNotifier::Read, this);
    connect(socket_notifier, SIGNAL(activated(int)), SLOT(parseDeviceInfo(int))); // will always active
    socket_notifier->setEnabled(false);
#elif CONFIG_TCPSOCKET
    //QAbstractSocket *socket = new QAbstractSocket(QAbstractSocket::UnknownSocketType, this); //will not detect "remove", why?
    tcp_socket = new QTcpSocket(this); //works too
    if (!tcp_socket->setSocketDescriptor(netlink_socket, QAbstractSocket::ConnectedState)) {
        qWarning("Failed to assign native socket to QAbstractSocket: %s",
                 qPrintable(tcp_socket->errorString()));
        delete tcp_socket;
        return false;
    }
#endif
    return true;
}

// Not used (moved to parseDeviceInfo())
void QDeviceWatcherPrivate::parseLine(const QByteArray &line)
{
    qDebug() << "LINE: " << line;

    zDebug("%s", line.constData());
#define USE_REGEXP 0
#if USE_REGEXP
    QRegExp rx("(\\w+)(?:@/.*/block/.*/)(\\w+)\\W*");
    //QRegExp rx("(add|remove|change)@/.*/block/.*/(\\w+)\\W*");
    if (rx.indexIn(line) == -1)
        return;
    QString action_str = rx.cap(1).toLower();
    QString dev = "/dev/" + rx.cap(2);
#else
    /*!
    if (!line.contains("/block/")) //hotplug
        return; !*/
    QString action_str = line.left(line.indexOf('@')).toLower();
    QString dev = "/dev/" + line.right(line.length() - line.lastIndexOf('/') - 1);
#endif //USE_REGEXP
    QDeviceChangeEvent *event = 0;

    if (action_str == QLatin1String("add")) {
        emitDeviceAdded(dev);
        event = new QDeviceChangeEvent(QDeviceChangeEvent::Add, dev);
    } else if (action_str == QLatin1String("remove")) {
        emitDeviceRemoved(dev);
        event = new QDeviceChangeEvent(QDeviceChangeEvent::Remove, dev);
    } else if (action_str == QLatin1String("change")) {
        emitDeviceChanged(dev);
        event = new QDeviceChangeEvent(QDeviceChangeEvent::Change, dev);
    }

    zDebug("%s %s", qPrintable(action_str), qPrintable(dev));

    if (event != 0 && !event_receivers.isEmpty()) {
        foreach (QObject *obj, event_receivers) {
            QCoreApplication::postEvent(obj, event, Qt::HighEventPriority);
        }
    }
}

#endif //Q_OS_LINUX

