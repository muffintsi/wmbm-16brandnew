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

struct MeterLansenPU : public virtual PulseCounter, public virtual MeterCommonImplementation {
    MeterLansenPU(MeterInfo &mi);

    double counterA();
    double counterB();

private:

    void processContent(Telegram *t);

    private:

    double pulse_counter_a_ {};
    double pulse_counter_b_ {};

};

MeterLansenPU::MeterLansenPU(MeterInfo &mi) :
    MeterCommonImplementation(mi, MeterDriver::LANSENPU)
{
    setExpectedTPLSecurityMode(TPLSecurityMode::AES_CBC_IV);

    // version 0x14 for which we have a test telegram
    // other version 0x0b reported, but we lack telegram
    addLinkMode(LinkMode::T1);

    addPrint("counter_a", Quantity::Counter,
             [&](Unit u) { assertQuantity(u, Quantity::Counter); return counterA(); },
             "The current number of counted pulses from counter a.",
             true, true);

    addPrint("counter_b", Quantity::Counter,
             [&](Unit u) { assertQuantity(u, Quantity::Counter); return counterB(); },
             "The current number of counted pulses from counter b.",
             true, true);
}

shared_ptr<PulseCounter> createLansenPU(MeterInfo &mi)
{
    return shared_ptr<PulseCounter>(new MeterLansenPU(mi));
}

double MeterLansenPU::counterA()
{
    // Doubles have a 52 bit significand 11 bit exp and 1 bit sign,
    // so double is good for incremental pulses up to 2^52 counts
    // which is approx 4.5*10^15
    // The values sent by this meter are 12 digit bcd, ie at most 10^13-1 counts.
    // so they fit inside a double.
    return (double)pulse_counter_a_;
}

double MeterLansenPU::counterB()
{
    return (double)pulse_counter_b_;
}

void MeterLansenPU::processContent(Telegram *t)
{
    /*
      (wmbus) 11: 0E dif (12 digit BCD Instantaneous value)
      (wmbus) 12: FD vif (Second extension of VIF-codes)
      (wmbus) 13: 3A vife (Dimensionless / no VIF)
      (wmbus) 14: 000000000000
      (wmbus) 1a: 8E dif (12 digit BCD Instantaneous value)
      (wmbus) 1b: 40 dife (subunit=1 tariff=0 storagenr=0)
      (wmbus) 1c: FD vif (Second extension of VIF-codes)
      (wmbus) 1d: 3A vife (Dimensionless / no VIF)
      (wmbus) 1e: 000000000000
    */
    int offset;

    extractDVdouble(&t->values, "0EFD3A", &offset, &pulse_counter_a_, false);
    t->addMoreExplanation(offset, " pulse counter a (%f)", pulse_counter_a_);
    extractDVdouble(&t->values, "8E40FD3A", &offset, &pulse_counter_b_, false);
    t->addMoreExplanation(offset, " pulse counter b (%f)", pulse_counter_b_);
}
