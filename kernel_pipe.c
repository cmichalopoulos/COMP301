
#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_pipe.h"
#include "kernel_cc.h"


file_ops reader_file_ops = {
	.Open = NULL,
	.Read = pipe_read,
	.Write = NULL,
	.Close = pipe_reader_close
};

file_ops writer_file_ops = {
	.Open = NULL,
	.Read = NULL,
	.Write = pipe_write,
	.Close = pipe_writer_close
};


int sys_Pipe(pipe_t* pipe)
{

	Fid_t fid[2];
	FCB* fcb[2];


	// Reserve FIDT, FCB. I define to reserve 2 of each, one for read and one for write
	if(!FCB_reserve(2, fid, fcb)){
		return -1;
	} else {
		pipe_cb *pipecb = (pipe_cb* )xmalloc(sizeof(pipe_cb));

		// The two file descriptors. fd[0] for read and fd[1] for write (READ -> r, WRITE->w)
		// General, happens at start and has nothing to do with reserving FCB and other pipecb variables.
		// read, write declared in tinyos.h
		pipe->read = fid[0];
		pipe->write = fid[1];

		pipecb->reader = fcb[0];
		pipecb->writer = fcb[1];

		// Read and write position in buffer of pipe_cb
		pipecb->r_position = 0;
		pipecb->w_position = 0;

		// Initialiaze reader stuff
		pipecb->reader = 0;
		fcb[0]->streamobj = pipecb;
		fcb[0]->streamfunc = &reader_file_ops;

		// Initialize writer stuff
		pipecb->writer = 0;
		fcb[1]->streamobj = pipecb;
		fcb[1]->streamfunc = &writer_file_ops;

		// Has_space blocks writer if there is no space to write, has_data blocks the reader until data to read are available
		pipecb->has_data = COND_INIT;
		pipecb->has_space = COND_INIT;

		return 0;

	}
}

int pipe_write(void* pipecb_t, const char* buf, unsigned int n){
	pipe_cb *pipecb = (pipe_cb* ) pipecb_t;
	int counter = 0;

	// Check if Writer or Reader is closed
	if(pipecb->writer == NULL || pipecb->reader == NULL){
		return -1; 	
	}

	// If writer loops buffer and reaches reader, enable kernel_wait on him with has_space
	while(((pipecb->w_position+1)%PIPE_BUFFER_SIZE) == pipecb->r_position && pipecb->reader != NULL){
		kernel_wait((&pipecb->has_space), SCHED_PIPE);
	}

	// Do write normally as long as writer is behind reader and there is space to write and there is space in the buffer
	while((counter != n) && (counter < PIPE_BUFFER_SIZE) && ((pipecb->w_position+1)%PIPE_BUFFER_SIZE != pipecb->r_position) && (pipecb->writer != NULL) && (pipecb->reader != NULL))
	{
		pipecb->BUFFER[pipecb->w_position] = buf[counter];
		pipecb->w_position = (pipecb->w_position + 1) % PIPE_BUFFER_SIZE;
		counter ++;
	}

	kernel_broadcast(&(pipecb->has_data));
	return counter;
}


int pipe_writer_close(void* _pipecb){	
	pipe_cb *pipecb = (pipe_cb*) _pipecb;

	if(pipecb != NULL){
		pipecb->writer = NULL; // closing the writer...

		if(pipecb->reader != NULL){
			kernel_broadcast(&(pipecb->has_data));
		} else {
			pipecb = NULL;
			free(pipecb);
		}
	}
	return 0;
}


int pipe_read(void* pipecb_t, char* buf, unsigned int n){
	pipe_cb  *pipecb = (pipe_cb*) pipecb_t;
	int counter = 0; 
	
	// Check if Reader is closed. 
	if(pipecb->reader == NULL){
		return -1; // no need to do anything in this function
	}

	// Check if Reader has reached Writer 
	// If yes and there is nothing to read, activate kernel_wait with has_Data
	while(pipecb->r_position == pipecb->w_position && pipecb->writer != NULL) {
		kernel_wait(&(pipecb->has_data), SCHED_PIPE);
	}

	// If there are no new writes and the reader is behind writer, keep on reading everything that remains
	if(pipecb->writer == NULL){
		while(pipecb->r_position < pipecb->w_position){	
			if (counter == n){
				return -1;
			}
			buf[counter] = pipecb->BUFFER[pipecb->r_position];
			pipecb->r_position = (pipecb->r_position + 1) % PIPE_BUFFER_SIZE;
			counter ++;
		}
		return counter;
	}

	// If reader keeps on reading and writer keeps on writing and counter is not equal to size and less than the buffer size, keep on reading.
	while(counter != n && counter < PIPE_BUFFER_SIZE && (pipecb->r_position != pipecb->w_position)){
		buf[counter] = pipecb->BUFFER[pipecb->r_position];
		pipecb->r_position = (pipecb->r_position + 1) % PIPE_BUFFER_SIZE;
		counter ++;
	}

	kernel_broadcast(&pipecb->has_space);
	return counter;

}


int pipe_reader_close(void* _pipecb){
	pipe_cb *pipecb = (pipe_cb*) _pipecb;
	if(pipecb != NULL){
		pipecb->reader = NULL; // closing the reader...

		if(pipecb->writer != NULL){
			kernel_broadcast(&(pipecb->has_space));
		} else {
			pipecb = NULL;
			free(pipecb);
		}
	}

	return 0;
}


