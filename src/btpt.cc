#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <cstddef>
#include <cstdlib>
#include <sys/stat.h>

#include <linux/uinput.h>

#include <NickelHook.h>

#include <QApplication>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFile>
#include <QWidget>
#include <QTextStream>
#include <QThread>

#include "btpt.h"
#include "eventcodes.h"

/* Directory in which to look for device configuration */
#define BTPT_DIR "/mnt/onboard/.btpt/"

/* Stop Bluetooth heartbeat after this many seconds of inactivity */
#define BLUETOOTH_TIMEOUT (10*60)

static void *(*BluetoothHeartbeat)(void *, long long);
static void (*BluetoothHeartbeat_beat)(void *);

static void *(*MainWindowController_sharedInstance)();
static QWidget *(*MainWindowController_currentView)(void *);

static QObject *(*PowerManager_sharedInstance)();
static int (*PowerManager_filter)(QObject *, QObject *, QEvent *);
static QEvent::Type (*TimeEvent_eventType)();

static void invokeMainWindowController(const char *method)
{
	QString name = QString();
	void *mwc = MainWindowController_sharedInstance();
	if (!mwc) {
		nh_log("invalid MainWindowController");
		return;
	}
	QWidget *cv = MainWindowController_currentView(mwc);
	if (!cv) {
		nh_log("invalid View");
		return;
	}
	name = cv->objectName();
	if (name == "ReadingView") {
		nh_log(method);
		QMetaObject::invokeMethod(cv, method, Qt::QueuedConnection);
	}
	else {
		nh_log("not reading view");
	}
}

bool BluetoothPageTurner::addDevice(
	const QString &name, const QString &uniq, const QString &handler)
{
	int fd;

	if (devices.contains(uniq)) {
		return false;
	}

	/* Fall back to device name */
	QFile cfg(BTPT_DIR + name);

	/* Look for case-insensitive Bluetooth address in BTPT_DIR */
	QStringList list = QDir(BTPT_DIR).entryList();
	for (int i = 0; i < list.size(); ++i) {
		QString filename = list.at(i);
		if (filename.toLower() == uniq.toLower()) {
			cfg.setFileName(BTPT_DIR + filename);
		}
	}

	if (!cfg.open(QIODevice::ReadOnly | QIODevice::Text)) {
		nh_log("unable to open %s%s",
		       BTPT_DIR, cfg.fileName().toStdString().c_str());
		return false;
	}

	QString path = "/dev/input/" + handler;
	fd = open(path.toStdString().c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		nh_log("error opening /dev/input/%s",
		       path.toStdString().c_str());
		return false;
	}

	/* Found a config file and a /dev/input handler, so configure device */
	Device &device = devices[uniq];
	device.fd = fd;
	device.cfg.clear();

	QByteArray text = cfg.readAll();
	cfg.close();
	QTextStream in(&text);

	while (!in.atEnd()) {
		/* Configuration file format is, one per line:
		 *
		 * METHOD TYPE CODE VALUE
		 *
		 * Invoke METHOD on MainWindowController when input event
		 * matches TYPE, CODE, and VALUE.
		 *
		 * TYPE, CODE, and VALUE can be #defines from
		 * <linux/input-event-codes.h>
		 */

		QString line = in.readLine();
		QList<QString> parts = line.split(" ", QString::SkipEmptyParts);

        /* invalid if it is NEITHER 4 NOR 5 columns*/
		if (parts.size() != 4 && parts.size() != 5) {
			nh_log("invalid config line: %s",
			       line.toStdString().c_str());
			devices.remove(uniq);
			return false;
		}

		struct input_event event = { 0 };
		bool ok;

		event.type = parseEventCode(&ok, parts[1]);
		if (!ok) {
			nh_log("invalid type: %s",
			       parts[1].toStdString().c_str());
			devices.remove(uniq);
			return false;
		}

		event.code = parseEventCode(&ok, parts[2]);
		if (!ok) {
			nh_log("invalid code: %s",
			       parts[2].toStdString().c_str());
			devices.remove(uniq);
			return false;
		}

		event.value = parseEventCode(&ok, parts[3]);
		if (!ok) {
			nh_log("invalid value: %s",
			       parts[3].toStdString().c_str());
			devices.remove(uniq);
			return false;
		}

        /* NEW: Create and populate ConfigRule */
        ConfigRule rule;
        rule.trigger = event;
        rule.method = parts[0];
        rule.type = TYPE_ANY; // Default to old behavior

        /* Check for optional 5th column */
        if (parts.size() >= 5) {
            QString typeStr = parts[4].toUpper();
            if (typeStr == "LONG") rule.type = TYPE_LONG;
            else if (typeStr == "SHORT") rule.type = TYPE_SHORT;
        }

        device.cfg.append(rule);
	}

	nh_log("acquired device %s: %s",
	       handler.toStdString().c_str(),
	       uniq.toStdString().c_str());

	return true;
}

void BluetoothPageTurner::directoryChanged(const QString &path)
{
	(void)path;
	mutex.lock();
	deviceChanges++;
	newDevice.wakeAll();
	mutex.unlock();
}

bool BluetoothPageTurner::scanDevices()
{
	bool devicesAdded = false;

	QFile file("/proc/bus/input/devices");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		nh_log("error opening /proc/bus/input/devices");
		return devicesAdded;
	}

	nh_log("checking /proc/bus/input/devices for new devices");

	QByteArray text = file.readAll();

	file.close();

	QTextStream in(&text);
	QString i;
	QString u;
	QString n;
	while (!in.atEnd()) {
		QString line = in.readLine();
		QString type = line.section(": ", 0, 0);
		if (type == "I") {
			i = line.section(": ", 1);
		}
		else if (type == "U") {
			/* Bluetooth address without the ':'s */
			u = line.section(": Uniq=", 1);
			u.remove(':');
		}
		else if (type == "N") {
			/* Bluetooth name without surrounding double-quotes */
			n = line.section(": Name=", 1);
			n.chop(1);
			n.remove(0, 1);
		}
		else if (type == "H") {
			/* Skip if not Bluetooth device */
			if (!i.startsWith("Bus=0005 ")) {
				nh_log("skipping %s", i.toStdString().c_str());
				continue;
			}

			/* Configure the device */
			nh_log("found %s", i.toStdString().c_str());
			QList<QString> handlers =
				line.section("Handlers=", 1).split(" ");
			foreach(const QString &handler, handlers) {
				if (handler.startsWith("event")) {
					devicesAdded |=
						addDevice(n, u, handler);
				}
			}
		}
	}

	nh_log("devices scanned");

	return devicesAdded;
}

void BluetoothPageTurner::run()
{
	void *bluetoothHeartbeat = NULL;
	fd_set rfds;
	struct timeval tv;
	struct timespec last_event = { 0 };
	struct timespec now = { 0 };

	nh_log("starting");

	/* FileSystemWatcher will increment devicesChanges and wake us up when
	 * /dev/input changes */
	QObject::connect(
		&watcher, &QFileSystemWatcher::directoryChanged,
		this, &BluetoothPageTurner::directoryChanged);
	watcher.addPath("/dev/input");

	while (1) {
		struct input_event e;
		int ret;

		/* Scan input devices for new devices */
		mutex.lock();
		if (deviceChanges == 0 && devices.empty()) {
			nh_log("waiting for input devices");
			newDevice.wait(&mutex);
			deviceChanges = 0;
			mutex.unlock();
			if (!scanDevices()) {
				continue;
			}
		}
		else if (deviceChanges > 0) {
			deviceChanges = 0;
			mutex.unlock();
			if (!scanDevices()) {
				continue;
			}
		}
		else {
			mutex.unlock();
		}

		/* Bluetooth heartbeat to prevent Bluetooth from turning off */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (last_event.tv_sec > 0 &&
		    now.tv_sec - last_event.tv_sec < BLUETOOTH_TIMEOUT) {
			if (bluetoothHeartbeat == NULL) {
				bluetoothHeartbeat = alloca(1024);
				new(bluetoothHeartbeat) QObject();
				BluetoothHeartbeat(bluetoothHeartbeat, 0);
			}
			BluetoothHeartbeat_beat(bluetoothHeartbeat);
		}

		/* Poll devices for input events */
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);

		foreach(const Device &device, devices.values()) {
			FD_SET(device.fd, &rfds);
		}

		ret = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
		if (ret <= 0) {
			continue;
		}

		/* Process events for each device */
		for (auto it = devices.begin(); it != devices.end();) {
			Device &device = it.value();

			if (!FD_ISSET(device.fd, &rfds)) {
				it++;
				continue;
			}

			ret = read(device.fd, &e, sizeof(e));
			if (ret < (int)sizeof(e)) {
				nh_log("lost device");
				it = devices.erase(it);
				continue;
			}

            /* Logic to handle Long vs Short press */
            /* If this is a key press (value 1), just record the time */
            if (e.value == 1) {
                device.pressTimes[e.code] = e.time;
                // We typically don't trigger actions on press for page turns,
                // so we can choose to continue or let it check TYPE_ANY rules.
                // For this implementation, we allow immediate triggers only if configured.
            }

            /* Calculate duration if this is a release (value 0) */
            long duration_ms = 0;
            if (e.value == 0 && device.pressTimes.contains(e.code)) {
                struct timeval start = device.pressTimes.take(e.code);
                /* Calculate difference in milliseconds */
                duration_ms = (e.time.tv_sec - start.tv_sec) * 1000 +
                              (e.time.tv_usec - start.tv_usec) / 1000;
            }

            for (int i = 0; i < device.cfg.size(); i++) {
                ConfigRule &rule = device.cfg[i];
                struct input_event test = rule.trigger;

                /* Check if event matches config */
                if (e.type == test.type &&
                    e.code == test.code &&
                    e.value == test.value) {

                    /* Check Long/Short Logic */
                    bool isPhysicalLong = (duration_ms > LONG_PRESS_THRESHOLD_MS);

                    /* If config demands LONG but press was short, skip */
                    if (rule.type == TYPE_LONG && !isPhysicalLong) continue;

                    /* If config demands SHORT but press was long, skip */
                    if (rule.type == TYPE_SHORT && isPhysicalLong) continue;

                    /* Match confirmed */

                    /* Update Bluetooth heartbeat time */
                    clock_gettime(CLOCK_MONOTONIC,
                                  &last_event);

                    /* Update PowerManager::timeLastUsed */
                    emit notify();

                    /* Invoke the configured method */
                    const char *method = rule.method.toStdString().c_str();
                    invokeMainWindowController(method);
                }
            }
			it++;
		}
	}
}

void TimeLastUsedUpdater::notify()
{
	/* Update PowerManager::timeLastUsed to prevent sleep */
	QEvent timeEvent(TimeEvent_eventType());

	QObject *pm = PowerManager_sharedInstance();
	if (pm == NULL) {
		nh_log("invalid PowerManager");
	}

	PowerManager_filter(pm, this, &timeEvent);
}

static int btpt_init()
{
	mkdir(BTPT_DIR, 0755);
	TimeLastUsedUpdater *timeLastUsedUpdater = new TimeLastUsedUpdater();
	BluetoothPageTurner *btpt = new BluetoothPageTurner();
	QObject::connect(
		btpt, &BluetoothPageTurner::notify,
		timeLastUsedUpdater, &TimeLastUsedUpdater::notify,
		Qt::QueuedConnection);
	QObject::connect(
		btpt, &QThread::finished,
		btpt, &QObject::deleteLater);
	QObject::connect(
		btpt, &QThread::finished,
		timeLastUsedUpdater, &QObject::deleteLater);
	btpt->start();

	return 0;
}

static struct nh_info btpt_info = {
	.name           = "BluetoothPageTurner",
	.desc           = "Turn pages with Bluetooth device",
	.uninstall_flag = BTPT_DIR "/uninstall",
};

static struct nh_hook btpt_hook[] = {
	{0},
};

static struct nh_dlsym btpt_dlsym[] = {
	{
		.name = "_ZN18BluetoothHeartbeatC1Ex",
		.out = nh_symoutptr(BluetoothHeartbeat)
	},
	{
		.name = "_ZN18BluetoothHeartbeat4beatEv",
		.out = nh_symoutptr(BluetoothHeartbeat_beat)
	},
	{
		.name = "_ZN20MainWindowController14sharedInstanceEv",
		.out = nh_symoutptr(MainWindowController_sharedInstance)
	},
	{
		.name = "_ZNK20MainWindowController11currentViewEv",
		.out = nh_symoutptr(MainWindowController_currentView)
	},
	{
		.name = "_ZN9TimeEvent9eventTypeEv",
		.out = nh_symoutptr(TimeEvent_eventType)
	},
	{
		.name = "_ZN12PowerManager14sharedInstanceEv",
		.out = nh_symoutptr(PowerManager_sharedInstance)
	},
	{
		.name = "_ZN12PowerManager6filterEP7QObjectP6QEvent",
		.out = nh_symoutptr(PowerManager_filter)
	},
	{0},
};

NickelHook(
	.init  = btpt_init,
	.info  = &btpt_info,
	.hook  = btpt_hook,
	.dlsym = btpt_dlsym,
)
