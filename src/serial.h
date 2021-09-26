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

#ifndef SERIAL_H_
#define SERIAL_H_

#include"util.h"

#include<functional>
#include<memory>
#include<string>
#include<vector>

using namespace std;

struct SerialCommunicationManager;

enum class PARITY { NONE, EVEN, ODD };

/**
  A SerialDevice can be connected to a tty with a baudrate.
  But can also be connected to stdin, a file, or the output from a subshell.
  If you try to do send bytes to such a non-tty, then send will return false.
*/
struct SerialDevice
{
    // If fail_if_not_ok then forcefully exit the program if cannot be opened.
    virtual AccessCheck open(bool fail_if_not_ok) = 0;
    virtual void close() = 0;
    // Explicitly closed fd == -1
    virtual bool isClosed() = 0;
    // Send will return true only if sending on a tty.
    virtual bool send(std::vector<uchar> &data) = 0;
    // Receive returns the number of bytes received.
    virtual int receive(std::vector<uchar> *data) = 0;
    // Read and skip until the desired character is found
    // and no further bytes can be read.
    virtual bool waitFor(uchar c) = 0;
    virtual int fd() = 0;
    virtual bool opened() = 0;
    virtual bool working() = 0;
    virtual bool resetting() = 0; // The serial device is working but can lack a valid file descriptor.
    // Used when connecting stdin to a tty driver for testing.
    virtual bool readonly() = 0;
    // Mark this device so that it is ignored by the select/callback event loop.
    virtual void disableCallbacks() = 0;
    // Enable this device to trigger callbacks from the event loop.
    virtual void enableCallbacks() = 0;
    virtual bool skippingCallbacks() = 0;

    // Return underlying device as string.
    virtual std::string device() = 0;

    virtual bool checkIfDataIsPending() = 0;
    virtual void fill(std::vector<uchar> &data) = 0; // Fill buffer with raw data.
    virtual SerialCommunicationManager *manager() = 0;
    virtual void resetInitiated() = 0;
    virtual void resetCompleted() = 0;

    virtual ~SerialDevice() = default;
};

struct SerialCommunicationManager
{
    // Read from a /dev/ttyUSB0 or /dev/ttyACM0 device with baud settings.
    virtual shared_ptr<SerialDevice> createSerialDeviceTTY(string dev, int baud_rate, PARITY parity, string purpose) = 0;
    // Read from a sub shell.
    virtual shared_ptr<SerialDevice> createSerialDeviceCommand(string identifier,
                                                               string command,
                                                               vector<string> args,
                                                               vector<string> envs,
                                                               string purpose) = 0;
    // Read from stdin (file="stdin") or a specific file.
    virtual shared_ptr<SerialDevice> createSerialDeviceFile(string file, string purpose) = 0;
    // A serial device simulator used for internal testing.
    virtual shared_ptr<SerialDevice> createSerialDeviceSimulator() = 0;

    // Invoke cb callback when data arrives on the serial device.
    virtual void listenTo(SerialDevice *sd, function<void()> cb) = 0;
    // Invoke cb callback when the serial device has disappeared!
    virtual void onDisappear(SerialDevice *sd, function<void()> cb) = 0;
    // Normally the communication mananager runs for ever.
    // But if you expect configured devices to work, then
    // the manager will exit when there are no working devices.
    virtual void expectDevicesToWork() = 0;
    virtual void stop() = 0;
    virtual void startEventLoop() = 0;
    virtual void waitForStop() = 0;
    virtual bool isRunning() = 0;
    // Register a new timer that regularly, every seconds, invokes the callback.
    // Returns an id for the timer.
    virtual int startRegularCallback(std::string name, int seconds, function<void()> callback) = 0;
    virtual void stopRegularCallback(int id) = 0;

    // List all real serial devices (avoid pseudo ttys)
    virtual std::vector<std::string> listSerialTTYs() = 0;
    // Return a serial device for the given device, if it exists! Otherwise NULL.
    virtual std::shared_ptr<SerialDevice> lookup(std::string device) = 0;
    // Remove a closed device, returns false and do not remove, if the device is still in use.
    virtual bool removeNonWorking(std::string device) = 0;
    virtual ~SerialCommunicationManager();
};

shared_ptr<SerialCommunicationManager> createSerialCommunicationManager(time_t exit_after_seconds,
                                                                        bool start_event_loop);

#endif
