/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2009 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/* ===================================================================== */
/*! @file
 *  This file contains a dynamic register/memory operand pattern profiler
 */


#include "pin.H"
#include <list>
#include <iostream>
#include <cassert>
#include <iomanip>
#include <fstream>
#include <stdlib.h>
#include <string.h>


/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,        "pintool",
    "o", "bblengthmix.out", "specify profile file name");
KNOB<BOOL>   KnobNoSharedLibs(KNOB_MODE_WRITEONCE,      "pintool",
    "no_shared_libs", "0", "do not instrument shared libraries");
KNOB<UINT32>   KnobMaxThreads(KNOB_MODE_WRITEONCE,      "pintool",
    "threads", "100", "Maximum number of threads");

/* ===================================================================== */

INT32 Usage()
{
    cerr <<
        "This pin tool computes a dynamic basic block length mix profile\n"
        "\n";

    cerr << KNOB_BASE::StringKnobSummary();

    cerr << endl;

    return -1;
}

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
UINT32 MaxNumThreads = 16;
const INT32 MAX_BBL_SIZE  = 1000;


typedef UINT64 COUNTER;

/*
typedef enum {
    PATTERN_INVALID,
    PATTERN_MEM_RW,
    PATTERN_MEM_R,
    PATTERN_MEM_W,
    PATTERN_NO_MEM,
    PATTERN_NO_MEM_LIES,
    PATTERN_LAST
} pattern_t;
*/

typedef struct 
{
    COUNTER bblength[MAX_BBL_SIZE];
} STATS;

STATS GlobalStats;

class BBLSTATS
{
  public:
    BBLSTATS(UINT16 * stats, INT32 size)
              : _stats(stats), _size(size)
    {
        _counter = new COUNTER[MaxNumThreads];
        memset(_counter,0,sizeof(COUNTER)*MaxNumThreads);
    };

    //array of uint16, one per instr in the block, 0 terminated
    const UINT16 * _stats; 
    const INT32    _size;

    // one ctr per thread to avoid runtime locking at the expense of memory
    COUNTER* _counter;

};

list<const BBLSTATS*> statsList;


//////////////////////////////////////////////////////////

PIN_LOCK lock;
UINT32 numThreads = 0;

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    GetLock(&lock, threadid+1);
    numThreads++;
    ReleaseLock(&lock);
    
    ASSERT(numThreads <= MaxNumThreads, "Maximum number of threads exceeded\n");
}

/* ===================================================================== */

VOID ComputeGlobalStats()
{
    for(int i=0;i<MAX_BBL_SIZE;i++)
        GlobalStats.bblength[i] = 0;

    // We have the count for each bbl and its stats, compute the summary
    for (list<const BBLSTATS*>::iterator bi = statsList.begin(); bi != statsList.end(); bi++)
      for(UINT32 thd = 0 ; thd < numThreads; thd++)
        GlobalStats.bblength[(*bi)->_size] += (*bi)->_counter[thd];
    
}

/* ===================================================================== */


/* ===================================================================== */

VOID docount(COUNTER * counter, THREADID tid)
{
    counter[tid]++;
}

INT32 RecordLength(BBL bbl, 
                   UINT16 * stats, 
                   UINT32 max_stats)
{
    UINT32 count = 0;
    
    for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
    {
        if (count >= max_stats)
        {
            cerr << "Too many stats in this block" << endl;
            exit(1);
        }
        count++;
        // inspect instruction here if desired
    }

    stats[count++] = 0;
    
    return count;
}


    

/* ===================================================================== */

VOID Trace(TRACE trace, VOID *v)
{
    const RTN rtn = TRACE_Rtn(trace);
    
    if (! RTN_Valid(rtn))
        return;
    
    const SEC sec = RTN_Sec(rtn);
    ASSERTX(SEC_Valid(sec));
    
    const IMG img = SEC_Img(sec);
    ASSERTX(IMG_Valid(img));
    
    if ( KnobNoSharedLibs.Value() ) {
        if(                  (RTN_Valid(TRACE_Rtn(trace))) && 
                    (SEC_Valid(RTN_Sec(TRACE_Rtn(trace)))) && 
          (IMG_Valid(SEC_Img(RTN_Sec(TRACE_Rtn(trace)))))) {

          if(IMG_Type(SEC_Img(RTN_Sec(TRACE_Rtn(trace)))) == IMG_TYPE_SHAREDLIB) {
            //cout << "IGNORING SHARED LIB " << 
            //  IMG_Name(SEC_Img(RTN_Sec(TRACE_Rtn(trace)))) << std::endl;
            return;
          }
          else {
            //cout << "TRACING IMAGE: " << 
            //  IMG_Name(SEC_Img(RTN_Sec(TRACE_Rtn(trace)))) << std::endl;
          }
        }
    }
    
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Record the registers into a dummy buffer so we can count them
        UINT16 buffer[MAX_BBL_SIZE];
        INT32 count = RecordLength(bbl, buffer, MAX_BBL_SIZE);
        ASSERTX(count < MAX_BBL_SIZE);
        
        // Summarize the stats for the bbl in a 0 terminated list
        // This is done at instrumentation time
        UINT16 * stats = new UINT16[count];

        memcpy(stats, buffer, count * sizeof(UINT16));

        // Insert instrumentation to count the number of times the bbl is executed
        BBLSTATS * bblstats = new BBLSTATS(stats, count - 1); //-1 for the 0 terminal marker
        INS_InsertCall(BBL_InsHead(bbl), IPOINT_BEFORE, AFUNPTR(docount), 
                       IARG_PTR, bblstats->_counter,
                       IARG_THREAD_ID,
                       IARG_END);

        // Remember the counter and stats so we can compute a summary at the end
        statsList.push_back(bblstats);
    }
}


/* ===================================================================== */
VOID EmitPerThreadStats(ostream* out)
{
    *out << std::setprecision(4) << showpoint;

    for(UINT32 thd = 0 ; thd < numThreads; thd++)
    {
        STATS ThreadStats;
        for(int i=0;i<MAX_BBL_SIZE;i++)
            ThreadStats.bblength[i] = 0;

        for (list<const BBLSTATS*>::iterator bi = statsList.begin(); bi != statsList.end(); bi++)
                ThreadStats.bblength[(*bi)->_size] += (*bi)->_counter[thd];

        COUNTER total = 0;
        for (int i = 0; i < MAX_BBL_SIZE; i++)
            total += ThreadStats.bblength[i];
        
        *out << "Thread " << thd << endl;
        for (int i = 0; i < MAX_BBL_SIZE; i++)
            if(ThreadStats.bblength[i] > 0) {
            *out << ljstr("",15) 
                 << decstr(i,12)
                 << decstr( ThreadStats.bblength[i],12)
                 << "\t"
                 << std::setw(10)
                 << 100.0*ThreadStats.bblength[i]/total
                 << std::endl;
            }
        *out << endl;
    }
    
}

static std::ofstream* out = 0;

VOID Fini(int, VOID * v)
{   
    ComputeGlobalStats();

    *out <<
        "#\n"
        "#block-length count percent\n"
        "#\n";
    
    *out << "### All Threads" << endl;
    COUNTER total = 0;
    for (int i = 0; i < MAX_BBL_SIZE; i++)
        total += GlobalStats.bblength[i];

    *out << std::setprecision(4) << showpoint;
    for (int i = 0; i < MAX_BBL_SIZE; i++)
        if(GlobalStats.bblength[i] > 0) {
          *out << ljstr("",15) 
              << decstr(i,12)
              << decstr( GlobalStats.bblength[i],12)
              << "\t"
              << std::setw(10)
              << 100.0*GlobalStats.bblength[i]/total
              << std::endl;
        }

    //*out<< endl;

    //EmitPerThreadStats(out);

    *out << "# eof" << endl;
    
    out->close();
}

/* ===================================================================== */

int main(int argc, CHAR *argv[])
{
    PIN_InitSymbols();

    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }
    MaxNumThreads = KnobMaxThreads.Value();
    out = new std::ofstream(KnobOutputFile.Value().c_str());

    PIN_AddThreadStartFunction(ThreadStart, 0);

    TRACE_AddInstrumentFunction(Trace, 0);

    PIN_AddFiniFunction(Fini, 0);

    // Never returns

    PIN_StartProgram();
    
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
