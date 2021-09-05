#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef WIN32 // UNIX 
#include "winpthreads.h"
#else // WINDOWS
#include <pthread.h>
#endif

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#ifndef bool
typedef uint8_t bool;
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifdef DEBUG
#define PRINT_FUNCTION_NAME() fprintf(stderr, " at %s:\n\t", __func__);
#else
#define PRINT_FUNCTION_NAME() fprintf(stderr, ":\n\t");
#endif

#define THROW_ERROR(...) \
    {\
			fflush(stdout);\
			fprintf(stderr, "\033[0;31m"); \
			fprintf(stderr, "Error"); \
			PRINT_FUNCTION_NAME() \
			fprintf(stderr, "\""); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\"\n\033[0m"); \
			exit(-1);\
    }

#define REPEAT(times)\
	for (int _i = 0; _i < times; _i++)

void *mallocWithError(size_t size){
  void *p = malloc(size);
  if (p == NULL) THROW_ERROR("Unable to allocate memory");
  return p;
}

#define BUFFER_SIZE 9 
#define DUMP_SIZE BUFFER_SIZE / 3

pthread_mutex_t reading_mutex, writing_mutex;
pthread_cond_t reading_can_produce, reading_can_consume, writing_can_produce, writing_can_consume;
u32 reading_producer_index, reading_consumer_index, writing_producer_index, writing_consumer_index;

char reading_buffer[BUFFER_SIZE];
u8 writing_buffer[BUFFER_SIZE]; 
bool reading_buffer_free, writing_buffer_free;


// THREAD CONTROL //////////
void mutexLock(pthread_mutex_t *mutex) {
	if (pthread_mutex_lock(mutex) != 0) THROW_ERROR("Error locking mutex");
}

void mutexUnlock(pthread_mutex_t *mutex) {
	if (pthread_mutex_unlock(mutex) != 0) THROW_ERROR("Error unlocking mutex");
}

void waitCondition(pthread_cond_t *cond, pthread_mutex_t *mutex){
	if (pthread_cond_wait(cond, mutex) != 0) THROW_ERROR("Unable to wait for condition");
}

void signalCondition(pthread_cond_t *cond){
	if (pthread_cond_signal(cond) != 0) THROW_ERROR("Unable to signal condition");
}
///////////////////////////

int readToCyclicBuffer(FILE *file){
	u32 distance_to_end = BUFFER_SIZE - reading_producer_index;
	int needs_dividing = distance_to_end < DUMP_SIZE;
	int c = 0;

	if (needs_dividing){
		c = fread(&reading_buffer[reading_producer_index], sizeof(char), distance_to_end, file);
		c += fread(reading_buffer, sizeof(char), DUMP_SIZE - distance_to_end, file);
		reading_producer_index = DUMP_SIZE - distance_to_end;
	}else{
		c = fread(&reading_buffer[reading_producer_index], DUMP_SIZE, sizeof(char), file);
		reading_producer_index = (reading_producer_index + DUMP_SIZE) % BUFFER_SIZE;
	}

	return c;
}

u32 getDistanceInBuffer(u32 a, u32 b){
	return a <= b? b - a: BUFFER_SIZE - a + b;  
}

void *readFile(void *arg){
	char *filename = (char *) arg;
	FILE *file = fopen(filename, "r");
	if (file == NULL) THROW_ERROR("Unable to open file: %s", filename);

	int c;

	do{	
		mutexLock(&reading_mutex);
		// Wait until able to write
		while (getDistanceInBuffer(reading_producer_index, reading_consumer_index) < DUMP_SIZE)
			waitCondition(&reading_can_produce, &reading_mutex);

		c = readToCyclicBuffer(file);
	
		signalCondition(&reading_can_consume);
		mutexUnlock(&reading_mutex);	
	}while(c != 0);

	// Check if any error occured
	if (ferror(file)) THROW_ERROR("An error occured while reading file");

	fclose(file);	
	reading_buffer_free = TRUE;
	signalCondition(&reading_can_consume);
	pthread_exit(NULL);
	return NULL;
}

u8 convertCharsToU8(const char chars[]){
	u8 result = 0;
	for (int i = 0; i < 2 ; i ++){
		result <<= 4;
		if (chars[i] >= '0' && chars[i] <= '9') // IF CHAR IS NUMBER
			result |= (u8) ((int)chars[i] - (int) '0');
		else if (chars[i] >= 'A' && chars[i] <= 'F') // IF IS UPPER LETTER
			result |= (u8) ((int)chars[i] - (int) 'A' + 10);
		else if (chars[i] >= 'a' && chars[i] <= 'f') // IF IS UPPER LETTER
			result |= (u8) ((int)chars[i] - (int) 'a' + 10);
		else THROW_ERROR("UNKOWN CHARACTER: %c", chars[i]);	
	}
	return result;
}

char vals[2];
void *convertFile(void *arg){
	(void) arg;
	u8 count = 0;

	while(!reading_buffer_free || getDistanceInBuffer(reading_consumer_index, reading_producer_index) != 1){
		// READ CHARS FROM READING BUFFER
		mutexLock(&reading_mutex);
		u32 dist = getDistanceInBuffer(reading_consumer_index, reading_producer_index);

		if (dist == 1 && reading_buffer_free) THROW_ERROR("END OF FILE REACHED");

		while(!reading_buffer_free && dist == 1){
			waitCondition(&reading_can_consume, &reading_mutex);
			dist = getDistanceInBuffer(reading_consumer_index, reading_producer_index);
		}

		reading_consumer_index = (reading_consumer_index + 1) % BUFFER_SIZE;

		char c = reading_buffer[reading_consumer_index];
		if (c != ' ' && c != 0 && c != '\r' && c != '\n' && c != '\t')
			vals[count++] = c;
		
		signalCondition(&reading_can_produce);
		mutexUnlock(&reading_mutex);

		if (count == 2){
			u8 val = convertCharsToU8(vals);

			// WRITE CONVERTED CHARS TO WRITING BUFFER
			mutexLock(&writing_mutex);
				
			while(getDistanceInBuffer(writing_producer_index, writing_consumer_index) == 1)
				waitCondition(&writing_can_produce, &writing_mutex);

			writing_buffer[writing_producer_index] = val;
			writing_producer_index = (writing_producer_index + 1) % BUFFER_SIZE;

			signalCondition(&writing_can_consume);
			mutexUnlock(&writing_mutex);
			count = 0;
		}	
	}
	writing_buffer_free = TRUE;
	signalCondition(&writing_can_consume);
	pthread_exit(NULL);
	return NULL;
}

void writeFromCyclicBuffer(FILE *file, u32 size){
	u8 tmp[size];

	for (int i = 1; i <= size; i++)
		tmp[i-1] = writing_buffer[(writing_consumer_index + i) % BUFFER_SIZE];
	
	fwrite(tmp, sizeof(u8), size, file);
	writing_consumer_index = (writing_consumer_index + size)%BUFFER_SIZE;
}

void *writeFile(void *arg){
	char *filename = (char *) arg;
	FILE *file = fopen(filename, "wb");
	if (file == NULL) THROW_ERROR("Unable to open file: %s", filename);

	while(!writing_buffer_free || getDistanceInBuffer(writing_consumer_index, writing_producer_index) != 1){	
		mutexLock(&writing_mutex);

		while (!writing_buffer_free && getDistanceInBuffer(writing_consumer_index, writing_producer_index) < DUMP_SIZE)
			waitCondition(&writing_can_consume, &writing_mutex);

		u32 size;
		
		if (writing_buffer_free)
			size = getDistanceInBuffer(writing_consumer_index, writing_producer_index) - 1;
		else
			size = DUMP_SIZE;

		writeFromCyclicBuffer(file, size);
	
		signalCondition(&writing_can_produce);
		mutexUnlock(&writing_mutex);	
	}
	
	fclose(file);	
	pthread_exit(NULL);
	return NULL;
}

pthread_t *createReadingThread(char file[]){
	pthread_t *thread = (pthread_t *) mallocWithError(sizeof(pthread_t));
	
	if (pthread_create(thread, NULL, readFile, (void *) file))
			THROW_ERROR("Couldnt create thread");
	return thread;
}

pthread_t *createConvertingThread(){
	pthread_t *thread = (pthread_t *) mallocWithError(sizeof(pthread_t));
	
	if (pthread_create(thread, NULL, convertFile, NULL))
			THROW_ERROR("Couldnt create thread");
	return thread;
}

pthread_t *createWritingThread(char file[]){
	pthread_t *thread = (pthread_t *) mallocWithError(sizeof(pthread_t));
	
	if (pthread_create(thread, NULL, writeFile, (void *) file))
			THROW_ERROR("Couldnt create thread");
	return thread;
}


int main(int argc, char *argv[]){
	if (argc != 3) THROW_ERROR("Invalid number of command line arguments");
	char *file_in = argv[1];
	char *file_out = argv[2];	
	
	pthread_mutex_init(&reading_mutex, NULL);	
	pthread_mutex_init(&writing_mutex, NULL);	

	pthread_cond_init(&reading_can_consume, NULL);
	pthread_cond_init(&reading_can_produce, NULL);
	pthread_cond_init(&writing_can_consume, NULL);
	pthread_cond_init(&writing_can_produce, NULL);

	reading_consumer_index = BUFFER_SIZE -1;
	writing_consumer_index = BUFFER_SIZE -1;
	reading_producer_index = 0;
	writing_producer_index = 0;

	reading_buffer_free = FALSE;
	writing_buffer_free = FALSE;

	// CREATE WORKING THREADS 
	pthread_t *readingThread = createReadingThread((void *) file_in);
	pthread_t *convertingThread = createConvertingThread();
	pthread_t *writingThread = createWritingThread((void *) file_out);

	// WAIT UNTIL PROCESSING IS FINISHED
	pthread_join(*readingThread, NULL);
	pthread_join(*convertingThread, NULL);
	pthread_join(*writingThread, NULL);

	free(readingThread);
	free(convertingThread);
	free(writingThread);
	
	pthread_mutex_destroy(&reading_mutex);
	pthread_mutex_destroy(&writing_mutex);

	return 0;
}
