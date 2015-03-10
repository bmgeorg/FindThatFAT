#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include "dataTypes.h"

#define IMAGE_SIZE_IN_BYTES 1474560
#define ROOT_DIR_SIZE_IN_BYTES 7168
#define SECTOR_SIZE_IN_BYTES 512
#define FILE_HEAD_SIZE_IN_BYTES 32

#define FAT_1_SECTOR 1
#define ROOT_DIR_SECTOR 19
#define DATA_SECTOR 33
#define LAST_CLUSTER_INDEX 2848

#define FILE_NAME_MAX_LENGTH 8
#define EXTENSION_MAX_LENGTH 3

#define FILES_PER_CLUSTER (SECTOR_SIZE_IN_BYTES/FILE_HEAD_SIZE_IN_BYTES)

static int numFound = 0;
static image *img;
static fat *fat1;
static rootDirectory *root;
static char *outputDirectory;

#include "helperFunctions.lib"

void processFile(fileHead *f, char *path);
void searchDirectory(fileHead *dir, char *path);

//Result must be freed
cluster** getClusters(fileHead *f, int *returnedNumClusters) {
#define INITIAL_ARR_CAPACITY 10
	cluster **result = malloc(INITIAL_ARR_CAPACITY*sizeof(cluster*));
	int arrCapacity = INITIAL_ARR_CAPACITY;
	int numClusters = 0;
	if(isDeleted(f)) {
		int fileSize = getFileSize(f);
		int clusterIndex = getFirstClusterIndex(f);
		//for a deleted file, pull only as many clusters as are necessary to contain the file size
		//for a deleted directory, pull exactly one cluster
		do {
			if(!isMarkedFatFree(clusterIndex) || clusterIndex == LAST_CLUSTER_INDEX+1) {
				break;
			}
			if(numClusters == arrCapacity) {
				result = realloc(result, 2*arrCapacity*sizeof(cluster*));
				arrCapacity *= 2;
			}
			result[numClusters++] = getCluster(clusterIndex);
			clusterIndex++;
			fileSize -= SECTOR_SIZE_IN_BYTES;
		} while(fileSize > 0);
	} else {
		int clusterIndex = getFirstClusterIndex(f);
		//keep pulling clusters until end of file
		while(clusterIndex < 0xFF8) {
			if(numClusters == arrCapacity) {
				result = realloc(result, 2*arrCapacity*sizeof(cluster*));
				arrCapacity *= 2;
			}
			result[numClusters++] = getCluster(clusterIndex);
			clusterIndex = getNextClusterIndex(clusterIndex);
		}
	}
	*returnedNumClusters = numClusters;
	return result;
}

void printFileContents(char *outputPath, fileHead *f) {
	assert(!isDirectory(f));
	int outputFile;
	char *outputPage;
	int sizeLeft = getFileSize(f);
	int offset = 0;

	outputFile = open(outputPath, O_RDWR | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
	if(outputFile == -1) {
		fprintf(stderr, "Could not create file %s\n", outputPath);
		perror("");
		exit(1);
	}
	//use ftruncate to make the file size sizeLeft
	ftruncate(outputFile, sizeLeft);
	//mmap fails if length = 0, so don't mmap an empty file
	if(sizeLeft > 0) {
		outputPage = mmap(NULL, sizeLeft, PROT_WRITE | PROT_READ, MAP_SHARED, outputFile, 0);
		if(outputPage == MAP_FAILED) {
			fprintf(stderr, "Could not mmap to output file: %s\n", outputPath);
			perror("");
			exit(1);
		}
	
		int numClusters;
		cluster **clusters = getClusters(f, &numClusters);
		for(int i = 0; i < numClusters; i++) {
			if(sizeLeft >= SECTOR_SIZE_IN_BYTES) {
				memcpy(outputPage+offset, clusters[i], SECTOR_SIZE_IN_BYTES);
				sizeLeft -= SECTOR_SIZE_IN_BYTES;
				offset += SECTOR_SIZE_IN_BYTES;
			} else {
				memcpy(outputPage+offset, clusters[i], sizeLeft);
				sizeLeft = 0;
				//Should only happen on last iteration
				assert(i == numClusters-1);
			}
		}
	
		//cleanup memory
		free(clusters);

		//If couldn't find all of the file, truncate rest.
		if(sizeLeft != 0) {
			ftruncate(outputFile, getFileSize(f)-sizeLeft);
		}

		//Unmap file and close
		if(munmap(outputPage, getFileSize(f)) == -1) {
			fprintf(stderr, "Could not munmap output file: %s\n", outputPath);
			perror("");
			exit(1);
		}
	
	}
	if(close(outputFile) == -1) {
		fprintf(stderr, "Could not close file %s\n", outputPath);
		perror("");
		exit(1);
	}
}

void processFile(fileHead *f, char *path) {
	if(f[0] == 0) {
		return;
	}
	//Manipulating strings in c sucks
	char *name = copyFileName(f);
	char *ext = copyFileExt(f);
	char *fullName = malloc(strlen(name)+strlen(ext)+2);
	if(strcmp(ext, "") == 0) {
		sprintf(fullName, "%s", name);
	} else {
		sprintf(fullName, "%s.%s", name, ext);
	}
	char *fullPath = malloc(strlen(path)+strlen(fullName)+2);
	sprintf(fullPath, "%s%s", path, fullName);
	if(isDirectory(f)) {
		int len = strlen(fullPath);
		fullPath[len] = '/';
		fullPath[len+1] = '\0';
		searchDirectory(f, fullPath);
	} else {
		char *assignedName = malloc(5+EXTENSION_MAX_LENGTH+getNumDigits(numFound));
		sprintf(assignedName, "file%d.%s", numFound, ext);
		numFound++;
		char *outputPath = malloc(strlen(outputDirectory)+strlen(assignedName)+2);
		sprintf(outputPath, "%s/%s", outputDirectory, assignedName);
		
		if(!isDeleted(f)) {
			printf("%s\tNORMAL\t%s\t%d\n", fullName, fullPath, getFileSize(f));
		} else {
			printf("%s\tDELETED\t%s\t%d\n", fullName, fullPath, getFileSize(f));
		}
		printFileContents(outputPath, f);
		//cleanup memory
		free(outputPath);
		free(assignedName);
	}
	//cleanup memory
	free(fullName);
	free(name);
	free(ext);
	free(fullPath);
}


void searchDirectory(fileHead *dir, char *path) {
	int numClusters;
	cluster **clusters = getClusters(dir, &numClusters);
	for(int i = 0; i < numClusters; i++) {
		cluster *c = clusters[i];
		for(int j = 0; j < FILES_PER_CLUSTER; j++) {
			//skip first two files (current directory and parent directory) in first cluster
			if(i == 0 && j == 0) {
				j = 2;
			}
			processFile(c + j*FILE_HEAD_SIZE_IN_BYTES, path);
		}
	}
	free(clusters);
}

void searchRoot() {
	int numFiles = ROOT_DIR_SIZE_IN_BYTES/FILE_HEAD_SIZE_IN_BYTES;
	for(int i = 0; i < numFiles; i++) {
		processFile(root + i*FILE_HEAD_SIZE_IN_BYTES, "/");
	}
}

int main(int argc, char **argv) {
	int imageFile;
	struct stat imageFileData;

	if(argc != 3) {
		fprintf(stderr, "Usage: ./executable <image filename> <output_directory>\n");
		exit(1);
	}
	
	imageFile = open(argv[1], O_RDONLY);
	if(imageFile == -1) {
		fprintf(stderr, "Could not open file: %s\n", argv[1]);
		perror("");
		exit(1);
	}
	
	if (fstat(imageFile, &imageFileData) == -1) {
		perror("Could not get file metadata with fstat");
		fprintf(stderr, "Assuming file size equals %d\n", IMAGE_SIZE_IN_BYTES);
	} else if(imageFileData.st_size != IMAGE_SIZE_IN_BYTES) {
		fprintf(stderr, "Image file size does not equal expected size\n");
		fprintf(stderr, "Expected Size: %d\n", IMAGE_SIZE_IN_BYTES);
		fprintf(stderr, "Actual Size: %ld\n", imageFileData.st_size);
		exit(1);
	}
	
	img = mmap(NULL, IMAGE_SIZE_IN_BYTES, PROT_READ, MAP_PRIVATE, imageFile, 0);
	if(img == MAP_FAILED) {
		fprintf(stderr, "Could not map image file: %s\n", argv[1]);
		perror("");
		exit(1);
	}

	fat1 = img + SECTOR_SIZE_IN_BYTES*FAT_1_SECTOR;
	root = img + SECTOR_SIZE_IN_BYTES*ROOT_DIR_SECTOR;
	outputDirectory = argv[2];
	searchRoot();
}
