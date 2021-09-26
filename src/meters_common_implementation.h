/*
 Copyright (C) 2018-2020 Fredrik Öhrström

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

#ifndef METERS_COMMON_IMPLEMENTATION_H_
#define METERS_COMMON_IMPLEMENTATION_H_

#include"meters.h"
#include"units.h"

#include<map>
#include<set>

struct MeterCommonImplementation : public virtual Meter
{
    int index();
    void setIndex(int i);
    string bus();
    vector<string>& ids();
    string idsc();
    vector<string>  fields();
    vector<Print>   prints();
    string name();
    MeterDriver driver();

    ELLSecurityMode expectedELLSecurityMode();
    TPLSecurityMode expectedTPLSecurityMode();

    string datetimeOfUpdateHumanReadable();
    string datetimeOfUpdateRobot();
    string unixTimestampOfUpdate();

    void onUpdate(function<void(Telegram*,Meter*)> cb);
    int numUpdates();

    static bool isTelegramForMeter(Telegram *t, Meter *meter, MeterInfo *mi);
    MeterKeys *meterKeys();

    std::vector<std::string> getRecords();
    double getRecordAsDouble(std::string record);
    uint16_t getRecordAsUInt16(std::string record);

    MeterCommonImplementation(MeterInfo &mi, MeterDriver driver);

    ~MeterCommonImplementation() = default;

    string meterDriver() { return toString(driver_); }

protected:

    void triggerUpdate(Telegram *t);
    void setExpectedELLSecurityMode(ELLSecurityMode dsm);
    void setExpectedTPLSecurityMode(TPLSecurityMode tsm);
    void addConversions(std::vector<Unit> cs);
    void addShell(std::string cmdline);
    void addExtraConstantField(std::string ecf);
    std::vector<std::string> &shellCmdlines();
    std::vector<std::string> &meterExtraConstantFields();
    void addLinkMode(LinkMode lm);
    // Print with the default unit for this quantity.
    void addPrint(string vname, Quantity vquantity,
                  function<double(Unit)> getValueFunc, string help, bool field, bool json);
    // Print with exactly this unit for this quantity.
    void addPrint(string vname, Quantity vquantity, Unit unit,
                  function<double(Unit)> getValueFunc, string help, bool field, bool json);
    // Print the dimensionless Text quantity, no unit is needed.
    void addPrint(string vname, Quantity vquantity,
                  function<std::string()> getValueFunc, string help, bool field, bool json);
    // The default implementation of poll does nothing.
    // Override for mbus meters that need to be queried and likewise for C2/T2 wmbus-meters.
    void poll(shared_ptr<BusManager> bus);
    bool handleTelegram(AboutTelegram &about, vector<uchar> frame, bool simulated, string *id, bool *id_match);
    void printMeter(Telegram *t,
                    string *human_readable,
                    string *fields, char separator,
                    string *json,
                    vector<string> *envs,
                    vector<string> *more_json, // Add this json "key"="value" strings.
                    vector<string> *selected_fields); // Only print these fields.
    // Json fields cannot be modified expect by adding conversions.
    // Json fields include all values except timestamp_ut, timestamp_utc, timestamp_lt
    // since Json is assumed to be decoded by a program and the current timestamp which is the
    // same as timestamp_utc, can always be decoded/recoded into local time or a unix timestamp.

    virtual void processContent(Telegram *t) = 0;

private:

    int index_ {};
    MeterDriver driver_ {};
    string bus_ {};
    MeterKeys meter_keys_ {};
    ELLSecurityMode expected_ell_sec_mode_ {};
    TPLSecurityMode expected_tpl_sec_mode_ {};
    string name_;
    vector<string> ids_;
    string idsc_;
    vector<function<void(Telegram*,Meter*)>> on_update_;
    int num_updates_ {};
    time_t datetime_of_update_ {};
    LinkModeSet link_modes_ {};
    vector<string> shell_cmdlines_;
    vector<string> extra_constant_fields_;

protected:
    std::map<std::string,std::pair<int,std::string>> values_;
    vector<Unit> conversions_;
    vector<Print> prints_;
    vector<string> fields_;
};

#endif
