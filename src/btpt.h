#ifndef BTPT_H
#define BTPT_H

#include <linux/input.h>
#include <linux/uinput.h>

#include <QFileSystemWatcher>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QThread>
#include <QWaitCondition>

/* NEW: Define the types of presses we support */
enum PressType {
    TYPE_ANY = 0,
    TYPE_SHORT,
    TYPE_LONG
};

/* NEW: A struct to hold the configuration for a single action */
struct ConfigRule {
    struct input_event trigger; /* The event code (type, code, value) */
    QString method;             /* The function name to call */
    PressType type;             /* Short, Long, or Any */
};

class Device
{
public:
    int fd;

    /* CHANGED: Use our new ConfigRule instead of QPair */
    QList<ConfigRule> cfg;

    /* NEW: Track the timestamp of when a key was pressed down (value=1) */
    /* Key: Event Code (e.g., KEY_A), Value: Timestamp */
    QMap<uint16_t, struct timeval> pressTimes;
};

class BluetoothPageTurner : public QThread
{
	Q_OBJECT

	void run() override;

private:
	bool addDevice(
		const QString &name,
		const QString &uniq,
		const QString &handler);
	bool scanDevices();
	void learn(Device &device);
	QFileSystemWatcher watcher;
	QMap<QString, Device> devices;
	QMutex mutex;
	QWaitCondition newDevice;
	int deviceChanges = 0;

public slots:
	void directoryChanged(const QString &path);

signals:
	void notify();
};

class TimeLastUsedUpdater : public QObject
{
	Q_OBJECT

public slots:
	void notify();
};

#endif /* BTPT_H */
