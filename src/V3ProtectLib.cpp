// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Build DPI protected C++ and SV
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3ProtectLib.h"

#include "V3Control.h"
#include "V3Hasher.h"
#include "V3InstrCount.h"
#include "V3String.h"
#include "V3Task.h"

#include <list>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// ProtectLib top-level visitor

class ProtectVisitor final : public VNVisitor {
    AstVFile* m_vfilep = nullptr;  // DPI-enabled Verilog wrapper
    AstCFile* m_cfilep = nullptr;  // C implementation of DPI functions
    // Verilog text blocks
    AstTextBlock* m_modPortsp = nullptr;  // Module port list
    AstTextBlock* m_comboPortsp = nullptr;  // Combo function port list
    AstTextBlock* m_seqPortsp = nullptr;  // Sequential function port list
    AstTextBlock* m_comboIgnorePortsp = nullptr;  // Combo ignore function port list
    AstTextBlock* m_comboDeclsp = nullptr;  // Combo signal declaration list
    AstTextBlock* m_seqDeclsp = nullptr;  // Sequential signal declaration list
    AstTextBlock* m_tmpDeclsp = nullptr;  // Temporary signal declaration list
    AstTextBlock* m_hashValuep = nullptr;  // CPP hash value
    AstTextBlock* m_comboParamsp = nullptr;  // Combo function parameter list
    AstTextBlock* m_clkSensp = nullptr;  // Clock sensitivity list
    AstTextBlock* m_comboIgnoreParamsp = nullptr;  // Combo ignore parameter list
    AstTextBlock* m_seqParamsp = nullptr;  // Sequential parameter list
    AstTextBlock* m_nbAssignsp = nullptr;  // Non-blocking assignment list
    AstTextBlock* m_seqAssignsp = nullptr;  // Sequential assignment list
    AstTextBlock* m_comboAssignsp = nullptr;  // Combo assignment list
    // C text blocks
    AstTextBlock* m_cHashValuep = nullptr;  // CPP hash value
    AstTextBlock* m_cComboParamsp = nullptr;  // Combo function parameter list
    AstTextBlock* m_cComboInsp = nullptr;  // Combo input copy list
    AstTextBlock* m_cComboOutsp = nullptr;  // Combo output copy list
    AstTextBlock* m_cSeqParamsp = nullptr;  // Sequential parameter list
    AstTextBlock* m_cSeqClksp = nullptr;  // Sequential clock copy list
    AstTextBlock* m_cSeqOutsp = nullptr;  // Sequential output copy list
    AstTextBlock* m_cIgnoreParamsp = nullptr;  // Combo ignore parameter list
    const string m_libName;
    const string m_topName;
    bool m_foundTop = false;  // Have seen the top module
    bool m_hasClk = false;  // True if the top module has sequential logic

    // VISITORS
    void visit(AstNetlist* nodep) override {
        m_vfilep
            = new AstVFile{nodep->fileline(), v3Global.opt.makeDir() + "/" + m_libName + ".sv"};
        nodep->addFilesp(m_vfilep);
        m_cfilep
            = new AstCFile{nodep->fileline(), v3Global.opt.makeDir() + "/" + m_libName + ".cpp"};
        nodep->addFilesp(m_cfilep);
        iterateChildren(nodep);
    }

    void visit(AstNodeModule* nodep) override {
        if (!nodep->isTop()) {
            return;
        } else {
            UASSERT_OBJ(!m_foundTop, nodep, "Multiple root modules");
        }
        FileLine* const fl = nodep->fileline();
        // Need to know the existence of clk before createSvFile()
        m_hasClk = checkIfClockExists(nodep);
        createSvFile(fl, nodep);
        createCppFile(fl);

        iterateChildren(nodep);

        // cppcheck-has-bug-suppress unreadVariable
        const V3Hash hash = V3Hasher::uncachedHash(m_cfilep);
        m_hashValuep->addText(fl, cvtToStr(hash.value()) + ";\n");
        m_cHashValuep->addText(fl, cvtToStr(hash.value()) + "U;\n");
        m_foundTop = true;
    }

    void addComment(AstTextBlock* txtp, FileLine* fl, const string& comment) {
        txtp->addNodesp(new AstComment{fl, comment});
    }

    void configSection(AstNodeModule* modp, AstTextBlock* txtp, FileLine* fl) {
        txtp->addText(fl, "\n`ifdef VERILATOR\n");
        txtp->addText(fl, "`verilator_config\n");

        // The `eval` function is called inside both update functions. As those functions
        // are created by text bashing, we need to find cost of `_eval` which is the first function
        // with a real cost in AST.
        uint32_t cost = 0;
        modp->foreach([&cost](AstCFunc* cfuncp) {
            if (cfuncp->name() == "_eval") cost = V3InstrCount::count(cfuncp, false);
        });
        txtp->addText(fl, "profile_data -hier-dpi \"" + m_libName
                              + "_protectlib_combo_update\" -cost 64'd" + std::to_string(cost)
                              + "\n");
        txtp->addText(fl, "profile_data -hier-dpi \"" + m_libName
                              + "_protectlib_seq_update\" -cost 64'd" + std::to_string(cost)
                              + "\n");

        // Mark remaining NDA protectlib wrapper DPIs as non-hazardous by deliberately forwarding
        // them with non-zero cost.
        // Also, specify hierarchical workers for those tasks for scheduling.
        txtp->addText(fl, "profile_data -hier-dpi \"" + m_libName
                              + "_protectlib_combo_ignore\" -cost 64'd1\n");

        txtp->addText(fl, "hier_workers -hier-dpi \"" + m_libName
                              + "_protectlib_combo_update\" -workers 16'd"
                              + std::to_string(V3Control::getHierWorkers(m_libName)) + "\n");
        txtp->addText(fl, "hier_workers -hier-dpi \"" + m_libName
                              + "_protectlib_seq_update\" -workers 16'd"
                              + std::to_string(V3Control::getHierWorkers(m_libName)) + "\n");
        // No workers for combo_ignore
        txtp->addText(fl, "`verilog\n");
        txtp->addText(fl, "`endif\n");
    }

    void hashComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Checks to make sure the .sv wrapper and library agree");
    }

    void initialComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Creates an instance of the library module at initial-time");
        addComment(txtp, fl, "(one for each instance in the user's design) also evaluates");
        addComment(txtp, fl, "the library module's initial process");
    }

    void comboComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Updates all non-clock inputs and retrieves the results");
    }

    void seqComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Updates all clocks and retrieves the results");
    }

    void comboIgnoreComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Need to convince some simulators that the input to the module");
        addComment(txtp, fl, "must be evaluated before evaluating the clock edge");
    }

    void finalComment(AstTextBlock* txtp, FileLine* fl) {
        addComment(txtp, fl, "Evaluates the library module's final process");
    }

    void createSvFile(FileLine* fl, AstNodeModule* modp) {
        // Comments
        AstTextBlock* const txtp = new AstTextBlock{fl};
        addComment(txtp, fl, "Wrapper module for DPI protected library");
        addComment(txtp, fl,
                   "This module requires lib" + m_libName + ".a or lib" + m_libName
                       + ".so to work");
        addComment(txtp, fl,
                   "See instructions in your simulator for how"
                   " to use DPI libraries\n");

        // Module declaration
        m_modPortsp = new AstTextBlock{fl, "module " + m_libName + " (\n", false, true};
        txtp->addNodesp(m_modPortsp);
        txtp->addText(fl, ");\n\n");

        // Timescale
        if (v3Global.opt.hierChild() && v3Global.rootp()->timescaleSpecified()) {
            // Emit timescale for hierarchical Verilation if input HDL specifies timespec
            txtp->addText(fl, "timeunit "s + modp->timeunit().ascii() + ";\n");
            txtp->addText(fl,
                          "timeprecision "s + +v3Global.rootp()->timeprecision().ascii() + ";\n");
        } else {
            addComment(txtp, fl,
                       "Precision of submodule"
                       " (commented out to avoid requiring timescale on all modules)");
            addComment(txtp, fl, "timeunit "s + v3Global.rootp()->timeunit().ascii() + ";");
            addComment(txtp, fl,
                       "timeprecision "s + v3Global.rootp()->timeprecision().ascii() + ";\n");
        }

        // DPI declarations
        hashComment(txtp, fl);
        txtp->addText(fl, "import \"DPI-C\" function void " + m_libName
                              + "_protectlib_check_hash(int protectlib_hash__V);\n\n");
        initialComment(txtp, fl);
        txtp->addText(fl, "import \"DPI-C\" function chandle " + m_libName
                              + "_protectlib_create(string scope__V);\n\n");
        comboComment(txtp, fl);
        m_comboPortsp = new AstTextBlock{fl,
                                         "import \"DPI-C\" function longint " + m_libName
                                             + "_protectlib_combo_update "
                                               "(\n",
                                         false, true};
        m_comboPortsp->addText(fl, "chandle handle__V\n");
        txtp->addNodesp(m_comboPortsp);
        txtp->addText(fl, ");\n\n");
        seqComment(txtp, fl);
        if (m_hasClk) {
            m_seqPortsp = new AstTextBlock{fl,
                                           "import \"DPI-C\" function longint " + m_libName
                                               + "_protectlib_seq_update"
                                                 "(\n",
                                           false, true};
            m_seqPortsp->addText(fl, "chandle handle__V\n");
            txtp->addNodesp(m_seqPortsp);
            txtp->addText(fl, ");\n\n");
        }
        comboIgnoreComment(txtp, fl);
        m_comboIgnorePortsp = new AstTextBlock{fl,
                                               "import \"DPI-C\" function void " + m_libName
                                                   + "_protectlib_combo_ignore"
                                                     "(\n",
                                               false, true};
        m_comboIgnorePortsp->addText(fl, "chandle handle__V\n");
        txtp->addNodesp(m_comboIgnorePortsp);
        txtp->addText(fl, ");\n\n");

        finalComment(txtp, fl);
        txtp->addText(fl, "import \"DPI-C\" function void " + m_libName
                              + "_protectlib_final(chandle handle__V);\n\n");

        // Local variables
        // Avoid tracing handle, as it is not a stable value, so breaks vcddiff
        // Likewise other internals aren't interesting to the user
        txtp->addText(fl, "// verilator tracing_off\n");

        txtp->addText(fl, "chandle handle__V;\n");
        txtp->addText(fl, "time last_combo_seqnum__V;\n");
        if (m_hasClk) txtp->addText(fl, "time last_seq_seqnum__V;\n");
        txtp->addText(fl, "\n");

        m_comboDeclsp = new AstTextBlock{fl};
        txtp->addNodesp(m_comboDeclsp);
        m_seqDeclsp = new AstTextBlock{fl};
        txtp->addNodesp(m_seqDeclsp);
        m_tmpDeclsp = new AstTextBlock{fl};
        txtp->addNodesp(m_tmpDeclsp);

        // CPP hash value
        addComment(txtp, fl, "Hash value to make sure this file and the corresponding");
        addComment(txtp, fl, "library agree");
        m_hashValuep = new AstTextBlock{fl, "localparam int protectlib_hash__V = 32'd"};
        txtp->addNodesp(m_hashValuep);
        txtp->addText(fl, "\n");

        // Initial
        txtp->addText(fl, "initial begin\n");
        txtp->addText(fl, m_libName + "_protectlib_check_hash(protectlib_hash__V);\n");
        txtp->addText(fl, "handle__V = " + m_libName
                              + "_protectlib_create"
                                "($sformatf(\"%m\"));\n");
        txtp->addText(fl, "end\n\n");

        // Combinatorial process
        addComment(txtp, fl, "Combinatorialy evaluate changes to inputs");
        m_comboParamsp = new AstTextBlock{fl,
                                          "always @* begin\n"
                                          "last_combo_seqnum__V = "
                                              + m_libName + "_protectlib_combo_update(\n",
                                          false, true};
        m_comboParamsp->addText(fl, "handle__V\n");
        txtp->addNodesp(m_comboParamsp);
        txtp->addText(fl, ");\n");
        txtp->addText(fl, "end\n\n");

        // Sequential process
        if (m_hasClk) {
            addComment(txtp, fl, "Evaluate clock edges");
            m_clkSensp = new AstTextBlock{fl, "always @(", false, true};
            txtp->addNodesp(m_clkSensp);
            txtp->addText(fl, ") begin\n");
            m_comboIgnoreParamsp
                = new AstTextBlock{fl, m_libName + "_protectlib_combo_ignore(\n", false, true};
            m_comboIgnoreParamsp->addText(fl, "handle__V\n");
            txtp->addNodesp(m_comboIgnoreParamsp);
            txtp->addText(fl, ");\n");
            m_seqParamsp = new AstTextBlock{
                fl, "last_seq_seqnum__V <= " + m_libName + "_protectlib_seq_update(\n", false,
                true};
            m_seqParamsp->addText(fl, "handle__V\n");
            txtp->addNodesp(m_seqParamsp);
            txtp->addText(fl, ");\n");
            m_nbAssignsp = new AstTextBlock{fl};
            txtp->addNodesp(m_nbAssignsp);
            txtp->addText(fl, "end\n\n");
        }

        // Select between combinatorial and sequential results
        addComment(txtp, fl, "Select between combinatorial and sequential results");
        txtp->addText(fl, "always @* begin\n");
        if (m_hasClk) {
            m_seqAssignsp = new AstTextBlock{fl, "if (last_seq_seqnum__V > "
                                                 "last_combo_seqnum__V) begin\n"};
            txtp->addNodesp(m_seqAssignsp);
            m_comboAssignsp = new AstTextBlock{fl, "end\nelse begin\n"};
            txtp->addNodesp(m_comboAssignsp);
            txtp->addText(fl, "end\n");
        } else {
            m_comboAssignsp = new AstTextBlock{fl, ""};
            txtp->addNodesp(m_comboAssignsp);
        }
        txtp->addText(fl, "end\n\n");

        // Final
        txtp->addText(fl, "final " + m_libName + "_protectlib_final(handle__V);\n\n");

        txtp->addText(fl, "endmodule\n");

        configSection(modp, txtp, fl);

        m_vfilep->tblockp(txtp);
    }

    void castPtr(FileLine* fl, AstTextBlock* txtp) {
        txtp->addText(fl, m_topName
                              + "_container* const handlep__V = "  // LCOV_EXCL_LINE  // lcov bug
                                "static_cast<"
                              + m_topName + "_container*>(vhandlep__V);\n");
    }

    void createCppFile(FileLine* fl) {
        // Comments
        AstTextBlock* const txtp = new AstTextBlock{fl};
        addComment(txtp, fl, "Wrapper functions for DPI protected library\n");

        // Includes
        txtp->addText(fl, "#include \"" + m_topName + ".h\"\n");
        txtp->addText(fl, "#include \"verilated_dpi.h\"\n\n");
        txtp->addText(fl, "#include <cstdio>\n");
        txtp->addText(fl, "#include <cstdlib>\n\n");

        // Verilated module plus sequence number
        addComment(txtp, fl, "Container class to house verilated object and sequence number");
        txtp->addText(fl, "class " + m_topName + "_container: public " + m_topName + " {\n");
        txtp->addText(fl, "public:\n");
        txtp->addText(fl, "long long m_seqnum;\n");
        txtp->addText(fl, m_topName + "_container(const char* scopep__V):\n");
        txtp->addText(fl, m_topName + "(scopep__V) {}\n");
        txtp->addText(fl, "};\n\n");

        // Extern C
        txtp->addText(fl, "extern \"C\" {\n\n");

        // Hash check
        hashComment(txtp, fl);
        txtp->addText(fl, "void " + m_libName
                              + "_protectlib_check_hash"
                                "(int protectlib_hash__V) {\n");
        m_cHashValuep = new AstTextBlock{fl, "const int expected_hash__V = "};
        txtp->addNodesp(m_cHashValuep);
        txtp->addText(fl, /**/ "if (protectlib_hash__V != expected_hash__V) {\n");
        txtp->addText(fl, /****/ "fprintf(stderr, \"%%Error: cannot use " + m_libName
                              + " library, "
                                "Verliog (%u) and library (%u) hash values do not "
                                "agree\\n\", protectlib_hash__V, expected_hash__V);\n");
        txtp->addText(fl, /****/ "std::exit(EXIT_FAILURE);\n");
        txtp->addText(fl, /**/ "}\n");
        txtp->addText(fl, "}\n\n");

        // Initial
        initialComment(txtp, fl);
        txtp->addText(fl, "void* " + m_libName + "_protectlib_create(const char* scopep__V) {\n");
        txtp->addText(fl, /**/ m_topName + "_container* const handlep__V = new " + m_topName
                              + "_container{scopep__V};\n");
        txtp->addText(fl, /**/ "return handlep__V;\n");
        txtp->addText(fl, "}\n\n");

        // Updates
        comboComment(txtp, fl);
        m_cComboParamsp = new AstTextBlock{
            fl, "long long " + m_libName + "_protectlib_combo_update(\n", false, true};
        m_cComboParamsp->addText(fl, "void* vhandlep__V\n");
        txtp->addNodesp(m_cComboParamsp);
        txtp->addText(fl, ")\n");
        m_cComboInsp = new AstTextBlock{fl, "{\n"};
        castPtr(fl, m_cComboInsp);
        txtp->addNodesp(m_cComboInsp);
        m_cComboOutsp = new AstTextBlock{fl, "handlep__V->eval();\n"};
        txtp->addNodesp(m_cComboOutsp);
        txtp->addText(fl, "return handlep__V->m_seqnum++;\n");
        txtp->addText(fl, "}\n\n");

        if (m_hasClk) {
            seqComment(txtp, fl);
            m_cSeqParamsp = new AstTextBlock{
                fl, "long long " + m_libName + "_protectlib_seq_update(\n", false, true};
            m_cSeqParamsp->addText(fl, "void* vhandlep__V\n");
            txtp->addNodesp(m_cSeqParamsp);
            txtp->addText(fl, ")\n");
            m_cSeqClksp = new AstTextBlock{fl, "{\n"};
            castPtr(fl, m_cSeqClksp);
            txtp->addNodesp(m_cSeqClksp);
            m_cSeqOutsp = new AstTextBlock{fl, "handlep__V->eval();\n"};
            txtp->addNodesp(m_cSeqOutsp);
            txtp->addText(fl, "return handlep__V->m_seqnum++;\n");
            txtp->addText(fl, "}\n\n");
        }

        comboIgnoreComment(txtp, fl);
        m_cIgnoreParamsp = new AstTextBlock{
            fl, "void " + m_libName + "_protectlib_combo_ignore(\n", false, true};
        m_cIgnoreParamsp->addText(fl, "void* vhandlep__V\n");
        txtp->addNodesp(m_cIgnoreParamsp);
        txtp->addText(fl, ")\n");
        txtp->addText(fl, "{ }\n\n");

        // Final
        finalComment(txtp, fl);
        txtp->addText(fl, "void " + m_libName + "_protectlib_final(void* vhandlep__V) {\n");
        castPtr(fl, txtp);
        txtp->addText(fl, /**/ "handlep__V->final();\n");
        txtp->addText(fl, /**/ "delete handlep__V;\n");
        txtp->addText(fl, "}\n\n");

        txtp->addText(fl, "}\n");
        m_cfilep->tblockp(txtp);
    }

    void visit(AstVar* nodep) override {
        if (!nodep->isIO()) return;
        if (nodep->direction() == VDirection::INPUT) {
            if (nodep->isUsedClock() || nodep->attrClocker() == VVarAttrClocker::CLOCKER_YES) {
                UASSERT_OBJ(m_hasClk, nodep, "checkIfClockExists() didn't find this clock");
                handleClock(nodep);
            } else {
                handleDataInput(nodep);
            }
        } else if (nodep->direction() == VDirection::OUTPUT) {
            handleOutput(nodep);
        } else {
            nodep->v3warn(E_UNSUPPORTED, "Unsupported: --lib-create port direction: "
                                             << nodep->direction().ascii());
        }
    }

    void visit(AstNode*) override {}

    string cInputConnection(AstVar* varp) {
        return V3Task::assignDpiToInternal("handlep__V->" + varp->name(), varp);
    }

    void handleClock(AstVar* varp) {
        FileLine* const fl = varp->fileline();
        handleInput(varp);
        m_seqPortsp->addNodesp(varp->cloneTree(false));
        if (m_hasClk) {
            m_seqParamsp->addText(fl, varp->prettyName() + "\n");
            m_clkSensp->addText(fl, "posedge " + varp->prettyName() + " or negedge "
                                        + varp->prettyName());
        }
        m_cSeqParamsp->addText(fl, varp->dpiArgType(true, false) + "\n");
        m_cSeqClksp->addText(fl, cInputConnection(varp));
    }

    void handleDataInput(AstVar* varp) {
        FileLine* const fl = varp->fileline();
        handleInput(varp);
        m_comboPortsp->addNodesp(varp->cloneTree(false));
        m_comboParamsp->addText(fl, varp->prettyName() + "\n");
        m_comboIgnorePortsp->addNodesp(varp->cloneTree(false));
        if (m_hasClk) m_comboIgnoreParamsp->addText(fl, varp->prettyName() + "\n");
        m_cComboParamsp->addText(fl, varp->dpiArgType(true, false) + "\n");
        m_cComboInsp->addText(fl, cInputConnection(varp));
        m_cIgnoreParamsp->addText(fl, varp->dpiArgType(true, false) + "\n");
    }

    void handleInput(AstVar* varp) { m_modPortsp->addNodesp(varp->cloneTree(false)); }

    static void addLocalVariable(AstTextBlock* textp, AstVar* varp, const char* suffix) {
        AstVar* const newVarp
            = new AstVar{varp->fileline(), VVarType::VAR, varp->name() + suffix, varp->dtypep()};
        textp->addNodesp(newVarp);
    }

    void handleOutput(AstVar* varp) {
        FileLine* const fl = varp->fileline();
        m_modPortsp->addNodesp(varp->cloneTree(false));
        m_comboPortsp->addNodesp(varp->cloneTree(false));
        m_comboParamsp->addText(fl, varp->prettyName() + "_combo__V\n");
        if (m_hasClk) {
            m_seqPortsp->addNodesp(varp->cloneTree(false));
            m_seqParamsp->addText(fl, varp->prettyName() + "_tmp__V\n");
        }

        addLocalVariable(m_comboDeclsp, varp, "_combo__V");

        if (m_hasClk) {
            addLocalVariable(m_seqDeclsp, varp, "_seq__V");
            addLocalVariable(m_tmpDeclsp, varp, "_tmp__V");

            m_nbAssignsp->addText(fl, varp->prettyName() + "_seq__V <= " + varp->prettyName()
                                          + "_tmp__V;\n");
            m_seqAssignsp->addText(fl,
                                   varp->prettyName() + " = " + varp->prettyName() + "_seq__V;\n");
        }
        m_comboAssignsp->addText(fl,
                                 varp->prettyName() + " = " + varp->prettyName() + "_combo__V;\n");
        m_cComboParamsp->addText(fl, varp->dpiArgType(true, false) + "\n");
        m_cComboOutsp->addText(fl,
                               V3Task::assignInternalToDpi(varp, true, "", "", "handlep__V->"));
        if (m_hasClk) {
            m_cSeqParamsp->addText(fl, varp->dpiArgType(true, false) + "\n");
            m_cSeqOutsp->addText(fl,
                                 V3Task::assignInternalToDpi(varp, true, "", "", "handlep__V->"));
        }
    }

    static bool checkIfClockExists(AstNodeModule* modp) {
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (const AstVar* const varp = VN_CAST(stmtp, Var)) {
                if (varp->direction() == VDirection::INPUT
                    && (varp->isUsedClock()
                        || varp->attrClocker() == VVarAttrClocker::CLOCKER_YES)) {
                    return true;
                }
            }
        }
        return false;
    }

public:
    explicit ProtectVisitor(AstNode* nodep)
        : m_libName{v3Global.opt.libCreate()}
        , m_topName{v3Global.opt.prefix()} {
        iterate(nodep);
    }
};

//######################################################################
// ProtectLib class functions

void V3ProtectLib::protect() {
    UINFO(2, __FUNCTION__ << ":");
    ProtectVisitor{v3Global.rootp()};
}
