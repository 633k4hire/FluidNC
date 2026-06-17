// Copyright (c) 2020 Mitch Bradley
// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

/*
  WebCommands.cpp - Settings and Commands for the interface
  to ESP3D_WebUI.  Code snippets extracted from commands.cpp in the
  old WebUI interface code are presented via the Settings class.
*/

#include "Settings.h"
#include "Machine/MachineConfig.h"
#include "Configuration/JsonGenerator.h"
#include "Report.h"  // git_info
#include "GCode.h"
#include "Lathe.h"
#include "Spindles/Spindle.h"

#include <Esp.h>

#include <sstream>
#include <iomanip>
#include <cstdlib>

#include "Module.h"

namespace WebUI {

    class WebCommands : public Module {
    public:
        WebCommands(const char* name) : Module(name) {}

        static std::string LeftJustify(const char* s, size_t width) {
            std::string ret(s);

            for (size_t l = ret.length(); width > l; width--) {
                ret += ' ';
            }
            return ret;
        }

        // Used by js/connectdlg.js

#ifdef ENABLE_AUTHENTICATION
        static Error setUserPassword(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP555
            if (*parameter == '\0') {
                user_password->setDefault();
                return Error::Ok;
            }
            if (user_password->setStringValue(parameter) != Error::Ok) {
                log_string(out, "Invalid Password");
                return Error::InvalidValue;
            }
            return Error::Ok;
        }
#endif

        static Error restart(const char* parameter, AuthenticationLevel auth_level, Channel& out) {
            log_info("Restarting");
            protocol_send_event(&fullResetEvent);
            return Error::Ok;
        }

        // used by js/restartdlg.js
        static Error setSystemMode(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP444
            // parameter = trim(parameter);
            if (strcasecmp(parameter, "RESTART") != 0) {
                log_string(out, "Parameter must be RESTART");
                return Error::InvalidValue;
            }
            return restart(parameter, auth_level, out);
        }

        // Used by js/statusdlg.js
        static Error showSysStatsJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP420

            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "420");
            j.member("status", "ok");
            j.begin_array("data");

            j.id_value_object("Chip ID", (uint16_t)(ESP.getEfuseMac() >> 32));
            j.id_value_object("CPU Cores", ESP.getChipCores());

            std::ostringstream msg;
            msg << ESP.getCpuFreqMHz() << "Mhz";
            j.id_value_object("CPU Frequency", msg.str());

            std::ostringstream msg2;
            msg2 << std::fixed << std::setprecision(1) << temperatureRead() << "°C";
            j.id_value_object("CPU Temperature", msg2.str());

            j.id_value_object("Free memory", formatBytes(ESP.getFreeHeap()));
            j.id_value_object("SDK", ESP.getSdkVersion());
            j.id_value_object("Flash Size", formatBytes(ESP.getFlashChipSize()));

            for (auto const& module : ModuleFactory::objects()) {
                module->wifi_stats(j);
            }

            std::string s("FluidNC ");
            s += git_info;
            j.id_value_object("FW version", s);

            j.end_array();
            j.end();
            return Error::Ok;
        }

        static void send_json_command_response(Channel& out, uint cmdID, bool isok, const std::string& message) {
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", String(cmdID).c_str());
            j.member("status", isok ? "ok" : "error");
            j.member("data", message);
            j.end();
        }


        static std::string float_string(float value, int precision = 3) {
            std::ostringstream msg;
            msg << std::fixed << std::setprecision(precision) << value;
            return msg.str();
        }

        static const char* lathe_spindle_mode_name() {
            return gc_state.modal.lathe_spindle_speed_mode == Lathe::SpindleSpeedMode::ConstantSurfaceSpeed ? "G96 CSS" : "G97 fixed RPM";
        }

        static const char* lathe_diameter_mode_name() {
            return gc_state.modal.lathe_diameter_mode == Lathe::DiameterMode::Diameter ? "G7 diameter" : "G8 radius";
        }

        static const char* feed_mode_name() {
            switch (gc_state.modal.feed_rate) {
                case FeedRate::InverseTime:
                    return "G93 inverse time";
                case FeedRate::UnitsPerRev:
                    return "G95 units/rev";
                case FeedRate::UnitsPerMin:
                    return "G94 units/min";
            }
            return "unknown";
        }

        static Error showLatheStatusJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP421
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "421");
            j.member("status", "ok");
            j.begin_array("data");

            j.id_value_object("Lathe enabled", Lathe::enabled() ? "true" : "false");
            j.id_value_object("Spindle speed mode", lathe_spindle_mode_name());
            j.id_value_object("Diameter mode", lathe_diameter_mode_name());
            j.id_value_object("Feed mode", feed_mode_name());
            j.id_value_object("Programmed S", float_string(gc_state.spindle_speed));
            j.id_value_object("Effective RPM", float_string(gc_state.lathe_commanded_rpm));
            j.id_value_object("CSS clamp RPM", float_string(Lathe::max_css_rpm()));
            j.id_value_object("Minimum CSS diameter mm", float_string(Lathe::min_css_diameter_mm()));
            j.id_value_object("Encoder enabled", Lathe::encoder_enabled() ? "true" : "false");
            j.id_value_object("Encoder pulses/rev", int32_t(Lathe::encoder_pulses_per_revolution()));

            auto tool = Lathe::active_tool_offset();
            j.id_value_object("Active lathe tool", tool.valid ? int32_t(tool.tool_number) : int32_t(0));
            j.id_value_object("Lathe tool X offset mm", float_string(tool.x_mm));
            j.id_value_object("Lathe tool Z offset mm", float_string(tool.z_mm));
            j.id_value_object("Tool nose radius mm", float_string(tool.nose_radius_mm));

            const auto feedback = spindle->latheFeedback().status();
            j.id_value_object("Feedback measured RPM", feedback.has_measured_rpm ? float_string(feedback.measured_rpm) : "not available");
            j.id_value_object("Feedback index", feedback.has_index_pulse ? "true" : "false");
            j.id_value_object("Feedback angular position", feedback.has_angular_position ? "true" : "false");
            j.id_value_object("Feedback stale", feedback.stale ? "true" : "false");
            j.id_value_object("Feedback fault", feedback.fault ? "true" : "false");

            j.end_array();
            j.end();
            return Error::Ok;
        }

        static bool get_float_param(const char* parameter, const char* key, float& value) {
            std::string text;
            if (!get_param(parameter, key, text)) {
                return false;
            }
            char* end = nullptr;
            value = strtof(text.c_str(), &end);
            return end != text.c_str();
        }

        static bool get_uint_param(const char* parameter, const char* key, uint32_t& value) {
            std::string text;
            if (!get_param(parameter, key, text)) {
                return false;
            }
            char* end = nullptr;
            value = strtoul(text.c_str(), &end, 10);
            return end != text.c_str();
        }

        static Error setLatheToolJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP422
            uint32_t tool_number = 0;
            if (!get_uint_param(parameter, "T=", tool_number) || tool_number == 0) {
                send_json_command_response(out, 422, false, errorString(Error::InvalidValue));
                return Error::InvalidValue;
            }

            Lathe::ToolData tool;
            if (auto existing = Lathe::get_tool_data(tool_number)) {
                tool = *existing;
            }

            get_float_param(parameter, "GX=", tool.geometry_x_mm);
            get_float_param(parameter, "GZ=", tool.geometry_z_mm);
            get_float_param(parameter, "WX=", tool.wear_x_mm);
            get_float_param(parameter, "WZ=", tool.wear_z_mm);
            get_float_param(parameter, "NR=", tool.nose_radius_mm);

            uint32_t orientation = 0;
            if (get_uint_param(parameter, "O=", orientation)) {
                tool.orientation = static_cast<Lathe::InsertOrientation>(orientation);
            }

            Lathe::set_tool_data(tool_number, tool);
            send_json_command_response(out, 422, true, "lathe tool saved");
            return Error::Ok;
        }

        static Error touchOffLatheToolJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP423
            Lathe::TouchOffSpec spec;
            if (!get_uint_param(parameter, "T=", spec.tool_number) || spec.tool_number == 0) {
                send_json_command_response(out, 423, false, errorString(Error::InvalidValue));
                return Error::InvalidValue;
            }

            spec.set_x = get_float_param(parameter, "MX=", spec.machine_x_mm) && get_float_param(parameter, "RX=", spec.reference_x_mm);
            spec.set_z = get_float_param(parameter, "MZ=", spec.machine_z_mm) && get_float_param(parameter, "RZ=", spec.reference_z_mm);

            std::string mode;
            if (get_param(parameter, "MODE=", mode) && (mode == "diameter" || mode == "G7" || mode == "g7")) {
                spec.x_mode = Lathe::DiameterMode::Diameter;
            }

            Error err = Lathe::touch_off_tool(spec);
            send_json_command_response(out, 423, err == Error::Ok, err == Error::Ok ? "lathe tool touched off" : errorString(err));
            return err;
        }

        static Error showSysStats(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP420
            if (paramIsJSON(parameter)) {
                return showSysStatsJSON(parameter, auth_level, out);
            }

            log_stream(out, "Chip ID: " << (uint16_t)(ESP.getEfuseMac() >> 32));
            log_stream(out, "CPU Cores: " << ESP.getChipCores());
            log_stream(out, "CPU Frequency: " << ESP.getCpuFreqMHz() << "Mhz");

            std::ostringstream msg;
            msg << std::fixed << std::setprecision(1) << temperatureRead() << "°C";
            log_stream(out, "CPU Temperature: " << msg.str());
            log_stream(out, "Free memory: " << formatBytes(ESP.getFreeHeap()));
            log_stream(out, "SDK: " << ESP.getSdkVersion());
            log_stream(out, "Flash Size: " << formatBytes(ESP.getFlashChipSize()));

            for (auto const& module : Modules()) {
                module->build_info(out);
            }

            log_stream(out, "FW version: FluidNC " << git_info);
            return Error::Ok;
        }

        static Error setWebSetting(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP401
            // The string is of the form "P=name T=type V=value
            // We do not need the "T=" (type) parameter because the
            // Setting objects know their own type.  We do not use
            // split_params because if fails if the value string
            // contains '='
            std::string p, v;
            bool        isJSON = paramIsJSON(parameter);
            if (!(get_param(parameter, "P=", p) && get_param(parameter, "V=", v))) {
                if (isJSON) {
                    send_json_command_response(out, 401, false, errorString(Error::InvalidValue));
                }
                return Error::InvalidValue;
            }

            Error ret = do_command_or_setting(p, v, auth_level, out);
            if (isJSON) {
                send_json_command_response(out, 401, ret == Error::Ok, errorString(ret));
            }

            return ret;
        }

        // Used by js/setting.js
        static Error listSettingsJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP400
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "400");
            j.member("status", "ok");
            j.begin_array("data");

            // NVS settings
            j.setCategory("Flash/Settings");
            for (Setting* js : Setting::List) {
                js->addWebui(&j);
            }

            // Configuration tree
            j.setCategory("Running/Config");
            Configuration::JsonGenerator gen(j);
            config->group(gen);

            j.end_array();
            j.end();

            return Error::Ok;
        }

        static Error listSettings(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP400
            if (parameter != NULL) {
                if (strstr(parameter, "json=yes") != NULL) {
                    return listSettingsJSON(parameter, auth_level, out);
                }
            }

            JSONencoder j(&out);

            j.begin();
            j.begin_array("EEPROM");

            // NVS settings
            j.setCategory("nvs");
            for (Setting* js : Setting::List) {
                js->addWebui(&j);
            }

            // Configuration tree
            j.setCategory("tree");
            Configuration::JsonGenerator gen(j);
            config->group(gen);

            j.end_array();
            j.end();

            return Error::Ok;
        }

        static Error showWebHelp(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP0
            log_string(out, "Persistent web settings - $name to show, $name=value to set");
            log_string(out, "ESPname FullName         Description");
            log_string(out, "------- --------         -----------");

            for (Setting* setting : Setting::List) {
                if (setting->getType() == WEBSET) {
                    log_stream(out,
                               LeftJustify(setting->getGrblName() ? setting->getGrblName() : "", 8)
                                   << LeftJustify(setting->getName(), 25 - 8) << setting->getDescription());
                }
            }
            log_string(out, "");
            log_string(out, "Other web commands: $name to show, $name=value to set");
            log_string(out, "ESPname FullName         Values");
            log_string(out, "------- --------         ------");

            for (Command* cp : Command::List) {
                if (cp->getType() == WEBCMD) {
                    LogStream s(out, "");
                    s << LeftJustify(cp->getGrblName() ? cp->getGrblName() : "", 8) << LeftJustify(cp->getName(), 25 - 8);
                    if (cp->getDescription()) {
                        s << cp->getDescription();
                    }
                }
            }
            return Error::Ok;
        }

        void init() override {
            // If authentication enabled, display_settings skips or displays <Authentication Required>
            // RU - need user or admin password to read
            // WU - need user or admin password to set
            // WA - need admin password to set
            new WebCommand(NULL, WEBCMD, WU, "ESP420", "System/Stats", showSysStats, anyState);
            new WebCommand(NULL, WEBCMD, WU, "ESP421", "System/Lathe", showLatheStatusJSON, anyState);
            new WebCommand("T=tool [GX=x] [GZ=z] [WX=x] [WZ=z] [NR=r] [O=orientation]", WEBCMD, WA, "ESP422", "Lathe/ToolSet", setLatheToolJSON, anyState);
            new WebCommand("T=tool [MX=x RX=x MODE=diameter|radius] [MZ=z RZ=z]", WEBCMD, WA, "ESP423", "Lathe/TouchOff", touchOffLatheToolJSON, anyState);
            new WebCommand("RESTART", WEBCMD, WA, "ESP444", "System/Control", setSystemMode);

            //      new WebCommand("ON|OFF", WEBCMD, WA, "ESP115", "Radio/State", setRadioState);

            new WebCommand("P=position T=type V=value", WEBCMD, WA, "ESP401", "WebUI/Set", setWebSetting);
            new WebCommand(NULL, WEBCMD, WU, "ESP400", "WebUI/List", listSettings, anyState);
            new WebCommand(NULL, WEBCMD, WG, "ESP0", "WebUI/Help", showWebHelp, anyState);
            new WebCommand(NULL, WEBCMD, WG, "ESP", "WebUI/Help", showWebHelp, anyState);
        }
    };
    ModuleFactory::InstanceBuilder<WebCommands> web_commands_module __attribute__((init_priority(103))) ("web_commands", true);
}
