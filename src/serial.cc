/*
 Copyright (C) 2017-2020 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include"util.h"
#include"rtlsdr.h"
#include"serial.h"
#include"shell.h"
#include"threads.h"
#include"timings.h"

#include <algorithm>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <libgen.h>
#include <memory.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/serial.h>
#endif

static int openSerialTTY(const char *tty, int baud_rate, PARITY parity);
static string showTTYSettings(int fd);

struct SerialDeviceImp;
struct SerialDeviceTTY;
struct SerialDeviceCommand;
struct SerialDeviceFile;
struct SerialDeviceSimulator;
struct Timer
{
    int id;
    int seconds;
    time_t last_call;
    function<void()> callback;
    string name;

    bool isTime(time_t now)
    {
        return (last_call+seconds) <= now;
    }
};

struct SerialCommunicationManagerImp : public SerialCommunicationManager
{
    SerialCommunicationManagerImp(time_t exit_after_seconds, bool start_event_loop);
    ~SerialCommunicationManagerImp();

    shared_ptr<SerialDevice> createSerialDeviceTTY(string dev, int baud_rate, PARITY parity, string purpose);
    shared_ptr<SerialDevice> createSerialDeviceCommand(string identifier, string command, vector<string> args,
                                                       vector<string> envs, string purpose);
    shared_ptr<SerialDevice> createSerialDeviceFile(string file, string purpose);
    shared_ptr<SerialDevice> createSerialDeviceSimulator();

    void listenTo(SerialDevice *sd, function<void()> cb);
    void onDisappear(SerialDevice *sd, function<void()> cb);

    void expectDevicesToWork();
    void stop();
    void startEventLoop();
    void waitForStop();
    bool isRunning();

    shared_ptr<SerialDevice> addSerialDeviceForManagement(SerialDevice *sd);
    void tickleEventLoop();
    void removeNonWorkingSerialDevices();
    void closeAllDoNotRemove();

    int startRegularCallback(string name, int seconds, function<void()> callback);
    void stopRegularCallback(int id);

    vector<string> listSerialTTYs();
    shared_ptr<SerialDevice> lookup(std::string device);
    bool removeNonWorking(std::string device);

private:

    void *eventLoop();
    void *timerLoop();

    void executeTimerCallbacks();
    time_t calculateTimeToNearestTimerCallback(time_t now);

    bool running_ {};
    bool expect_devices_to_work_ {}; // false during detection phase, true when running.
    time_t start_time_ {};
    time_t exit_after_seconds_ {};

    vector<shared_ptr<SerialDevice>> serial_devices_;
    RecursiveMutex serial_devices_mutex_ = { "serial_devices_mutex" };
#define LOCK_SERIAL_DEVICES(where) WITH(serial_devices_mutex_, serial_devices_mutex, where)

    RecursiveMutex event_loop_mutex_ = {"event_loop_mutex" };
#define LOCK_EVENT_LOOP(where) WITH(event_loop_mutex_, event_loop_mutex, where)

    vector<Timer> timers_;  // Protected by LOCK_TIMERS
    RecursiveMutex timers_mutex_ = { "timers_mutex" };
#define LOCK_TIMERS(where) WITH(timers_mutex_, timers_mutex, where)
};

SerialCommunicationManagerImp::~SerialCommunicationManagerImp()
{
    // Stop the loop.
    stop();
    // Grab the event_loop_lock. This can only be done when the eventLoop has stopped running.
    event_loop_mutex_.lock();
    // Close all managed devices (not yet closed)
    closeAllDoNotRemove();
    // Remove all closed devices.
    removeNonWorkingSerialDevices();
    // Now we can be sure the eventLoop has stopped and it is safe to
    // free this Manager object.
}

struct SerialDeviceImp : public SerialDevice
{
    void disableCallbacks() { no_callbacks_ = true; }
    void enableCallbacks() { no_callbacks_ = false; }
    bool skippingCallbacks() { return no_callbacks_; }
    void fill(vector<uchar> &data) {};
    int receive(vector<uchar> *data);
    bool waitFor(uchar c);
    bool working() { return resetting_ || fd_ != -1; }
    bool resetting() { return resetting_; }
    bool opened() { return resetting_ || fd_ != -2; }
    bool isClosed() { return fd_ == -1 && !resetting_; }
    bool readonly() { return is_stdin_ || is_file_; }

    void expectAscii() { expecting_ascii_ = true; }
    void setIsFile() { is_file_ = true; }
    void setIsStdin() { is_stdin_ = true; }
    string device() { return ""; }
    int fd() { return fd_; }
    SerialCommunicationManager *manager() { return manager_; }
    void resetInitiated() { debug("(serial) initiate reset\n"); resetting_ = true; }
    void resetCompleted() { debug("(serial) reset completed\n"); resetting_ = false; }
    bool checkIfDataIsPending()
    {
        if (!opened() || !working()) return false; // No data can be pending if device is not opened nor working.
        int available = -1;
        int rc = ioctl(fd_, FIONREAD, &available);
        if (rc == -1) return false;
        return available > 0;
    }

    SerialDeviceImp(SerialCommunicationManagerImp *manager, string purpose)
    {
        manager_ = manager;
        purpose_ = purpose;
    }
    ~SerialDeviceImp() = default;

protected:

    RecursiveMutex read_mutex_ = { "read_mutex" };
#define LOCK_READ_SERIAL(where) WITH(read_mutex_, read_mutex, where)

    RecursiveMutex write_mutex_ = { "write_mutex" };
#define LOCK_WRITE_SERIAL(where) WITH(write_mutex_, write_mutex, where)

    function<void()> on_data_;
    function<void()> on_disappear_;
    int fd_ = -2; // -2 not yet opened, -1 not working
    bool expecting_ascii_ {}; // If true, print using safeString instead if bin2hex
    bool is_file_ = false;
    bool is_stdin_ = false;
    // When feeding from stdin, to prevent early exit, we want
    // at least some data before leaving the loop!
    // I.e. do not exit before we have received something!
    bool no_callbacks_ = false;
    SerialCommunicationManagerImp *manager_;
    bool resetting_ {}; // Set to true while resetting.
    string purpose_; // Can be set to identify a serial device purose.

    friend struct SerialCommunicationManagerImp;
};

bool SerialDeviceImp::waitFor(uchar c)
{
    vector<uchar> data;
    for (;;)
    {
        int n = receive(&data);
        if (n == 0) break;
        if (data.back() == c) return true;
    }
    return false;
}

int SerialDeviceImp::receive(vector<uchar> *data)
{
    LOCK_READ_SERIAL(receive);

    bool close_me = false;

    data->clear();
    int num_read = 0;

    while (true)
    {
        data->resize(num_read+1024);
        int nr = read(fd_, &((*data)[num_read]), 1024);
        if (nr > 0)
        {
            num_read += nr;
        }
        if (nr == 0)
        {
            if (is_file_)
            {
                debug("(serial) no more data on file fd=%d\n", fd_);
                close_me = true;
            }
            if (is_stdin_)
            {
                if (getchar() == EOF)
                {
                    debug("(serial) no more data on stdin fd=%d\n", fd_);
                    close_me = true;
                }
            }
            break;
        }
        if (nr < 0)
        {
            if (errno == EINTR && fd_ != -1) continue; // Interrupted try again.
            if (errno == EAGAIN) break;   // No more data available since it would block.
            if (errno == EBADF)
            {
                debug("(serial) got EBADF for fd=%d closing it.\n", fd_);
                close_me = true;
                break;
            }
            break;
        }
    }
    data->resize(num_read);

    if (isDebugEnabled())
    {
        if (expecting_ascii_)
        {
            string msg = safeString(*data);
            debug("(serial) received ascii \"%s\"\n", msg.c_str());
        }
        else
        {
            string msg = bin2hex(*data);
            debug("(serial) received binary \"%s\"\n", msg.c_str());
        }
    }

    if (close_me) close();

    return num_read;
}

struct SerialDeviceTTY : public SerialDeviceImp
{
    SerialDeviceTTY(string device, int baud_rate, PARITY parity, SerialCommunicationManagerImp * manager, string purpose);
    ~SerialDeviceTTY();

    AccessCheck open(bool fail_if_not_ok);
    void close();
    bool send(vector<uchar> &data);
    bool working();
    string device() { return device_; }

    private:

    string device_;
    int baud_rate_ {};
    PARITY parity_ {};
};

SerialDeviceTTY::SerialDeviceTTY(string device, int baud_rate, PARITY parity,
                                 SerialCommunicationManagerImp *manager,
                                 string purpose)
    : SerialDeviceImp(manager, purpose)
{
    device_ = device;
    baud_rate_ = baud_rate;
    parity_ = parity;
}

SerialDeviceTTY::~SerialDeviceTTY()
{
    close();
}

AccessCheck SerialDeviceTTY::open(bool fail_if_not_ok)
{
    assert(device_ != "");
    bool ok = checkCharacterDeviceExists(device_.c_str(), fail_if_not_ok);
    if (!ok) return AccessCheck::NotThere;
    fd_ = openSerialTTY(device_.c_str(), baud_rate_, parity_);
    if (fd_ < 0)
    {
        if (fail_if_not_ok)
        {
            if (fd_ == -1)
            {
                error("Could not open %s with %d baud N81\n", device_.c_str(), baud_rate_);
            }
            else if (fd_ == -2)
            {
                error("Device %s is already in use and locked.\n", device_.c_str());
            }
        }
        else
        {
            if (fd_ == -1)
            {
                return AccessCheck::NotThere;
            }
            else if (fd_ == -2)
            {
                return AccessCheck::NotSameGroup;
            }
        }
    }
    verbose("(serialtty) opened %s fd %d (%s)\n", device_.c_str(), fd_, purpose_.c_str());
    return AccessCheck::AccessOK;
}

void SerialDeviceTTY::close()
{
    if (fd_ == -1) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
    if (on_disappear_ && !resetting_)
    {
        on_disappear_();
        on_disappear_ = NULL;
    }
    manager_->tickleEventLoop();

    verbose("(serialtty) closed %s (%s)\n", device_.c_str(), purpose_.c_str());
}

bool SerialDeviceTTY::send(vector<uchar> &data)
{
    LOCK_WRITE_SERIAL(send);

    assert(data.size() > 0);

    bool rc = true;
    int n = data.size();
    int written = 0;
    while (true)
    {
        int nw = write(fd_, &data[written], n-written);
        if (nw > 0) written += nw;
        if (nw < 0)
        {
            if (errno==EINTR) continue;
            rc = false;
            if (isDebugEnabled()) {
                string msg = bin2hex(data);
                debug("(serial %s) failed to send \"%s\"\n", device_.c_str(), msg.c_str());
            }
            goto end;
        }
        if (written == n) break;
    }

    if (isDebugEnabled()) {
        string msg = bin2hex(data);
        debug("(serial %s) sent \"%s\"\n", device_.c_str(), msg.c_str());
    }

    if (signalsInstalled())
    {
        if (getEventLoopThread()) pthread_kill(getEventLoopThread(), SIGUSR1);
    }

    end:
    return rc;
}

bool SerialDeviceTTY::working()
{
    if (resetting_) return true;
    if (fd_ == -1) return false;

    bool working = checkCharacterDeviceExists(device_.c_str(), false);

    if (!working) {
        debug("(serial) device %s is gone\n", device_.c_str());
    }

    return working;
}


struct SerialDeviceCommand : public SerialDeviceImp
{
    SerialDeviceCommand(string identifier, string command, vector<string> args, vector<string> envs,
                        SerialCommunicationManagerImp *manager,
                        string purpose);
    ~SerialDeviceCommand();

    AccessCheck open(bool fail_if_not_ok);
    void close();
    bool send(vector<uchar> &data);
    int available();
    bool working();
    string device() { return identifier_; }
    string command() { return command_; }

    private:

    string identifier_;
    string command_;
    int pid_ {};
    vector<string> args_;
    vector<string> envs_;

    pthread_mutex_t write_lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t read_lock_ = PTHREAD_MUTEX_INITIALIZER;
};

SerialDeviceCommand::SerialDeviceCommand(string identifier,
                                         string command,
                                         vector<string> args,
                                         vector<string> envs,
                                         SerialCommunicationManagerImp *manager,
                                         string purpose)
    : SerialDeviceImp(manager, purpose)
{
    assert(identifier != "");
    identifier_ = identifier;
    command_ = command;
    args_ = args;
    envs_ = envs;
}

SerialDeviceCommand::~SerialDeviceCommand()
{
    close();
}

AccessCheck SerialDeviceCommand::open(bool fail_if_not_ok)
{
    expectAscii();
    bool ok = invokeBackgroundShell("/bin/sh", args_, envs_, &fd_, &pid_);
    assert(fd_ >= 0);
    if (!ok) return AccessCheck::NotThere;
    setIsStdin();
    verbose("(serialcmd) opened %s pid %d fd %d (%s)\n", command_.c_str(), pid_, fd_, purpose_.c_str());
    return AccessCheck::AccessOK;
}

void SerialDeviceCommand::close()
{
    int p = pid_, f = fd_;
    if (pid_ == 0 && fd_ == -1) return;
    if (pid_ && stillRunning(pid_))
    {
        stopBackgroundShell(pid_);
        pid_ = 0;
    }
    if (on_disappear_ && !resetting_)
    {
        on_disappear_();
        on_disappear_ = NULL;
    }
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;

    manager_->tickleEventLoop();

    verbose("(serialcmd) closed %s pid=%d fd=%d (%s)\n", command_.c_str(), p, f, purpose_.c_str());
}

bool SerialDeviceCommand::working()
{
    if (resetting_) return true;
    if (fd_ == -1) return false;
    int n = -1;
    int rc = ioctl(fd_, FIONREAD, &n);
    if (rc != 0) return false;
    // There is still data available for reading,
    // even though the child-process might have ended.
    if (n > 0) return true;

    // No data and no pid. For sure its not working.
    if (!pid_) return false;
    // Ok check the pid, still running?
    bool r = stillRunning(pid_);
    if (r) return true;
    return false;
}

bool SerialDeviceCommand::send(vector<uchar> &data)
{
    LOCK_WRITE_SERIAL(sendcmd);

    assert(data.size() > 0);

    bool rc = true;
    int n = data.size();
    int written = 0;
    while (true) {
        int nw = write(fd_, &data[written], n-written);
        if (nw > 0) written += nw;
        if (nw < 0) {
            if (errno==EINTR) continue;
            rc = false;
            goto end;
        }
        if (written == n) break;
    }

    if (isDebugEnabled()) {
        string msg = bin2hex(data);
        debug("(serial %s) sent \"%s\"\n", command_.c_str(), msg.c_str());
    }

    end:
    return rc;
}

struct SerialDeviceFile : public SerialDeviceImp
{
    SerialDeviceFile(string file, SerialCommunicationManagerImp *manager, string purpose);
    ~SerialDeviceFile();

    AccessCheck open(bool fail_if_not_ok);
    void close();
    bool working();
    bool send(vector<uchar> &data);
    int available();
    string device() { return file_; }

    private:

    string file_;
};

SerialDeviceFile::SerialDeviceFile(string file,
                                   SerialCommunicationManagerImp *manager,
                                   string purpose)
    : SerialDeviceImp(manager, purpose)
{
    file_ = file;
}

SerialDeviceFile::~SerialDeviceFile()
{
    close();
}

AccessCheck SerialDeviceFile::open(bool fail_if_not_ok)
{
    if (file_ == "stdin")
    {
        fd_ = 0;
        int flags = fcntl(0, F_GETFL);
        flags |= O_NONBLOCK;
        fcntl(0, F_SETFL, flags);
        setIsStdin();
        verbose("(serialfile) reading from stdin (%s)\n", purpose_.c_str());
    }
    else
    {
        bool ok = checkFileExists(file_.c_str());
        if (!ok) return AccessCheck::NotThere;
        fd_ = ::open(file_.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd_ == -1)
        {
            if (fail_if_not_ok)
            {
                error("Could not open file %s for reading.\n", file_.c_str());
            }
            else
            {
                return AccessCheck::NotThere;
            }
        }
        setIsFile();
        verbose("(serialfile) reading from file %s (%s)\n", file_.c_str(), purpose_.c_str());
    }
    manager_->tickleEventLoop();

    return AccessCheck::AccessOK;
}

void SerialDeviceFile::close()
{
    if (fd_ == -1) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;

    manager_->tickleEventLoop();

    verbose("(serialfile) closed %s %d (%s)\n", file_.c_str(), fd_, purpose_.c_str());
}

bool SerialDeviceFile::working()
{
    if (resetting_) return true;
    if (fd_ == -1) return false;
    int n = -1;
    int rc = ioctl(fd_, FIONREAD, &n);
    // The file descriptor was bad.
    if (rc != 0) return false;

    // There is still data available for reading.
    if (n > 0) return true;

    // Oh it is still open, lets continue use it.
    // This could be stdin for example.
    return true;
}

bool SerialDeviceFile::send(vector<uchar> &data)
{
    return true;
}

struct SerialDeviceSimulator : public SerialDeviceImp
{
    SerialDeviceSimulator(SerialCommunicationManagerImp *m, string purpose) :
        SerialDeviceImp(m, purpose)
    {
        verbose("(serialsimulator) opened (%s)\n", purpose_.c_str());
    };
    ~SerialDeviceSimulator() { };

    AccessCheck open(bool fail_if_not_ok) { return AccessCheck::AccessOK; };
    void close() { };
    bool readonly() { return true; }
    bool send(vector<uchar> &data) { return true; };
    void fill(vector<uchar> &data) { data_ = data; on_data_(); }; // Fill buffer and trigger callback.

    int receive(vector<uchar> *data)
    {
        *data = data_;
        data_.clear();
        return data->size();
    }
    int available() { return data_.size(); }
    int fd() { return -1; }
    bool working() { return false; } // Only one message that has already been handled! So return false here.

    private:

    vector<uchar> data_;
};

SerialCommunicationManagerImp::SerialCommunicationManagerImp(time_t exit_after_seconds,
                                                             bool start_event_loop)
{
    running_ = true;
    // Block the event loop until everything is configured.
    if (start_event_loop)
    {
        event_loop_mutex_.lock();
        startEventLoopThread(call(this, eventLoop));
        startTimerLoopThread(call(this, timerLoop));
    }
    wakeMeUpOnSigChld(getEventLoopThread());
    start_time_ = time(NULL);
    exit_after_seconds_ = exit_after_seconds;
}

shared_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceTTY(string device,
                                                                              int baud_rate,
                                                                              PARITY parity,
                                                                              string purpose)
{
    return addSerialDeviceForManagement(new SerialDeviceTTY(device, baud_rate, parity, this, purpose));
}

shared_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceCommand(string identifier,
                                                                                  string command,
                                                                                  vector<string> args,
                                                                                  vector<string> envs,
                                                                                  string purpose)
{
    return addSerialDeviceForManagement(new SerialDeviceCommand(identifier, command, args, envs, this, purpose));
}

shared_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceFile(string file, string purpose)
{
    return addSerialDeviceForManagement(new SerialDeviceFile(file, this, purpose));
}

shared_ptr<SerialDevice> SerialCommunicationManagerImp::createSerialDeviceSimulator()
{
    return addSerialDeviceForManagement(new SerialDeviceSimulator(this, ""));
}

void SerialCommunicationManagerImp::listenTo(SerialDevice *sd, function<void()> cb)
{
    if (sd == NULL) return;
    SerialDeviceImp *si = dynamic_cast<SerialDeviceImp*>(sd);
    if (!si)
    {
        error("Internal error: Invalid serial device passed to listenTo.\n");
    }
    si->on_data_ = cb;
}

void SerialCommunicationManagerImp::onDisappear(SerialDevice *sd, function<void()> cb)
{
    if (sd == NULL) return;
    SerialDeviceImp *si = dynamic_cast<SerialDeviceImp*>(sd);
    if (!si)
    {
        error("Internal error: Invalid serial device passed to onDisappear.\n");
    }
    si->on_disappear_ = cb;
}

void SerialCommunicationManagerImp::expectDevicesToWork()
{
    debug("(serial) expecting devices to work\n");
    expect_devices_to_work_ = true;
}

void SerialCommunicationManagerImp::stop()
{
    // Notify the main waitForStop thread that we are stopped!
    if (running_ == true)
    {
        debug("(serial) stopping manager\n");
        running_ = false;
        if (getMainThread() != 0)
        {
            if (signalsInstalled())
            {
                if (getMainThread()) pthread_kill(getMainThread(), SIGUSR2);
                if (getEventLoopThread()) pthread_kill(getEventLoopThread(), SIGUSR1);
                if (getTimerLoopThread()) pthread_kill(getTimerLoopThread(), SIGUSR1);
            }
        }
    }
}

void SerialCommunicationManagerImp::startEventLoop()
{
    // Release the event loop!
    event_loop_mutex_.unlock();
}

void SerialCommunicationManagerImp::waitForStop()
{
    debug("(serial) waiting for stop\n");

    recordMyselfAsMainThread();
    while (running_)
    {
        {
            LOCK_SERIAL_DEVICES(wait_for_stop);
            if (serial_devices_.size() == 0) break;
        }
        int rc = usleep(1000*1000);
        if (rc == -1 && errno == EINTR)
        {
            debug("(serial) MAIN thread interrupted\n");
            continue;
        }
    }

    closeAllDoNotRemove();

    if (signalsInstalled())
    {
        if (getEventLoopThread()) pthread_kill(getEventLoopThread(), SIGUSR1);
        if (getTimerLoopThread()) pthread_kill(getTimerLoopThread(), SIGUSR1);
    }

    pthread_join(getEventLoopThread(), NULL);
    pthread_join(getTimerLoopThread(), NULL);
}

bool SerialCommunicationManagerImp::isRunning()
{
    return running_;
}

shared_ptr<SerialDevice> SerialCommunicationManagerImp::addSerialDeviceForManagement(SerialDevice *sd)
{
    LOCK_SERIAL_DEVICES(opened);

    shared_ptr<SerialDevice> ptr = shared_ptr<SerialDevice>(sd);
    serial_devices_.push_back(ptr);

    tickleEventLoop();
    return ptr;
}

void SerialCommunicationManagerImp::tickleEventLoop()
{
    LOCK_SERIAL_DEVICES(tickle);

    if (signalsInstalled())
    {
        // Tickle the event loop to use the new file descriptor in the select.
        if (getEventLoopThread()) pthread_kill(getEventLoopThread(), SIGUSR1);
    }
}

void SerialCommunicationManagerImp::removeNonWorkingSerialDevices()
{
    LOCK_SERIAL_DEVICES(remove_non_working_serial_devices);

    for (auto i = serial_devices_.begin(); i != serial_devices_.end(); )
    {
        if ((*i)->opened() && !(*i)->working())
        {
            i = serial_devices_.erase(i);
        }
        else
        {
            i++;
        }
    }

    if (serial_devices_.size() == 0 && expect_devices_to_work_)
    {
        debug("(serial) no devices working emergency exit!\n");
        stop();
    }
}

void SerialCommunicationManagerImp::closeAllDoNotRemove()
{
    LOCK_SERIAL_DEVICES(close_all_do_not_remove);

    if (serial_devices_.size() == 0) return;

    debug("(serial) closing %d devices\n", serial_devices_.size());

    for (auto i = serial_devices_.begin(); i != serial_devices_.end(); i++)
    {
        (*i)->close();
    }
}

void SerialCommunicationManagerImp::executeTimerCallbacks()
{
    time_t curr = time(NULL);
    vector<Timer> to_be_called;

    {
        LOCK_TIMERS(execute_timer_callbacks);

        for (Timer &t : timers_)
        {
            if (t.isTime(curr))
            {
                trace("[SERIAL] timer isTime! %d %s\n", t.id, t.name.c_str());
                t.last_call = curr;
                to_be_called.push_back(t);
            }
        }
    }

    for (Timer &t : to_be_called)
    {
        trace("[SERIAL] invoking callback %s(%d)\n", t.name.c_str(), t.id);
        t.callback();
    }
}

time_t SerialCommunicationManagerImp::calculateTimeToNearestTimerCallback(time_t now)
{
    time_t r = 1024*1024*1024;
    for (Timer &t : timers_)
    {
        // Expected time when to trigger in the future.
        // Well, could be in the past as well, if we are unlucky.
        time_t done = t.last_call+t.seconds;
        // Now how long time is it left....could be negative
        // if we are late.
        time_t remaining = done-now;
        if (remaining < r) r = remaining;
    }
    return r;
}

void *SerialCommunicationManagerImp::timerLoop()
{
    while (running_)
    {
        int rc = usleep(1000*1000);
        if (rc == -1 && errno == EINTR)
        {
            debug("(serial) TIMER thread interrupted\n");
            continue;
        }

        time_t curr = time(NULL);

        if (exit_after_seconds_ > 0)
        {
            time_t diff = curr-start_time_;
            if (diff > exit_after_seconds_)
            {
                // Running time limit hit, now stop.
                verbose("(serial) exit after %ld seconds\n", diff);
                stop();
                break;
            }
        }

        executeTimerCallbacks();
    }
    return NULL;
}

void *SerialCommunicationManagerImp::eventLoop()
{
    LOCK_EVENT_LOOP(eventLoop);

    fd_set readfds;

    while (running_)
    {
        FD_ZERO(&readfds);

        bool all_working = true;

        {
            LOCK_SERIAL_DEVICES(list_file_descriptiors_to_listen_to);

            for (shared_ptr<SerialDevice> &sd : serial_devices_)
            {
                if (sd->opened() && sd->working() && !sd->skippingCallbacks())
                {
                    if (!sd->resetting() && sd->fd() >= 0)
                    {
                        trace("[SERIAL] select read on fd %d\n", sd->fd());
                        FD_SET(sd->fd(), &readfds);
                    }
                }
                if (sd->opened() && !sd->working()) all_working = false;
            }
        }

        if (!all_working && expect_devices_to_work_)
        {
            debug("(serial) not all devices working, emergency exit!\n");
            stop();
            break;
        }

        // Perform a select call every second.
        struct timeval timeout { 1, 0 };

        trace("[SERIAL] select timeout %d s\n", timeout.tv_sec);

        int max_fd = 0;
        for (shared_ptr<SerialDevice> &sp : serial_devices_)
        {
            if (sp->fd() > max_fd)
            {
                max_fd = sp->fd();
            }
        }

        int activity = select(max_fd+1 , &readfds, NULL , NULL, &timeout);

        if (activity == -1 && errno == EINTR)
        {
            debug("(serial) EVENT thread interrupted\n");
        }
        if (!running_) break;
        if (activity < 0 && errno!=EINTR)
        {
            warning("(serial) internal error after select! errno=%s\n", strerror(errno));
        }

        if (activity > 0)
        {
            // Something has happened that caused the sleeping select to wake up.
            vector<shared_ptr<SerialDevice>> to_be_notified;
            {
                LOCK_SERIAL_DEVICES(find_triggering_file_descriptions);

                for (shared_ptr<SerialDevice> &sd : serial_devices_)
                {
                    if (sd->opened() && sd->working() && !sd->resetting() && sd->fd() >= 0)
                    {
                        if (FD_ISSET(sd->fd(), &readfds))
                        {
                            trace("[SERIAL] select detected data available for reading on fd %d\n", sd->fd());
                            to_be_notified.push_back(sd);
                        }
                    }
                }
            }

            for (shared_ptr<SerialDevice> &sd : to_be_notified)
            {
                SerialDeviceImp *si = dynamic_cast<SerialDeviceImp*>(sd.get());
                if (si->on_data_)
                {
                    si->on_data_();
                }
            }
        }

        vector<shared_ptr<SerialDevice>> non_working;
        {
            LOCK_SERIAL_DEVICES(find_non_working_serial_devices);

            for (shared_ptr<SerialDevice> &sd : serial_devices_)
            {
                if (sd->opened() && !sd->working() && !sd->isClosed()) non_working.push_back(sd);
            }
        }

        for (shared_ptr<SerialDevice> &sd : non_working)
        {
            debug("(serial) closing non working fd=%d \"%s\"\n", sd->fd(), sd->device().c_str());
            sd->close();
        }

        removeNonWorkingSerialDevices();

        if (non_working.size() > 0 && expect_devices_to_work_)
        {
            debug("(serial) non working devices found, exiting.\n");
            stop();
            break;
        }
    }
    verbose("(serial) event loop stopped!\n");

    return NULL;
}

shared_ptr<SerialCommunicationManager> createSerialCommunicationManager(time_t exit_after_seconds,
                                                                        bool start_event_loop)
{
    return shared_ptr<SerialCommunicationManager>(new SerialCommunicationManagerImp(exit_after_seconds,
                                                                                    start_event_loop));
}

static int openSerialTTY(const char *tty, int baud_rate, PARITY parity)
{
    int rc = 0;
    speed_t speed = 0;
    struct termios tios;
    string tty_info;
    //int DTR_flag = TIOCM_DTR;

    int fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd == -1) {
        usleep(1000*1000);
        fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd == -1) goto err;
    }
    rc = flock(fd, LOCK_EX | LOCK_NB);
    if (rc == -1)
    {
        // It is already locked by another wmbusmeter process.
        fd = -2;
        goto err;
    }

    tty_info = showTTYSettings(fd);
    debug("(serial) before config: %s %s\n",  tty, tty_info.c_str());

    switch (baud_rate)
    {
    case 300:   speed = B300;  break;
    case 600:   speed = B600;  break;
    case 1200:   speed = B1200;  break;
    case 2400:   speed = B2400;  break;
    case 4800:   speed = B4800;  break;
    case 9600:   speed = B9600;  break;
    case 19200:  speed = B19200; break;
    case 38400:  speed = B38400; break;
    case 57600:  speed = B57600; break;
    case 115200: speed = B115200;break;
    default:
        goto err;
    }

    memset(&tios, 0, sizeof(tios));

    rc = cfsetispeed(&tios, speed);
    if (rc < 0) goto err;
    rc = cfsetospeed(&tios, speed);
    if (rc < 0) goto err;


    // CREAD=Enable receive CLOCAL=Ignore any Carrier Detect signal.
    tios.c_cflag |= (CREAD | CLOCAL);
    tios.c_cflag &= ~CSIZE;
    tios.c_cflag |= CS8;
    tios.c_cflag &=~ CSTOPB;
    if (parity == PARITY::NONE)
    {
        // Disable parity bit.
        tios.c_cflag &=~ PARENB;
    }
    else if (parity == PARITY::EVEN)
    {
        // Enable parity even, ie not odd.
        tios.c_cflag |= PARENB;
        tios.c_cflag &=~ PARODD;
    }
    else if (parity == PARITY::ODD)
    {
        // Enable parity odd.
        tios.c_cflag |= PARENB;
        tios.c_cflag |= PARODD;
    }
    else
    {
        assert(0);
    }

    tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tios.c_iflag &= ~INPCK;

    tios.c_iflag &= ~(IXON | IXOFF | IXANY);

    tios.c_oflag &=~ OPOST;
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;

    rc = tcsetattr(fd, TCSANOW, &tios);
    if (rc < 0) goto err;


    // This code can toggle DTR... maybe necessary
    // for the pl2303 usb2serial driver/device.
    //rc = ioctl(fd, TIOCMBIC, &DTR_flag);
    //if (rc != 0) goto err;

    tty_info = showTTYSettings(fd);
    debug("(serial) after config:  %s %s\n",  tty, tty_info.c_str());

    return fd;

err:
    if (fd > 0)
    {
        close(fd);
        fd = -1;
    }
    return fd;
}

SerialCommunicationManager::~SerialCommunicationManager()
{
}

int SerialCommunicationManagerImp::startRegularCallback(string name, int seconds, function<void()> callback)
{
    LOCK_TIMERS(start_regular_callback);

    Timer t = { (int)timers_.size(), seconds, time(NULL), callback, name };
    timers_.push_back(t);
    debug("(serial) registered regular callback %s(%d) every %d seconds\n", name.c_str(), t.id, seconds);

    return t.id;
}

void SerialCommunicationManagerImp::stopRegularCallback(int id)
{
    LOCK_TIMERS(stop_regular_callback);

    debug("(serial) stopping regular callback %d\n", id);
    for (auto i = timers_.begin(); i != timers_.end(); ++i)
    {
        if ((*i).id == id)
        {
            timers_.erase(i);
            break;
        }
    }
}


shared_ptr<SerialDevice> SerialCommunicationManagerImp::lookup(string device)
{
    LOCK_SERIAL_DEVICES(lookup);

    for (auto sd : serial_devices_)
    {
        if (sd->device() == device) return shared_ptr<SerialDevice>(sd);
    }
    return NULL;
}

bool SerialCommunicationManagerImp::removeNonWorking(string device)
{
    LOCK_SERIAL_DEVICES(remove_non_working);

    bool found_and_removed = false;
    for (auto i = serial_devices_.begin(); i != serial_devices_.end(); )
    {
        if ((*i)->opened() && !(*i)->working() && (*i)->device() == device)
        {
            i = serial_devices_.erase(i);
            found_and_removed = true;
        }
        else
        {
            i++;
        }
    }

    return found_and_removed;
}


#if defined(__APPLE__) || defined(__FreeBSD__)
vector<string> SerialCommunicationManagerImp::listSerialTTYs()
{
    vector<string> list;
    list.push_back("Please add code here!");
    return list;
}
#endif

#if defined(__linux__)

static string lookup_device_driver(string tty)
{
    struct stat st;
    tty += "/device";
    int rc = lstat(tty.c_str(), &st);
    if (rc==0 && S_ISLNK(st.st_mode))
    {
        tty += "/driver";
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));
        int rc = readlink(tty.c_str(), buffer, sizeof(buffer));
        if (rc > 0)
        {
            return basename(buffer);
        }
    }
    return "";
}

static void check_if_serial(string tty, vector<string> *found_serials, vector<string> *found_8250s)
{
    string driver = lookup_device_driver(tty);

    if (driver.size() > 0)
    {
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));
        strncpy(buffer, tty.c_str(), sizeof(buffer)-1);

        string dev = buffer;

        if (driver == "serial8250")
        {
            found_8250s->push_back(dev);
        }
        else
        {
            // The dev is now something like: /sys/class/tty/ttyUSB0
            // Drop the /sys/class/tty/ prefix and replace with /dev/
            if (dev.rfind("/sys/class/tty/", 0) == 0) {
                dev = string("/dev/")+dev.substr(15);
            }
            found_serials->push_back(dev);
        }
    }
}

static void check_serial8250s(vector<string> *found_serials, vector<string> &found_8250s)
{
    struct serial_struct serinfo;

    for (string dev : found_8250s)
    {
        int fd = open(dev.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);

        if (fd >= 0)
        {
            int rc = ioctl(fd, TIOCGSERIAL, &serinfo);
            if (rc == 0)
            {
                if (serinfo.type != PORT_UNKNOWN)
                {
                    found_serials->push_back(dev);
                }
                close(fd);
            }
        }
    }
}

int sorty(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

vector<string> SerialCommunicationManagerImp::listSerialTTYs()
{
    struct dirent **entries;
    vector<string> found_serials;
    vector<string> found_8250s;
    string sysdir = "/sys/class/tty/";

    int n = scandir(sysdir.c_str(), &entries, NULL, sorty);
    if (n < 0)
    {
        perror("scandir");
        return found_serials;
    }

    for (int i=0; i<n; ++i)
    {
        string name = entries[i]->d_name;

        if (name ==  ".." || name == ".")
        {
            free(entries[i]);
            continue;
        }

        string tty = sysdir+name;
        check_if_serial(tty, &found_serials, &found_8250s);
        free(entries[i]);
    }
    free(entries);

    check_serial8250s(&found_serials, found_8250s);

    return found_serials;
}

#endif

#define CHECK_SPEED(x) { if (speed == x) return #x; }

string translateSpeed(speed_t speed)
{
    string flags;

	CHECK_SPEED(B50)
	CHECK_SPEED(B75)
	CHECK_SPEED(B110)
	CHECK_SPEED(B134)
	CHECK_SPEED(B150)
	CHECK_SPEED(B200)
	CHECK_SPEED(B300)
	CHECK_SPEED(B600)
	CHECK_SPEED(B1200)
	CHECK_SPEED(B1800)
	CHECK_SPEED(B2400)
	CHECK_SPEED(B4800)
	CHECK_SPEED(B9600)
#ifdef B57600
	CHECK_SPEED(B57600)
#endif
#ifdef B115200
	CHECK_SPEED(B115200)
#endif
	CHECK_SPEED(B19200)
#ifdef B230400
	CHECK_SPEED(B230400)
#endif
	CHECK_SPEED(B38400)
#ifdef B460800
	CHECK_SPEED(B460800)
#endif
#ifdef B500000
	CHECK_SPEED(B500000)
#endif
#ifdef B57600
	CHECK_SPEED(B57600)
#endif
#ifdef B921600
	CHECK_SPEED(B921600)
#endif
#ifdef B1000000
	CHECK_SPEED(B1000000)
#endif
#ifdef B1152000
	CHECK_SPEED(B1152000)
#endif
#ifdef B1500000
	CHECK_SPEED(B1500000)
#endif
#ifdef B2000000
	CHECK_SPEED(B2000000)
#endif
#ifdef B2500000
	CHECK_SPEED(B2500000)
#endif
#ifdef B3000000
	CHECK_SPEED(B3000000)
#endif
#ifdef B3500000
	CHECK_SPEED(B3500000)
#endif
#ifdef B4000000
	CHECK_SPEED(B4000000)
#endif

    return "UnknownSpeed";
};

#undef CHECK_SPEED

string lookupSpeed(struct termios *tios)
{
    speed_t in = cfgetispeed(tios);
    speed_t out = cfgetispeed(tios);

    if (in == out)
    {
        return translateSpeed(in);
    }

    return translateSpeed(in)+","+translateSpeed(out);
}

#define CHECK_FLAG(x) { if (bits & x) flags += #x "|"; }

string iflags(tcflag_t bits)
{
    string flags;

    CHECK_FLAG(BRKINT)
	CHECK_FLAG(ICRNL)
	CHECK_FLAG(IGNBRK)
	CHECK_FLAG(IGNCR)
	CHECK_FLAG(IGNPAR)
#ifdef IMAXBEL
	CHECK_FLAG(IMAXBEL)
#endif
	CHECK_FLAG(INLCR)
	CHECK_FLAG(ISTRIP)
#ifdef IUTF8
	CHECK_FLAG(IUTF8)
#endif
	CHECK_FLAG(IXANY)
	CHECK_FLAG(IXOFF)
	CHECK_FLAG(IXON)
	CHECK_FLAG(PARMRK)

    if (flags.length() > 0) flags.pop_back();
    return flags;
};

string oflags(tcflag_t bits)
{
    string flags;

#ifdef BS1
	CHECK_FLAG(BS1)
#endif
#ifdef NL1
	CHECK_FLAG(NL1)
#endif
	CHECK_FLAG(ONLCR)
#ifdef ONOEOT
	CHECK_FLAG(ONOEOT)
#endif
	CHECK_FLAG(OPOST)
#ifdef OXTABS
	CHECK_FLAG(OXTABS)
#endif

    if (flags.length() > 0) flags.pop_back();
    return flags;
};

string cflags(tcflag_t bits)
{
    string flags;

	CHECK_FLAG(CLOCAL)
	CHECK_FLAG(CREAD)
	CHECK_FLAG(CSIZE)
	CHECK_FLAG(CSTOPB)
	CHECK_FLAG(HUPCL)
    CHECK_FLAG(PARENB)
    CHECK_FLAG(PARODD)

    if (flags.length() > 0) flags.pop_back();
    return flags;
};

string lflags(tcflag_t bits)
{
    string flags;

	CHECK_FLAG(ECHO)
	CHECK_FLAG(ECHOCTL)
	CHECK_FLAG(ECHOE)
	CHECK_FLAG(ECHOK)
	CHECK_FLAG(ECHOKE)
	CHECK_FLAG(ECHONL)
	CHECK_FLAG(ECHOPRT)
	CHECK_FLAG(FLUSHO)
	CHECK_FLAG(ICANON)
	CHECK_FLAG(IEXTEN)
	CHECK_FLAG(ISIG)
	CHECK_FLAG(NOFLSH)
	CHECK_FLAG(PENDIN)
	CHECK_FLAG(TOSTOP)
#ifdef XCASE
	CHECK_FLAG(XCASE)
#endif

    if (flags.length() > 0) flags.pop_back();
    return flags;
};

#undef CHECK_FLAG

string showSpecialChars(struct termios *tios)
{
    string s;

    int n = sizeof(tios->c_cc)/sizeof(cc_t);
	for (int i=0; i<n; ++i)
    {
        cc_t c = tios->c_cc[i];
        if (c != 0)
        {
            string cc;
            strprintf(cc, "%u", c);
            s += cc+",";
        }
    }
    if (s.length() > 0) s.pop_back();
    return s;
}

static string showTTYSettings(int fd)
{
    string info;
    string bits;

    struct termios tios;
    int modem_bits;

    int rc = tcgetattr(fd, &tios);
    if (rc != 0) goto err;

    info += "speed("+lookupSpeed(&tios)+") ";
    info += "input("+iflags(tios.c_iflag) + ") ";
    info += "output("+oflags(tios.c_oflag) + ") ";
    info += "control("+cflags(tios.c_cflag) + ") ";
    info += "local("+lflags(tios.c_lflag) + ") ";
    info += "special_chars("+showSpecialChars(&tios)+") ";

    rc = ioctl(fd, TIOCMGET, &modem_bits);
    if (rc != 0) goto err;

    if (modem_bits & TIOCM_LE) bits += "LE|"; // Line Enabled (same as DSR below?)
    if (modem_bits & TIOCM_DTR) bits += "DTR|"; // Data Terminal Ready (Computer is ready.)
    if (modem_bits & TIOCM_RTS) bits += "RTS|"; // Request to send (hardware flow control)
    if (modem_bits & TIOCM_ST) bits += "ST|";
    if (modem_bits & TIOCM_SR) bits += "SR|";
    if (modem_bits & TIOCM_CTS) bits += "CTS|"; // Clear to Send (hardware flow control)
    if (modem_bits & TIOCM_CD) bits += "CD|"; // Data Carrier Detected (Modem is connected to another modem)
    if (modem_bits & TIOCM_RI) bits += "RING|"; // Ring Indicator
    if (modem_bits & TIOCM_DSR) bits += "DSR|"; // Data Set Ready (Modem is ready.)

    if (bits.length() > 0) bits.pop_back();
    info += "modem("+bits+")";
    return info;

err:

    return "error";
}
