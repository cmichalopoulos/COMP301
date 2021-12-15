#ifndef KERNEL_SYSINFO_H
#define KERNEL_SYSINFO_H

#include "tinyos.h"

int procinfo_read(void*, char*, unsigned int);

int procinfo_close(void*);

int procinfo_write(void*, const char*, unsigned int);

file_ops proc_info ={
  .Open = NULL,
  .Read = procinfo_read,
  .Close = procinfo_close,
  .Write = procinfo_write,
};

typedef struct proc_info_control_block{
	procinfo* curinfo;
	int pcb_cursor;
}procinf_cb;


#endif