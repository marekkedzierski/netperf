/*
#  Copyright 2021 Hewlett Packard Enterprise Development LP
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
#
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
# USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

char   netcpu_ntperf_id[]="\
@(#)netcpu_ntperf.c (c) Copyright 2005-2012, Hewlett-Packard Company, 2021 Hewlett Packard Enterprise Development LP, Version 2.6.0";

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>

#include <process.h>
#include <time.h>

#include <windows.h>
#include <assert.h>

#include <winsock2.h>
// If you are trying to compile on Windows 2000 or NT 4.0 you may
// need to define DONT_IPV6 in the "sources" files.
#ifndef DONT_IPV6
#include <ws2tcpip.h>
#endif

#include "netsh.h"
#include "netlib.h"

//
// System CPU time information class.
// Used to get CPU time information.
//
//     SDK\inc\ntexapi.h
// Function x8:   SystemProcessorPerformanceInformation
// DataStructure: SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
//

#define SystemProcessorPerformanceInformation 0x08

typedef struct
{
        LARGE_INTEGER   IdleTime;
        LARGE_INTEGER   KernelTime;
        LARGE_INTEGER   UserTime;
        LARGE_INTEGER   DpcTime;
        LARGE_INTEGER   InterruptTime;
        LONG                    InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, *PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

//
// Calls to get the information
//
typedef ULONG (__stdcall *NT_QUERY_SYSTEM_INFORMATION)(
                                                                                        ULONG SystemInformationClass,
                                                                                        PVOID SystemInformation,
                                                                                        ULONG SystemInformationLength,
                                                                                        PULONG ReturnLength
                                                                                        );

NT_QUERY_SYSTEM_INFORMATION NtQuerySystemInformation = NULL;


typedef BOOL(WINAPI *GET_LOGICAL_PROCESSOR_INFORMATION)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
GET_LOGICAL_PROCESSOR_INFORMATION GetLogicalProcessorInformationCall = NULL;

static LARGE_INTEGER TickHz = {{0,0}};

_inline LARGE_INTEGER ReadPerformanceCounter(VOID)
{
        LARGE_INTEGER Counter;
        QueryPerformanceCounter(&Counter);

        return(Counter);
}       // ReadperformanceCounter


/* The NT performance data is accessed through the NtQuerySystemInformation
   call.  References to the PDH.DLL have been deleted.  This structure
   is the root for these data structures. */

typedef struct sPerfObj
{
        LARGE_INTEGER   StartTime;
        LARGE_INTEGER   EndTime;
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION StartInfo[MAXCPUS +1];
        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION EndInfo[MAXCPUS +1];
} PerfObj, *PPerfObj;

static PerfObj *PerfCntrs;

// Forward declarations

PerfObj *InitPerfCntrs();
void RestartPerfCntrs(PerfObj *PerfCntrs);
double ReportPerfCntrs(PerfObj *PerfCntrs);  /* returns CPU utilization */
void ClosePerfCntrs(PerfObj *PerfCntrs);


void
cpu_util_init(void)
{
  if (NtQuerySystemInformation == NULL) {
    // Open the performance counter interface
    PerfCntrs = InitPerfCntrs();
  }
  return;
}

void
cpu_util_terminate(void)
{
  return;
}

int
get_cpu_method(void)
{
  return NT_METHOD;
}

typedef unsigned __int64    uint64_t;

void
get_cpu_idle(uint64_t *res)
{
  RestartPerfCntrs(PerfCntrs);
  return;
}

float
calibrate_idle_rate(int iterations, int interval)
{
  return (float)0.0;
}


/*
  InitPerfCntrs() -

  Changed to no longer access the NT performance registry interfaces.
  A direct call to NtQuerySystemInformation (an undocumented NT API)
  is made instead.  Parameters determined by decompilation of ntkrnlmp
  and ntdll.
*/


PerfObj *InitPerfCntrs()
{
  PerfObj *NewPerfCntrs;
  DWORD NTVersion;
  DWORD status;

  NewPerfCntrs = (PerfObj *)GlobalAlloc(GPTR, sizeof(PerfObj));
  assert(NewPerfCntrs != NULL);

  ZeroMemory((PCHAR)NewPerfCntrs, sizeof(PerfObj));

  // get NT version
  NTVersion = GetVersion();
  if (NTVersion >= 0x80000000)
    {
      fprintf(stderr, "Not running on Windows NT\n");
      exit(1);
    }

  // locate the calls we need in NTDLL
  //Lint
  NtQuerySystemInformation =
    (NT_QUERY_SYSTEM_INFORMATION)GetProcAddress( GetModuleHandle("ntdll.dll"),
						 "NtQuerySystemInformation" );

  if ( !(NtQuerySystemInformation) )
    {
      //Lint
      status = GetLastError();
      fprintf(stderr, "GetProcAddressFailed, status: %lX\n", status);
      exit(1);
    }

  GetLogicalProcessorInformationCall = 
	  (GET_LOGICAL_PROCESSOR_INFORMATION)GetProcAddress(GetModuleHandle("ntdll.dll"),
	  "GetLogicalProcessorInformation");

  // setup to measure timestamps with the high resolution timers.
  if (QueryPerformanceFrequency(&TickHz) == FALSE)
    {
      fprintf(stderr,"MAIN - QueryPerformanceFrequency Failed!\n");
      exit(2);
    }

  RestartPerfCntrs(NewPerfCntrs);

  return(NewPerfCntrs);
}  /* InitPerfCntrs */

/*
 Helper function to count set bits in the processor mask
 Code stolen from 
 https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformation
*/
DWORD CountSetBits(ULONG_PTR bitMask)
{
	DWORD LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
	DWORD bitSetCount = 0;
	ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
	DWORD i;

	for (i = 0; i <= LSHIFT; ++i)
	{
		bitSetCount += ((bitMask & bitTest) ? 1 : 0);
		bitTest /= 2;
	}

	return bitSetCount;
}

DWORD GetCpuCount()
{
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION logicalProcInfoBuffer = NULL;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
	DWORD logicalProcessorCount = 0;
	DWORD bufferLen = 0;
	DWORD processorInfoCount = 0;
	DWORD i = 0;

	// Get size of logicalProcInfoBuffer for data
	BOOL result = GetLogicalProcessorInformationCall(logicalProcInfoBuffer, &bufferLen);
	if (!result)
	{
		DWORD lastError = GetLastError();
		if (lastError != ERROR_INSUFFICIENT_BUFFER)
		{
			fprintf(stderr, "GetLogicalProcessorInformationCall failed, status: %lX\n", 
				lastError);
			exit(1);
		}
	}

	logicalProcInfoBuffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)GlobalAlloc(GPTR,
		bufferLen);

	if (!logicalProcInfoBuffer)
	{
		fprintf(stderr, "GlobalAlloc failed, status: %lX\n", GetLastError());
		exit(1);
	}

	result = GetLogicalProcessorInformationCall(logicalProcInfoBuffer, &bufferLen);
	if (!result)
	{
		fprintf(stderr, "GetLogicalProcessorInformation failed, status: %lX\n", GetLastError());
		GlobalFree(logicalProcInfoBuffer);
		exit(1);
	}

	ptr = logicalProcInfoBuffer;

	processorInfoCount = bufferLen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

	for (i = 0; i < processorInfoCount; i++)
	{
		switch (ptr->Relationship)
		{
		case RelationProcessorCore:
			logicalProcessorCount += CountSetBits(ptr->ProcessorMask);
			break;
		}
		++ptr;
	}

	if (logicalProcInfoBuffer)
	{
		GlobalFree(logicalProcInfoBuffer);
	}
	return logicalProcessorCount;
}

/*
  RestartPerfCntrs() -

  The Performance counters must be read twice to produce rate and
  percentage results.  This routine is called before the start of a
  benchmark to establish the initial counters.  It must be called a
  second time after the benchmark completes to collect the final state
  of the performance counters.  ReportPerfCntrs is called to print the
  results after the benchmark completes.
*/

void RestartPerfCntrs(PerfObj *PerfCntrs)
{
  DWORD returnLength = 0;  //Lint
  DWORD returnNumCPUs = 0;  //Lint
  DWORD i = 0;

  DWORD status;
  SYSTEM_INFO SystemInfo;
  DWORD numberOfProcessors = 0;

  // Is there a new function to get information about CPUs count?
  if (GetLogicalProcessorInformationCall)
  {	  
	  numberOfProcessors = GetCpuCount();
  }
  else
  {
	  // Use the legacy way
	  GetSystemInfo(&SystemInfo);
	  numberOfProcessors = SystemInfo.dwNumberOfProcessors;
  }

  // Move previous data from EndInfo to StartInfo.
  CopyMemory((PCHAR)&PerfCntrs->StartInfo[0],
	     (PCHAR)&PerfCntrs->EndInfo[0],
	     sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)*(MAXCPUS +1));

  PerfCntrs->StartTime = PerfCntrs->EndTime;

  // get the current CPUTIME information
  if ( (status = NtQuerySystemInformation( SystemProcessorPerformanceInformation,
					   (PCHAR)&PerfCntrs->EndInfo[0], sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)*MAXCPUS,
					   &returnLength )) != 0)
    {
      fprintf(stderr, "NtQuery failed, status: %lX\n", status);
      exit(1);
    }

  PerfCntrs->EndTime = ReadPerformanceCounter();

  // Validate that NtQuery returned a reasonable amount of data
  if ((returnLength % sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)) != 0)
    {
      fprintf(stderr, "NtQuery didn't return expected amount of data\n");
      fprintf(stderr, "Expected a multiple of %i, returned %lu\n",
	      sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION), returnLength);
      exit(1);
    }
  returnNumCPUs = returnLength / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);

  if (returnNumCPUs != (int)numberOfProcessors)
    {
      fprintf(stderr, "NtQuery didn't return expected amount of data\n");
      fprintf(stderr, "Expected data for %i CPUs, returned %lu\n",
	      (int)numberOfProcessors, returnNumCPUs);
      exit(1);
    }

  // Zero entries not returned by NtQuery
  ZeroMemory((PCHAR)&PerfCntrs->EndInfo[returnNumCPUs],
	     sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)*
	     (MAXCPUS +1 - returnNumCPUs));

  // Total all of the CPUs
  //      KernelTime needs to be fixed-up; it includes both idle &
  // true kernel time
  //  Note that kernel time also includes DpcTime & InterruptTime, but
  // I like this.
  for (i=0; i < returnNumCPUs; i++)
    {
      PerfCntrs->EndInfo[i].KernelTime.QuadPart         -= PerfCntrs->EndInfo[i].IdleTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].IdleTime.QuadPart     += PerfCntrs->EndInfo[i].IdleTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].KernelTime.QuadPart   += PerfCntrs->EndInfo[i].KernelTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].UserTime.QuadPart     += PerfCntrs->EndInfo[i].UserTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].DpcTime.QuadPart      += PerfCntrs->EndInfo[i].DpcTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].InterruptTime.QuadPart += PerfCntrs->EndInfo[i].InterruptTime.QuadPart;
      PerfCntrs->EndInfo[MAXCPUS].InterruptCount                += PerfCntrs->EndInfo[i].InterruptCount;
    }

}   /* RestartPerfCntrs */

/*
  ReportPerfCntrs() -
  This routine reports the results of the various performance
  counters.
*/

double ReportPerfCntrs(PerfObj *PerfCntrs)
{
  double tot_CPU_Util;
  int i;
  double duration;  // in milliseconds

  LARGE_INTEGER ActualDuration;

  SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION        DeltaInfo[MAXCPUS +1];

  LARGE_INTEGER   TotalCPUTime[MAXCPUS +1];

  SYSTEM_INFO SystemInfo;
  DWORD numberOfProcessors = 0;

  if (GetLogicalProcessorInformationCall)
  {
	  numberOfProcessors = GetCpuCount();
  }
  else
  {
	  GetSystemInfo(&SystemInfo);
	  numberOfProcessors = SystemInfo.dwNumberOfProcessors;
  }
  
  for (i=0; i <= MAXCPUS; i++)
    {
      DeltaInfo[i].IdleTime.QuadPart    = PerfCntrs->EndInfo[i].IdleTime.QuadPart -
	PerfCntrs->StartInfo[i].IdleTime.QuadPart;
      DeltaInfo[i].KernelTime.QuadPart          = PerfCntrs->EndInfo[i].KernelTime.QuadPart -
	PerfCntrs->StartInfo[i].KernelTime.QuadPart;
      DeltaInfo[i].UserTime.QuadPart    = PerfCntrs->EndInfo[i].UserTime.QuadPart -
	PerfCntrs->StartInfo[i].UserTime.QuadPart;
      DeltaInfo[i].DpcTime.QuadPart     = PerfCntrs->EndInfo[i].DpcTime.QuadPart -
	PerfCntrs->StartInfo[i].DpcTime.QuadPart;
      DeltaInfo[i].InterruptTime.QuadPart = PerfCntrs->EndInfo[i].InterruptTime.QuadPart -
	PerfCntrs->StartInfo[i].InterruptTime.QuadPart;
      DeltaInfo[i].InterruptCount               = PerfCntrs->EndInfo[i].InterruptCount -
	PerfCntrs->StartInfo[i].InterruptCount;

      TotalCPUTime[i].QuadPart =
	DeltaInfo[i].IdleTime.QuadPart +
	DeltaInfo[i].KernelTime.QuadPart +
	DeltaInfo[i].UserTime.QuadPart;
      // KernelTime already includes DpcTime & InterruptTime!
      // + DeltaInfo[i].DpcTime.QuadPart  +
      //  DeltaInfo[i].InterruptTime.QuadPart;
    }

  tot_CPU_Util = 100.0*(1.0 - (double)DeltaInfo[MAXCPUS].IdleTime.QuadPart/(double)TotalCPUTime[MAXCPUS].QuadPart);  //Lint

  // Re-calculate duration, since we may have stoped early due to cntr-C.
  ActualDuration.QuadPart = PerfCntrs->EndTime.QuadPart -
    PerfCntrs->StartTime.QuadPart;

  // convert to 100 usec (1/10th milliseconds) timebase.
  ActualDuration.QuadPart = (ActualDuration.QuadPart*10000)/TickHz.QuadPart;
  duration = (double)ActualDuration.QuadPart/10.0;  // duration in ms

  if (verbosity > 1)
    {
      fprintf(where,"ActualDuration (ms): %d\n", (int)duration);
    }

  if (verbosity > 1)
    {
      fprintf(where, "%% CPU    _Total");
      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t CPU %i", i);
	    }
	}
      fprintf(where, "\n");

      fprintf(where, "Busy      %5.2f", tot_CPU_Util);
      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.2f",
		      100.0*(1.0 - (double)DeltaInfo[i].IdleTime.QuadPart/(double)TotalCPUTime[i].QuadPart));  //Lint
	    }
	}
      fprintf(where, "\n");

      fprintf(where, "Kernel    %5.2f",
	      100.0*(double)DeltaInfo[MAXCPUS].KernelTime.QuadPart/(double)TotalCPUTime[MAXCPUS].QuadPart);  //Lint

      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.2f",
		      100.0*(double)DeltaInfo[i].KernelTime.QuadPart/(double)TotalCPUTime[i].QuadPart);  //Lint
	    }
	}
      fprintf(where, "\n");

      fprintf(where, "User      %5.2f",
	      100.0*(double)DeltaInfo[MAXCPUS].UserTime.QuadPart/(double)TotalCPUTime[MAXCPUS].QuadPart);

      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.2f",
		      100.0*(double)DeltaInfo[i].UserTime.QuadPart/TotalCPUTime[i].QuadPart);  //Lint
	    }
	}
      fprintf(where, "\n");

      fprintf(where, "Dpc       %5.2f",
	      100.0*(double)DeltaInfo[MAXCPUS].DpcTime.QuadPart/(double)TotalCPUTime[MAXCPUS].QuadPart);  //Lint

      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.2f",
		      100.0*(double)DeltaInfo[i].DpcTime.QuadPart/(double)TotalCPUTime[i].QuadPart);  //Lint
	    }
	}
      fprintf(where, "\n");

      fprintf(where, "Interrupt %5.2f",
	      100.0*(double)DeltaInfo[MAXCPUS].InterruptTime.QuadPart/(double)TotalCPUTime[MAXCPUS].QuadPart);  //Lint

      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.2f",
		      100.0*(double)DeltaInfo[i].InterruptTime.QuadPart/TotalCPUTime[i].QuadPart);  //Lint
	    }
	}
      fprintf(where, "\n\n");

      fprintf(where, "Interrupt/Sec. %5.1f",
	      (double)DeltaInfo[MAXCPUS].InterruptCount*1000.0/duration);

      if ((int)numberOfProcessors > 1)
	{
	  for (i=0; i < (int)numberOfProcessors; i++)
	    {
	      fprintf(where, "\t %5.1f",
		      (double)DeltaInfo[i].InterruptCount*1000.0/duration);
	    }
	}
      fprintf(where, "\n\n");
      fflush(where);
    }

  return (tot_CPU_Util);

}  /* ReportPerfCntrs */

/*
  ClosePerfCntrs() -

  This routine cleans up the performance counter APIs.
*/

void ClosePerfCntrs(PerfObj *PerfCntrs)
{
        GlobalFree(PerfCntrs);

        NtQuerySystemInformation = NULL;
}   /* ClosePerfCntrs */

void
cpu_start_internal(void)
{
  RestartPerfCntrs(PerfCntrs);
}

void
cpu_stop_internal(void)
{
  RestartPerfCntrs(PerfCntrs);
}

float
calc_cpu_util_internal(float elapsed_time)
{
  float correction_factor;

  memset(&lib_local_cpu_stats, 0, sizeof(lib_local_cpu_stats));

  /* It is possible that the library measured a time other than */
  /* the one that the user want for the cpu utilization */
  /* calculations - for example, tests that were ended by */
  /* watchdog timers such as the udp stream test. We let these */
  /* tests tell up what the elapsed time should be. */

  if (elapsed_time != 0.0) {
    correction_factor = (float) 1.0 +
      ((lib_elapsed - elapsed_time) / elapsed_time);
  }
  else {
    correction_factor = (float) 1.0;
  }

  if (debug) {
    fprintf(where, "correction factor: %f\n", correction_factor);
  }

  lib_local_cpu_stats.cpu_util = (float)ReportPerfCntrs(PerfCntrs);
  lib_local_cpu_stats.cpu_util *= correction_factor;
  return lib_local_cpu_stats.cpu_util;

}
