//===-- SBCJThread.i -----------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

namespace lldb {

%feature("docstring",
"Represents a cjthread of execution. :py:class:`SBProcess` contains SBCJThread(s).

SBCJThreads can be referred to by their ID, which maps to the system specific cjthread
identifier, or by IndexID.  The ID may or may not be unique depending on whether the
system reuses its cjthread identifiers.  The IndexID is a monotonically increasing identifier
that will always uniquely reference a particular cjthread, and when that cjthread goes
away it will not be reused.

SBCJThread supports frame iteration. For example (from test/python_api/
lldbutil/iter/TestLLDBIterator.py), ::

        from lldbutil import print_stacktrace
        stopped_due_to_breakpoint = False
        for cjthread in process:
            if self.TraceOn():
                print_stacktrace(cjthread)
            ID = cjthread.GetCJThreadID()
            if cjthread.GetStopReason() == lldb.eStopReasonBreakpoint:
                stopped_due_to_breakpoint = True
            for frame in cjthread:
                self.assertTrue(frame.GetCJThread().GetCJThreadID() == ID)
                if self.TraceOn():
                    print frame

        self.assertTrue(stopped_due_to_breakpoint)

See also :py:class:`SBFrame` ."
) SBCJThread;

class SBCJThread
{
public:
    SBCJThread ();

    SBCJThread (const lldb::SBCJThread &thread);

   ~SBCJThread();

    bool
    IsValid() const;

    explicit operator bool() const;

    void
    Clear ();

    void
    StepOver (lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

    %feature("autodoc",
    "Do a source level single step over in the currently selected thread.") StepOver;
    void
    StepOver (lldb::RunMode stop_other_threads, SBError &error);

    void
    StepInto (lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

    void
    StepInto (const char *target_name, lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

    %feature("autodoc", "
    Step the current thread from the current source line to the line given by end_line, stopping if
    the thread steps into the function given by target_name.  If target_name is None, then stepping will stop
    in any of the places we would normally stop.") StepInto;
    void
    StepInto (const char *target_name,
              uint32_t end_line,
              SBError &error,
              lldb::RunMode stop_other_threads = lldb::eOnlyDuringStepping);

    void
    StepOut ();

    %feature("autodoc",
    "Step out of the currently selected thread.") StepOut;
    void
    StepOut (SBError &error);
    void
    StepInstruction(bool step_over);

    %feature("autodoc",
    "Do an instruction level single step in the currently selected thread.") StepInstruction;
    void
    StepInstruction(bool step_over, SBError &error);

    uint32_t
    GetNumFrames ();

    lldb::SBFrame
    GetFrameAtIndex (uint32_t idx);

    lldb::SBFrame
    GetSelectedFrame ();

    lldb::SBFrame
    SetSelectedFrame (uint32_t frame_idx);

    bool
    GetDescription (lldb::SBStream &description) const;

    %feature("docstring", "
    Get the description strings for this thread that match what the
    lldb driver will present, using the thread-format (stop_format==false)
    or thread-stop-format (stop_format = true).") GetDescription;
    bool GetDescription(lldb::SBStream &description, bool stop_format) const;

    %feature("docstring", "
    Returns the stop reason for this cjthread.
    See also GetStopReasonDataAtIndex().") GetStopReason;
    lldb::StopReason
    GetStopReason ();

    %feature("docstring", "
    Returns the cjthread ID which maps to the system specific cjthread identifier.") GetCJThreadID;
    lldb::tid_t
    GetCJThreadID () const;

    %feature("docstring", "
    Returns the host OS thread ID (tid) that this cjthread is currently bound to.
    When the cjthread state is 'running', it is scheduled on this OS thread.") GetHostThreadID;
    lldb::tid_t
    GetHostThreadID () const;

    %feature("docstring", "
    Returns the name of this cjthread.") GetName;
    const char *
    GetName () const;

    bool
    operator == (const lldb::SBCJThread &rhs) const;

    bool
    operator != (const lldb::SBCJThread &rhs) const;

};
}