/*
 Copyright (C) 2021 Fredrik Öhrström

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

#include"wmbus.h"
#include"wmbus_common_implementation.h"
#include"wmbus_utils.h"
#include"serial.h"

#include<assert.h>
#include<pthread.h>
#include<semaphore.h>
#include<errno.h>
#include<unistd.h>

using namespace std;

struct MBusRawTTY : public virtual WMBusCommonImplementation
{
    bool ping();
    string getDeviceId();
    string getDeviceUniqueId();
    LinkModeSet getLinkModes();
    void deviceReset();
    void deviceSetLinkModes(LinkModeSet lms);
    LinkModeSet supportedLinkModes() { return Any_bit; }
    int numConcurrentLinkModes() { return 0; }
    bool canSetLinkModes(LinkModeSet desired_modes) { return true; }
    bool sendTelegram(ContentStartsWith starts_with, vector<uchar> &content);

    void processSerialData();
    void simulate() { }

    MBusRawTTY(string alias, shared_ptr<SerialDevice> serial, shared_ptr<SerialCommunicationManager> manager);
    ~MBusRawTTY() { }

private:

    vector<uchar> read_buffer_;
    LinkModeSet link_modes_;
    vector<uchar> received_payload_;
};

shared_ptr<WMBus> openMBUS(Detected detected, shared_ptr<SerialCommunicationManager> manager, shared_ptr<SerialDevice> serial_override)
{
    string bus_alias  = detected.specified_device.bus_alias;
    string device = detected.found_file;
    int bps = detected.found_bps;

    assert(device != "");

    if (serial_override)
    {
        MBusRawTTY *imp = new MBusRawTTY(bus_alias, serial_override, manager);
        imp->markAsNoLongerSerial();
        return shared_ptr<WMBus>(imp);
    }
    auto serial = manager->createSerialDeviceTTY(device.c_str(), bps, PARITY::EVEN, "mbus");
    MBusRawTTY *imp = new MBusRawTTY(bus_alias, serial, manager);
    return shared_ptr<WMBus>(imp);
}

MBusRawTTY::MBusRawTTY(string bus_alias, shared_ptr<SerialDevice> serial, shared_ptr<SerialCommunicationManager> manager) :
    WMBusCommonImplementation(bus_alias, DEVICE_MBUS, manager, serial, true)
{
    reset();
}

bool MBusRawTTY::ping()
{
    return true;
}

string MBusRawTTY::getDeviceId()
{
    return "?";
}

string MBusRawTTY::getDeviceUniqueId()
{
    return "?";
}

LinkModeSet MBusRawTTY::getLinkModes() {
    return link_modes_;
}

void MBusRawTTY::deviceReset()
{
}

void MBusRawTTY::deviceSetLinkModes(LinkModeSet lms)
{
}

void MBusRawTTY::processSerialData()
{
    vector<uchar> data;

    // Receive and accumulated serial data until a full frame has been received.
    serial()->receive(&data);

    read_buffer_.insert(read_buffer_.end(), data.begin(), data.end());

    size_t frame_length;
    int payload_len, payload_offset;

    for (;;)
    {
        FrameStatus status = checkMBusFrame(read_buffer_, &frame_length, &payload_len, &payload_offset);

        if (status == PartialFrame)
        {
            // Partial frame, stop eating.
            break;
        }
        if (status == ErrorInFrame)
        {
            verbose("(mbus) protocol error in message received!\n");
            string msg = bin2hex(read_buffer_);
            debug("(mbus) protocol error \"%s\"\n", msg.c_str());
            read_buffer_.clear();
            break;
        }
        if (status == FullFrame)
        {
            vector<uchar> payload;
            if (payload_len > 0)
            {
                uchar l = payload_len;
                payload.insert(payload.end(), &l, &l+1); // Re-insert the len byte.
                payload.insert(payload.end(), read_buffer_.begin()+payload_offset, read_buffer_.begin()+payload_offset+payload_len);
            }
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin()+frame_length);
            AboutTelegram about("", 0, FrameType::MBUS);
            handleTelegram(about, payload);
        }
    }
}

bool MBusRawTTY::sendTelegram(ContentStartsWith starts_with, vector<uchar> &content)
{
    if (serial()->readonly()) return true;
    if (content.size() > 250) return false;

    vector<uchar> msg;
    if (starts_with == ContentStartsWith::SHORT_FRAME)
    {
        msg.push_back(0x10);
    }
    else if (starts_with == ContentStartsWith::LONG_FRAME)
    {
        msg.push_back(0x68);
        msg.push_back((uchar)content.size());
        msg.push_back((uchar)content.size());
        msg.push_back(0x68);
    }
    else
    {
        warning("(mbus) cannot use %s for sending\n", toString(starts_with));
        return false;
    }

    uchar crc = 0;
    for (size_t i=0; i<content.size(); ++i)
    {
        msg.push_back(content[i]);
        crc += content[i];
    }
    msg.push_back(crc);
    msg.push_back(0x16);

    bool sent = serial()->send(msg);
    if (!sent) return false;

    return true;
}

AccessCheck detectMBUS(Detected *detected, shared_ptr<SerialCommunicationManager> manager)
{
    string tty = detected->specified_device.file;
    int bps = atoi(detected->specified_device.bps.c_str());

    // Since we do not know how to talk to the other end, it might not
    // even respond. The only thing we can do is to try to open the serial device.
    auto serial = manager->createSerialDeviceTTY(tty.c_str(), bps, PARITY::EVEN, "detect mbus");
    AccessCheck rc = serial->open(false);
    if (rc != AccessCheck::AccessOK) return AccessCheck::NotThere;

    serial->close();

    detected->setAsFound("", WMBusDeviceType::DEVICE_MBUS, bps, false,
        detected->specified_device.linkmodes);

    return AccessCheck::AccessOK;
}
