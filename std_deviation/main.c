#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#define MAX_SOURCE_SIZE (0x100000)
#define WORK_GROUP_SIZE 64
#define FILE_NAME "dataset_50K.txt" // DATASET FILE HERE
#define GPU "GeForce"

const char *parallelSum_kernel = "\n" \
"__kernel void parallelSum(__global const double* input, __global double* groupSum, __local double* localSum) \n" \
"{                                                                                  \n" \
"   uint localID   = get_local_id(0);                                                \n" \
"   uint globalID  = get_global_id(0);                                               \n" \
"   uint groupID = get_group_id(0);                                                 \n" \
"   uint groupSize = get_local_size(0);                                              \n" \
"                                                                                   \n" \
"   localSum[localID] = input[globalID];                                            \n" \
"                                                                                   \n" \
"   for(uint stride = groupSize / 2; stride > 0; stride /= 2)                      \n" \
"   {                                                                               \n" \
"      //wait for all local memory to get to this point and have their localSum[localID]              \n" \
"      //available. This is so that we can add the current element + stride element                   \n" \
"      barrier(CLK_LOCAL_MEM_FENCE);                                                \n" \
"      if(localID < stride)                                                         \n" \
"      {                                                                            \n" \
"        localSum[localID] += localSum[localID + stride];                        \n" \
"      }                                                                            \n" \
"   }                                                                               \n" \
"                                                                                   \n" \
"   if(localID == 0)                                                                \n" \
"   {                                                                               \n" \
"      groupSum[groupID] = localSum[0];                                             \n" \
"   }                                                                               \n" \
"}                                                                                  \n" \
"\n";

double getTime()
{
  struct timeval t;
  double sec, msec;

  while(gettimeofday(&t, NULL) != 0);
  sec = t.tv_sec;
  msec = t.tv_usec;
  sec = sec + msec/1000000.0;

  return sec;
}



int countDataEntries()
{
  FILE *file = fopen(FILE_NAME, "r");
  double num;
  int count = 0;
  while(fscanf(file, "%lf", &num) > 0)
  {
    count++;
  }
  return count;
}

void storeDataToProcess(double* data)
{
	FILE *file = fopen(FILE_NAME, "r");
	double num;
  int i = 0;
	while(fscanf(file, "%lf" ,&num) > 0)
	{
		data[i] = num;
    i++;
	}
	fclose(file);
}

void testPrintData(double* data, int data_size)
{
  int i;
  for(i=1; i < data_size + 1; i++)
  {
    printf("%d : %lf \n", i, data[i-1]);
  }
}


// calculate average sequentially
double seq_average(double* data, int data_size)
{
  int i;
  double average = 0;
  double accumulate = 0;

  for(i = 0; i < data_size; i++)
  {
    accumulate += data[i];
  }

  average = accumulate/data_size;

  printf("seq sum : %lf \n", accumulate);

  return average;
}


// calculate standard deviation sequentially
double calculateSequentialStdDev(double* input, int data_size, double mean) {
  int i;
  double* temp = malloc(data_size * sizeof(double));
  double sum = 0;
  for (i = 0; i < data_size; i++) {
    temp[i] = (input[i] - mean) * (input[i] - mean);
    sum += temp[i];
  }

  printf("sequential variance: %lf\n", sum/data_size);

  return sqrt(sum/data_size);
}


int main (int argc, char** argv)
{
  //count how many entries are there
  double* data;
  double* std_dev_results;
  double* sum_results;

  int data_size;
  data_size = countDataEntries();

  printf("Count: %d \n",data_size);

  data = malloc(data_size *sizeof(double));
  if(data == NULL)
  {
    printf("failed to malloc input \n");
  }

	storeDataToProcess(data);

  double average = 0.0;
  double t0 = getTime();
  average = seq_average(data,data_size);
  double t1 = getTime();
  printf("time to sequentially calculate avg: %lf\n", t1-t0);
  double seq_std_dev = calculateSequentialStdDev(data, data_size, average);
  double t2 = getTime();
  printf("time to sequentially calcualte std dev: %lf\n", t2-t1);
  printf("total sequential time: %lf\n", t2-t0);

  printf("sequential avg = %lf \n", average);
  printf("sequential std_deviation: %lf\n", seq_std_dev);
  //DONE SEQUENTIAL--

 //START OPENCL CALCULATIONs
  int error;

  size_t globalSize = data_size;
  size_t localSize = WORK_GROUP_SIZE;

  if((data_size % localSize) != 0)
  {
    printf("data size is not divisible by workgroup size. Datasize is : %d, workgroup size: %d \n", data_size, WORK_GROUP_SIZE);
    exit(1);
  }

  int numberOfWorkGroup = data_size/localSize;

  printf("\ndatasize: %d, workgroup: %d, numberofworkgroup: %d \n", data_size, WORK_GROUP_SIZE, numberOfWorkGroup);


  //holder for std_dev_results from each workgroup
  std_dev_results = malloc(data_size*sizeof(double));
  if(std_dev_results == NULL)
  {
    printf("failed to malloc std_dev_results!!! \n");
  }

  //initilise result array to 0
  int i = 0;
  for(i = 0; i < data_size; i++)
  {
    std_dev_results[i] = 0.0;
  }

  //holder for sum_results from each workgroup
  sum_results = malloc(numberOfWorkGroup*sizeof(double));
  if(sum_results == NULL)
  {
    printf("failed to malloc sum_results!!! \n");
  }

  //initilise result array to 0
  for(i = 0; i < numberOfWorkGroup; i++)
  {
    sum_results[i] = 0.0;
  }


  // Load the kernel source code into the array source_str
  FILE *fp;
  char *source_str;
  size_t source_size;

  fp = fopen("std_deviation_kernel.cl", "r");
  if (!fp) {
      fprintf(stderr, "Failed to load kernel.\n");
      exit(1);
  }
  source_str = (char*)malloc(MAX_SOURCE_SIZE);
  source_size = fread( source_str, 1, MAX_SOURCE_SIZE, fp);
  fclose( fp );


  cl_platform_id platform_id = NULL;
  cl_device_id device_id = NULL;
  cl_uint ret_num_devices;
  cl_uint ret_num_platforms;
  cl_int ret = clGetPlatformIDs(1, &platform_id, &ret_num_platforms);
  error = clGetDeviceIDs( platform_id, CL_DEVICE_TYPE_ALL, 1,
            &device_id, &ret_num_devices);
  if(error != CL_SUCCESS)
  {
    printf("failed to create a device group");
    return EXIT_FAILURE;
  }

  //create a compute context with a gpu
  cl_context context = clCreateContext(NULL, 1, &device_id, NULL, NULL, &error);
  if(!context)
  {
    printf("failed to create a context");
    return EXIT_FAILURE;
  }

  cl_command_queue commands = clCreateCommandQueue(context, device_id, 0, &error);
  if(!commands)
  {
    printf("failed to create a command queue \n");
    return EXIT_FAILURE;
  }

  // create sum program
  cl_program sum_program = clCreateProgramWithSource(context, 1, (const char **)&parallelSum_kernel, NULL, &error);
  if(!sum_program)
  {
    printf("cant create opencl program");
    exit(1);
  }

  //create  program object for a context, load kernel code
  cl_program std_dev_program = clCreateProgramWithSource(context, 1,
          (const char **)&source_str, (const size_t *)&source_size, &ret);
  if(!std_dev_program)
  {
    printf("cant create opencl std_dev_program");
    exit(1);
  }

  // build sum program
  error = clBuildProgram(sum_program, 1, &device_id, NULL, NULL, NULL);
  if(error != CL_SUCCESS)
  {
    size_t len;
    char buffer[2048];

    printf("Error: Failed to build sum_program executable!\n");
    clGetProgramBuildInfo(sum_program, device_id, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, &len);
    printf("%s\n", buffer);
    exit(1);
  }

  error = clBuildProgram(std_dev_program, 1, &device_id, NULL, NULL, NULL);
  if(error != CL_SUCCESS)
  {
    printf("%d\n", error);
    //only if failed, do this
    size_t len;
    char *buffer;

    printf("error: failed to build std_dev_program executable \n");

    clGetProgramBuildInfo(std_dev_program, device_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &len);
    buffer = malloc(len);
    clGetProgramBuildInfo(std_dev_program, device_id, CL_PROGRAM_BUILD_LOG, len, buffer, NULL);
    printf("%s\n", buffer);

    exit(1);
  }


  // create sum kernel
  cl_kernel sum_kernel = clCreateKernel(sum_program, "parallelSum", &error);
  if(!sum_kernel || error != CL_SUCCESS)
  {
    printf("failed to create sum_kernel \n");
    exit(1);
  }

  // create std_dev kernel
  cl_kernel std_dev_kernel = clCreateKernel(std_dev_program, "std_deviation", &error);
  if(!std_dev_kernel || error != CL_SUCCESS)
  {
    printf("failed to create kernel \n");
    exit(1);
  }


  cl_mem input = clCreateBuffer(context, CL_MEM_READ_ONLY, data_size*sizeof(double), NULL, NULL);
  cl_mem sum_output = clCreateBuffer(context, CL_MEM_READ_ONLY, numberOfWorkGroup*sizeof(double), NULL, NULL);


  error = clEnqueueWriteBuffer(commands, input, CL_TRUE, 0, sizeof(double) * data_size, data, 0, NULL, NULL);
  error = clEnqueueWriteBuffer(commands, sum_output, CL_TRUE, 0,  numberOfWorkGroup*sizeof(double), sum_results, 0, NULL, NULL);

  if(error != CL_SUCCESS)
  {
    printf("error failed to enqueu buffer to device\n");
    printf("error: %d\n", error);
    exit(1);
  }


  //set kernel arguments
  error = 0;
  double mean = 1;
  error = clSetKernelArg(sum_kernel, 0, sizeof(cl_mem), &input);
  error |= clSetKernelArg(sum_kernel,1, sizeof(cl_mem), &sum_output);
  error |= clSetKernelArg(sum_kernel,2, localSize*sizeof(double), NULL);

  if(error != CL_SUCCESS)
  {
    printf("failed to set arguments \n");
    exit(1);
  }

  t0 = getTime();

  //enqueue command to execute on device
  error = clEnqueueNDRangeKernel(commands, sum_kernel, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
  if(error != CL_SUCCESS)
  {
    printf("failed to exe sum_kernel %d \n", error);
    exit(1);
  }

  clFinish(commands);

  t1 = getTime();
  printf("parallel time to calculate mean: %lf\n", t1-t0);

  error = clEnqueueReadBuffer(commands, sum_output, CL_TRUE, 0, sizeof(double)*numberOfWorkGroup, sum_results, 0, NULL, NULL);
  if(error)
  {
    printf("failed to read sum_results \n");
    printf("error: %d\n", error);
    exit(1);
  }

    //Read the std_dev_results from GPU
  double sum_resultsFromGPU = 0;
  double averageFromGPU = 0;

  for(i = 0; i < numberOfWorkGroup; i++)
  {
    sum_resultsFromGPU += sum_results[i];
  }

  averageFromGPU = sum_resultsFromGPU / data_size;
  printf("AVG :Results from GPU is %lf \n", averageFromGPU);



  // STD DEV STARTS HERE


  cl_mem std_dev_output = clCreateBuffer(context, CL_MEM_READ_ONLY, data_size*sizeof(double), NULL, NULL);

  if(!input || !std_dev_output)
  {
    printf("cant create buffer");
    exit(1);
  }

  error = clEnqueueWriteBuffer(commands, input, CL_TRUE, 0, sizeof(double) * data_size, data, 0, NULL, NULL);
  error = clEnqueueWriteBuffer(commands, std_dev_output, CL_TRUE, 0,  data_size*sizeof(double), std_dev_results, 0, NULL, NULL);

  if(error != CL_SUCCESS)
  {
    printf("error failed to enqueu buffer to device\n");
    printf("error: %d\n", error);
    exit(1);
  }


  //set kernel arguments
  error = 0;
  //double mean = 1;
  error = clSetKernelArg(std_dev_kernel, 0, sizeof(cl_mem), &input);
  error |= clSetKernelArg(std_dev_kernel,1, sizeof(cl_mem), &std_dev_output);
  error |= clSetKernelArg(std_dev_kernel,2, localSize*sizeof(double), NULL);
  error |= clSetKernelArg(std_dev_kernel, 3, sizeof(double), &averageFromGPU);

  if(error != CL_SUCCESS)
  {
    printf("failed to set arguments \n");
    exit(1);
  }

  t2 = getTime();

  //enqueue command to execute on device
  error = clEnqueueNDRangeKernel(commands, std_dev_kernel, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
  if(error != CL_SUCCESS)
  {
    printf("failed to exe std_dev_kernel %d \n", error);
    exit(1);
  }

  clFinish(commands);

  double t3 = getTime();
  printf("parallel time to calculate diff from mean: %lf\n", t3-t2);

  error = clEnqueueReadBuffer(commands, std_dev_output, CL_TRUE, 0, sizeof(double)*data_size, std_dev_results, 0, NULL, NULL);
  if(error)
  {
    printf("failed to read std_dev_results \n");
    printf("error: %d\n", error);
    exit(1);
  }


  // SUM UP AND CALCULATE VARIANCE IN PARALLEL
  double* variance_sum_results;
  variance_sum_results = malloc(numberOfWorkGroup*sizeof(double));
  if(variance_sum_results == NULL)
  {
    printf("failed to malloc variance_sum_results!!! \n");
  }

  //initilise result array to 0
  for(i = 0; i < numberOfWorkGroup; i++)
  {
    variance_sum_results[i] = 0.0;
  }


  input = clCreateBuffer(context, CL_MEM_READ_ONLY, data_size*sizeof(double), NULL, NULL);
  cl_mem var_sum_output = clCreateBuffer(context, CL_MEM_READ_ONLY, numberOfWorkGroup*sizeof(double), NULL, NULL);
  error = clEnqueueWriteBuffer(commands, input, CL_TRUE, 0, sizeof(double) * data_size, std_dev_results, 0, NULL, NULL);
  error = clEnqueueWriteBuffer(commands, var_sum_output, CL_TRUE, 0,  numberOfWorkGroup*sizeof(double), variance_sum_results, 0, NULL, NULL);

  error = clSetKernelArg(sum_kernel, 0, sizeof(cl_mem), &input);
  error |= clSetKernelArg(sum_kernel,1, sizeof(cl_mem), &var_sum_output);
  error |= clSetKernelArg(sum_kernel,2, localSize*sizeof(double), NULL);

  double t4 = getTime();

  error = clEnqueueNDRangeKernel(commands, sum_kernel, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
  if(error != CL_SUCCESS)
  {
    printf("failed to exe sum_kernel %d \n", error);
    exit(1);
  }

  clFinish(commands);

  double t5 = getTime();
  printf("parallel time to calculate sum of difference from mean: %lf\n", t5-t4);

  printf("total time parallel: %lf\n", getTime() - t0);

  error = clEnqueueReadBuffer(commands, var_sum_output, CL_TRUE, 0, sizeof(double)*numberOfWorkGroup, variance_sum_results, 0, NULL, NULL);
  if(error)
  {
    printf("failed to read sum_results \n");
    printf("error: %d\n", error);
    exit(1);
  }

  double var_sum_final = 0;
  for(i = 0; i < numberOfWorkGroup; i++)
  {
    var_sum_final += variance_sum_results[i];
  }

  double variance = var_sum_final/data_size;
  printf("parallel variance: %lf\n", variance);
  double std_dev = sqrt(variance);
  printf("parallel std deviation: %lf\n", std_dev);


  clReleaseMemObject(input);
  clReleaseMemObject(std_dev_output);
  clReleaseProgram(std_dev_program);
  clReleaseProgram(sum_program);
  clReleaseKernel(std_dev_kernel);
  clReleaseCommandQueue(commands);
  clReleaseContext(context);

  return 0;
}
