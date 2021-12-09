#ifndef _KERNEL_PIPE_H
#define _KERNEL_PIPE_H

#define PIPE_BUFFER_SIZE 16384

typedef struct pipe_control_block
{
	FCB *reader, *writer;
	CondVar has_space; // For blocking writer if no space is available
	CondVar has_data; // For blocking reader until data are available

	int w_position, r_position;

	char BUFFER[PIPE_BUFFER_SIZE];
} pipe_cb;


int sys_Pipe(pipe_t* pipe);

int pipe_read(void* pipecb_t, char* buf, unsigned int n);

int pipe_write(void* pipecb_t, const char* buf, unsigned int n);

int pipe_reader_close(void* _pipecb);

int pipe_writer_close(void* _pipecb);


#endif

