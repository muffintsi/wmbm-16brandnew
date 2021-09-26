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

#include"dvparser.h"
#include"meters.h"
#include"meters_common_implementation.h"
#include"wmbus.h"
#include"wmbus_utils.h"
#include"util.h"

#include<assert.h>

using namespace std;


struct MeterUnknown : public virtual UnknownMeter, public virtual MeterCommonImplementation {
    MeterUnknown(MeterInfo &mi);

    string meter_info_;
    void processContent(Telegram *t);
};

MeterUnknown::MeterUnknown(MeterInfo &mi) :
    MeterCommonImplementation(mi, MeterDriver::AUTO)
{
    addPrint("meter_info", Quantity::Text,
             [&](){ return meter_info_; },
             "Information about the meter telegram.",
             true, true);
}

shared_ptr<UnknownMeter> createUnknown(MeterInfo &mi)
{
    return shared_ptr<UnknownMeter>(new MeterUnknown(mi));
}

void MeterUnknown::processContent(Telegram *t)
{
}
