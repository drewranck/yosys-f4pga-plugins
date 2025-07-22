/*
 * Copyright 2020-2022 F4PGA Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <tcl.h>
#include <algorithm>
#include <array>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "clocks.h"
#include "kernel/log.h"
#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "propagation.h"
#include "sdc_writer.h"
#include "set_clock_groups.h"
#include "set_false_path.h"
#include "set_max_delay.h"

USING_YOSYS_NAMESPACE

PRIVATE_NAMESPACE_BEGIN

struct ReadSdcCmd : public Frontend {
    ReadSdcCmd() : Frontend("sdc", "Read SDC file") {}

    void help() override
    {
        log("\n");
        log("    read_sdc <filename>\n");
        log("\n");
        log("Read SDC file.\n");
        log("\n");
    }

    void execute(std::istream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *) override
    {
        if (args.size() < 2) {
            log_cmd_error("Missing script file.\n");
        }
        log("\nReading clock constraints file(SDC)\n\n");
        size_t argidx = 1;
        extra_args(f, filename, args, argidx);
        std::string content{std::istreambuf_iterator<char>(*f), std::istreambuf_iterator<char>()};
        log("%s\n", content.c_str());
        Tcl_Interp *interp = yosys_get_tcl_interp();
        if (Tcl_EvalFile(interp, args[argidx].c_str()) != TCL_OK) {
            log_cmd_error("TCL interpreter returned an error: %s\n", Tcl_GetStringResult(interp));
        }
    }
};

struct GetPortsCmd : public Pass {
    GetPortsCmd() : Pass("get_ports", "Get top-level ports from the design") {}

    void help() override
    {
        log("\n");
        log("    get_ports [<port_name>]\n");
        log("\n");
        log("Returns all top-level ports in the design.\n");
        log("If a port name is specified, it returns only that port if it exists.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        if (!design->top_module()) {
            log_cmd_error("No top module selected.\n");
        }

        RTLIL::Module *top = design->top_module();
        Tcl_Interp *interp = yosys_get_tcl_interp();
        Tcl_Obj *tcl_list = Tcl_NewListObj(0, NULL);

        // If a single port name is provided
        if (args.size() == 2) {
            std::string port_to_check = args[1];
            bool found = false;

            for (auto &it : top->wires_) {
                RTLIL::Wire *wire = it.second;
                if (!wire->port_id)
                    continue;
                std::string port_name = RTLIL::id2cstr(wire->name);
                if (port_name == port_to_check) {
		    //std::string full_path = "%top/" + port_name;
                    Tcl_Obj *name_obj = Tcl_NewStringObj(port_name.c_str(), -1);
                    log("%s\n", port_name.c_str()); // Print to Yosys log
                    Tcl_SetObjResult(interp, name_obj);
                    found = true;
                    break;
                }
            }

            if (!found) {
                log("Error:: port %s not exist\n", port_to_check.c_str()); // if not found
                Tcl_SetObjResult(interp, Tcl_NewStringObj("", -1));
            }
        }
        // No port name specified, return all top-level ports
        else if (args.size() == 1) {
	    bool found_any_port = false;
            for (auto &it : top->wires_) {
                RTLIL::Wire *wire = it.second;
                if (!wire->port_id)
                    continue;
                std::string port_name = RTLIL::id2cstr(wire->name);
                Tcl_Obj *name_obj = Tcl_NewStringObj(port_name.c_str(), -1);
                Tcl_ListObjAppendElement(interp, tcl_list, name_obj);
		found_any_port = true;
            }
	    if (!found_any_port) {
	        log("Error:: No ports found for 'get_ports'\n");  // Error message if no ports were found
                Tcl_SetObjResult(interp, Tcl_NewStringObj("", -1));
	    }

            // Print each port to Yosys log
            int list_len;
            Tcl_ListObjLength(interp, tcl_list, &list_len);
            for (int i = 0; i < list_len; i++) {
                Tcl_Obj *obj;
                Tcl_ListObjIndex(interp, tcl_list, i, &obj);
                log("%s\n", Tcl_GetString(obj));
            }

            Tcl_SetObjResult(interp, tcl_list);
        }
        else {
            log_cmd_error("Invalid usage: get_ports [port_name]\n");
        }
    }
};

//get_ports end

struct WriteSdcCmd : public Backend {
    WriteSdcCmd(SdcWriter &sdc_writer) : Backend("sdc", "Write SDC file"), sdc_writer_(sdc_writer) {}

    void help() override
    {
        log("\n");
        log("    write_sdc [-include_propagated_clocks] <filename>\n");
        log("\n");
        log("Write SDC file.\n");
        log("\n");
        log("    -include_propagated_clocks\n");
        log("       Write out all propagated clocks");
        log("\n");
    }

    void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
    {
        size_t argidx;
        bool include_propagated = false;
        if (args.size() < 2) {
            log_cmd_error("Missing output file.\n");
        }
        for (argidx = 1; argidx < args.size(); argidx++) {
            std::string arg = args[argidx];
            if (arg == "-include_propagated_clocks" && argidx + 1 < args.size()) {
                include_propagated = true;
                continue;
            }
            break;
        }
        log("\nWriting out clock constraints file(SDC)\n");
        extra_args(f, filename, args, argidx);
        sdc_writer_.WriteSdc(design, *f, include_propagated);
    }

    SdcWriter &sdc_writer_;
};

struct CreateClockCmd : public Pass {
    CreateClockCmd() : Pass("create_clock", "Create clock object") {}

    void help() override
    {
        log("\n");
        log("    create_clock [ -name clock_name ] -period period_value "
            "[-waveform <edge_list>] <target>\n");
        log("Define a clock.\n");
        log("If name is not specified then the name of the first target is "
            "selected as the clock's name.\n");
        log("Period is expressed in nanoseconds.\n");
        log("The waveform option specifies the duty cycle (the rising a "
            "falling edges) of the clock.\n");
        log("It is specified as a list of two elements/time values: the first "
            "rising edge and the next falling edge.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        size_t argidx;
        std::string name;
        bool is_waveform_specified(false);
        float rising_edge(0);
        float falling_edge(0);
        float period(0);
        if (args.size() < 4) {
            log_cmd_error("Incorrect number of arguments\n");
        }
        for (argidx = 1; argidx < args.size(); argidx++) {
            std::string arg = args[argidx];
            if (arg == "-add" && argidx + 1 < args.size()) {
                continue;
            }
            if (arg == "-name" && argidx + 1 < args.size()) {
                name = args[++argidx];
                continue;
            }
            if (arg == "-period" && argidx + 1 < args.size()) {
                period = std::stof(args[++argidx]);
                continue;
            }
            if (arg == "-waveform" && argidx + 1 < args.size()) {
                std::string edges(args[++argidx]);
                std::copy_if(edges.begin(), edges.end(), edges.begin(), [](char c) { return c != '{' or c != '}'; });
                std::stringstream ss(edges);
                ss >> rising_edge >> falling_edge;
                is_waveform_specified = true;
                continue;
            }
            break;
        }
        if (period <= 0) {
            log_cmd_error("Incorrect period value\n");
        }
        // Add "w:" prefix to selection arguments to enforce wire object
        // selection
        AddWirePrefix(args, argidx);
        extra_args(args, argidx, design);
        // If clock name is not specified then take the name of the first target
        std::vector<RTLIL::Wire *> selected_wires;
        for (auto module : design->modules()) {
            if (!design->selected(module)) {
                continue;
            }
            for (auto wire : module->wires()) {
                if (design->selected(module, wire)) {
#ifdef SDC_DEBUG
                    log("Selected wire %s\n", RTLIL::unescape_id(wire->name).c_str());
#endif
                    selected_wires.push_back(wire);
                }
            }
        }
        if (selected_wires.empty()) {
            for (auto module : design->modules()) {
                if (design->selected(module)) {
                    for (auto wire : module->wires()) {
                        if (wire->name == name) {
                            selected_wires.push_back(wire);
                            design->select(module, wire);  // Automatically select the wire
                        }
                    }
                }
            }
        }
        if (selected_wires.size() == 0) {
            log_cmd_error("Target selection is empty\n");
        }
        if (name.empty()) {
            name = RTLIL::unescape_id(selected_wires.at(0)->name);
        }
        if (!is_waveform_specified) {
            rising_edge = 0;
            falling_edge = period / 2;
        }
        Clock::Add(name, selected_wires, period, rising_edge, falling_edge, Clock::EXPLICIT);
    }

    void AddWirePrefix(std::vector<std::string> &args, size_t argidx)
    {
        auto selection_begin = args.begin() + argidx;
        std::transform(selection_begin, args.end(), selection_begin, [](std::string &w) { return "w:" + w; });
    }
};

#include <map>

struct GetClocksCmd : public Pass {
    GetClocksCmd() : Pass("get_clocks", "Create clock object") {}

    // Static map to track logical names for clocks
    static std::map<std::string, std::string> logical_to_wire_map;

    void help() override {
        log("\n");
        log("    get_clocks [-include_generated_clocks] [-of <nets>] "
            "[<patterns>]\n");
        log("\n");
        log("Returns all clocks in the design.\n");
        log("\n");
    }

    std::vector<std::string> extract_list(const std::string &args) {
        std::vector<std::string> port_list;
        std::stringstream ss(args);
        std::istream_iterator<std::string> begin(ss);
        std::istream_iterator<std::string> end;
        std::copy(begin, end, std::back_inserter(port_list));
        return port_list;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        // Ensure design is loaded
        if (!design) {
            log_cmd_error("No design loaded.\n");
            return;
        }

        // Parse command arguments
        bool include_generated_clocks = false;
        std::vector<std::string> clocks_nets;
        size_t argidx = 0;

        // Parse command switches
        for (argidx = 1; argidx < args.size(); argidx++) {
            std::string arg = args[argidx];
            if (arg == "-include_generated_clocks") {
                include_generated_clocks = true;
                continue;
            }
            if (arg == "-of" && argidx + 1 < args.size()) {
                clocks_nets = extract_list(args[++argidx]);
                continue;
            }
            if (arg.size() > 0 && arg[0] == '-') {
                log_cmd_error("Unknown option %s.\n", arg.c_str());
            }
            break;
        }

        // Parse object patterns (clock names)
        std::vector<std::string> clocks_list(args.begin() + argidx, args.end());

        // Fetch clocks in the design
        std::map<std::string, RTLIL::Wire *> clocks = Clocks::GetClocks(design);

        // Ensure clocks are retrieved properly
        if (clocks.size() == 0) {
            log_warning("No clocks found in design\n");
        }

        // Extract clocks into a Tcl list
        Tcl_Interp *interp = yosys_get_tcl_interp();
        if (!interp) {
            log_cmd_error("Tcl interpreter is not available.\n");
            return;
        }

        Tcl_Obj *tcl_list = Tcl_NewListObj(0, NULL);

        // Iterate over the clock wires in the design and find their logical names
        for (auto &clock : clocks) {
            auto &wire = clock.second;

            // Dynamically get the wire name (PCLK_i, PCLK_o etc.)
            const std::string wire_name = RTLIL::id2cstr(wire->name);

            // Now, we look up the logical name from the static map
            if (logical_to_wire_map.find(wire_name) != logical_to_wire_map.end()) {
                // The wire name exists, we should return the logical name
                const std::string logical_name = logical_to_wire_map[wire_name];

                // Add logical clock name to the Tcl list
                Tcl_Obj *name_obj = Tcl_NewStringObj(logical_name.c_str(), -1);
                Tcl_ListObjAppendElement(interp, tcl_list, name_obj);
            }
        }

        // Set result in Tcl interpreter
        Tcl_SetObjResult(interp, tcl_list);

        // Retrieve and log the result string (all clock names)
        Tcl_Obj *result = Tcl_GetObjResult(interp);
        const char *result_str = Tcl_GetString(result);

        // Print each clock name line by line
        std::stringstream ss(result_str);
        std::string clock_name;
        while (ss >> clock_name) {
            log("%s\n", clock_name.c_str());
        }

        // Ensure internal state is updated for shell interaction
        if (clocks.size() == 0) {
            log_warning("No clocks found in design after processing.\n");
        }
    }

    // This function should be called when the create_clock command is issued
    static void create_clock_with_name(const std::string &logical_name, const std::string &wire_name) {
        // Store the mapping of logical name to wire name
        logical_to_wire_map[wire_name] = logical_name;
    }
};

// Initialize static map
std::map<std::string, std::string> GetClocksCmd::logical_to_wire_map;


struct PropagateClocksCmd : public Pass {
    PropagateClocksCmd() : Pass("propagate_clocks", "Propagate clock information") {}

    void help() override
    {
        log("\n");
        log("    propagate_clocks\n");
        log("\n");
        log("Propagate clock information throughout the design.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        if (args.size() > 1) {
            log_warning("Command accepts no arguments.\nAll will be ignored.\n");
        }
        if (!design->top_module()) {
            log_cmd_error("No top module selected\n");
        }

        std::array<std::unique_ptr<Propagation>, 2> passes{std::unique_ptr<Propagation>(new BufferPropagation(design, this)),
                                                           std::unique_ptr<Propagation>(new ClockDividerPropagation(design, this))};

        log("Perform clock propagation\n");

        for (auto &pass : passes) {
            pass->Run();
        }

        Clocks::UpdateAbc9DelayTarget(design);
    }
};



void register_tcl_command(const std::string &name, Pass *pass)
{
    Tcl_Interp *interp = yosys_get_tcl_interp();
    Tcl_CreateObjCommand(interp, name.c_str(),
        [](ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) -> int {
            Pass *pass = static_cast<Pass *>(clientData);
            try {
                std::vector<std::string> args;
                for (int i = 0; i < objc; ++i) {
                    args.push_back(Tcl_GetString(objv[i]));
                }
                //pass->run(args);
		pass->execute(args, yosys_get_design());
                return TCL_OK;
            } catch (log_cmd_error_exception &) {
                //Tcl_SetResult(interp, (char *)e.what(), TCL_VOLATILE);
		//Tcl_SetResult(interp, (char *)e.c_str(), TCL_VOLATILE);
                return TCL_ERROR;
            } catch (std::exception &e) {
                Tcl_SetResult(interp, (char *)e.what(), TCL_VOLATILE);
                return TCL_ERROR;
            }
        },
        (ClientData)pass,
        nullptr);
}

class SdcPlugin
{
  public:
    SdcPlugin()
        : sdc_writer_(),
	  read_sdc_cmd_(),
          get_ports_cmd_(),
          write_sdc_cmd_(sdc_writer_),
          create_clock_cmd_(),
          get_clocks_cmd_(),
          propagate_clocks_cmd_(),
          set_false_path_cmd_(sdc_writer_),
          set_max_delay_cmd_(sdc_writer_),
          set_clock_groups_cmd_(sdc_writer_)
    {
        log("Loaded SDC plugin\n");

        register_tcl_command("get_ports", &get_ports_cmd_);
        register_tcl_command("create_clock", &create_clock_cmd_);
        register_tcl_command("get_clocks", &get_clocks_cmd_);
        register_tcl_command("propagate_clocks", &propagate_clocks_cmd_);
        register_tcl_command("set_false_path", &set_false_path_cmd_);
        register_tcl_command("set_max_delay", &set_max_delay_cmd_);
        register_tcl_command("set_clock_groups", &set_clock_groups_cmd_);
    }

  private:
    SdcWriter sdc_writer_;
    ReadSdcCmd read_sdc_cmd_;
    GetPortsCmd get_ports_cmd_;
    WriteSdcCmd write_sdc_cmd_;
    CreateClockCmd create_clock_cmd_;
    GetClocksCmd get_clocks_cmd_;
    PropagateClocksCmd propagate_clocks_cmd_;
    SetFalsePath set_false_path_cmd_;
    SetMaxDelay set_max_delay_cmd_;
    SetClockGroups set_clock_groups_cmd_;
} SdcPlugin;



PRIVATE_NAMESPACE_END
