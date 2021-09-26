/*
 Copyright (C) 2020 Fredrik Öhrström

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
#include"units.h"
#include"util.h"

struct MeterCompact5 : public virtual HeatMeter, public virtual MeterCommonImplementation
{
    MeterCompact5(MeterInfo &mi);

    double totalEnergyConsumption(Unit u);
    double currentPeriodEnergyConsumption(Unit u);
    double previousPeriodEnergyConsumption(Unit u);

    private:

    void processContent(Telegram *t);

    double total_energy_kwh_ {};
    double curr_energy_kwh_ {};
    double prev_energy_kwh_ {};
};

shared_ptr<HeatMeter> createCompact5(MeterInfo &mi)
{
    return shared_ptr<HeatMeter>(new MeterCompact5(mi));
}

MeterCompact5::MeterCompact5(MeterInfo &mi) :
    MeterCommonImplementation(mi, MeterDriver::COMPACT5)
{
    // media 0x04 is used for C telegrams
    // media 0xC3 is used for T telegrams

    addLinkMode(LinkMode::C1);
    addLinkMode(LinkMode::T1);

    addPrint("total", Quantity::Energy,
             [&](Unit u){ return totalEnergyConsumption(u); },
             "The total energy consumption recorded by this meter.",
             true, true);

    addPrint("current", Quantity::Energy,
             [&](Unit u){ return currentPeriodEnergyConsumption(u); },
             "Energy consumption so far in this billing period.",
             true, true);

    addPrint("previous", Quantity::Energy,
             [&](Unit u){ return previousPeriodEnergyConsumption(u); },
             "Energy consumption in previous billing period.",
             true, true);
}

double MeterCompact5::totalEnergyConsumption(Unit u)
{
    assertQuantity(u, Quantity::Energy);
    return convert(total_energy_kwh_, Unit::KWH, u);
}

double MeterCompact5::currentPeriodEnergyConsumption(Unit u)
{
    assertQuantity(u, Quantity::Energy);
    return convert(curr_energy_kwh_, Unit::KWH, u);
}

double MeterCompact5::previousPeriodEnergyConsumption(Unit u)
{
    assertQuantity(u, Quantity::Energy);
    return convert(prev_energy_kwh_, Unit::KWH, u);
}

void MeterCompact5::processContent(Telegram *t)
{
    // Unfortunately, the Techem Compact V is mostly a proprieatary protocol
    // simple wrapped inside a wmbus telegram since the ci-field is 0xa2.
    // Which means that the entire payload is manufacturer specific.

    map<string,pair<int,DVEntry>> vendor_values;
    vector<uchar> content;

    t->extractPayload(&content);
    uchar prev_lo = content[3];
    uchar prev_hi = content[4];
    double prev = (256.0*prev_hi+prev_lo);

    string prevs;
    strprintf(prevs, "%02x%02x", prev_lo, prev_hi);
    int offset = t->parsed.size()+3;
    vendor_values["0215"] = { offset, DVEntry(MeasurementType::Instantaneous, 0x15, 0, 0, 0, prevs) };
    t->explanations.push_back({ offset, prevs });
    t->addMoreExplanation(offset, " energy used in previous billing period (%f KWH)", prev);

    uchar curr_lo = content[7];
    uchar curr_hi = content[8];
    double curr = (256.0*curr_hi+curr_lo);

    string currs;
    strprintf(currs, "%02x%02x", curr_lo, curr_hi);
    offset = t->parsed.size()+7;
    vendor_values["0215"] = { offset, DVEntry(MeasurementType::Instantaneous, 0x15, 0, 0, 0, currs) };
    t->explanations.push_back({ offset, currs });
    t->addMoreExplanation(offset, " energy used in current billing period (%f KWH)", curr);

    total_energy_kwh_ = prev+curr;
    curr_energy_kwh_ = curr;
    prev_energy_kwh_ = prev;
}
