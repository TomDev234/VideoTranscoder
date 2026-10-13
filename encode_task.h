/*
 * One background process encodes the queue. It never touches MUI.
 * The main task drains VT_Encode_Signal() and then updates jobs.
 */
#ifndef VT_ENCODE_TASK_H
#define VT_ENCODE_TASK_H

#include "job.h"

BOOL VT_Encode_Init(void);
void VT_Encode_Shutdown(VTJobList *jobs);
ULONG VT_Encode_Signal(void);
BOOL VT_Encode_IsReady(void);
BOOL VT_Encode_IsRunning(void);

/* Copies waiting jobs and returns at once. Output names are chosen
   when each job actually starts. */
BOOL VT_Encode_Submit(VTJobList *jobs, const char *output_drawer);

/* Stops the running file and leaves the rest waiting. */
BOOL VT_Encode_Stop(void);

/* Applies queued progress to the job list. status may be NULL.
   TRUE means the list should be redrawn. */
BOOL VT_Encode_Drain(VTJobList *jobs, char *status, ULONG status_len);

#endif
