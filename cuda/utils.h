#ifndef AOS_CUDA_UTILS_H
#define AOS_CUDA_UTILS_H
#include <cuda.h>
#include <cublas_v2.h>
#include <cuComplex.h>
#include "cusparse.h"
#include "curmat.h"
#define fcomplex cuFloatComplex
#define dcomplex cuDoubleComplex

#define cudaCallocHostBlock(P,N) ({DO(cudaMallocHost(&(P),N)); DO(cudaMemset(P,0,N)); CUDA_SYNC_DEVICE;})
#define cudaCallocBlock(P,N)     ({DO(cudaMalloc(&(P),N));     DO(cudaMemset(P,0,N)); CUDA_SYNC_DEVICE;})
#define cudaCallocHost(P,N,stream) ({DO(cudaMallocHost(&(P),N)); DO(cudaMemsetAsync(P,0,N,stream));})
#define cudaCalloc(P,N,stream) ({DO(cudaMalloc(&(P),N));DO(cudaMemsetAsync(P,0,N,stream));})
#define TO_IMPLEMENT error("Please implement")

#define DO(A) if((A)!=0) error("(cuda) %d: %s\n", cudaGetLastError(), cudaGetErrorString(cudaGetLastError()));
#define CONCURRENT 0
#if CONCURRENT
#define CUDA_SYNC_STREAM				\
    while(cudaStreamQuery(stream)!=cudaSuccess){	\
	if(THREAD_RUN_ONCE){/* no jobs to do*/		\
	    cudaStreamSynchronize(stream);		\
	    break;					\
	}						\
    }
#else
#define CUDA_SYNC_STREAM cudaStreamSynchronize(stream)
#endif
#define CUDA_SYNC_DEVICE DO(cudaDeviceSynchronize())

#define TIMING 0
#if TIMING == 1
extern int nstream;
#define STREAM_NEW(stream) ({DO(cudaStreamCreate(&stream));info2("nstream=%d\n",lockadd(&nstream,1)+1);})
#define STREAM_DONE(stream) ({DO(cudaStreamSynchronize(stream));DO(cudaStreamDestroy(stream));info2("nstream=%d\n",lockadd(&nstream,-1)-1);})
#else
#define STREAM_NEW(stream) DO(cudaStreamCreate(&stream))
#define STREAM_DONE(stream) DO(cudaStreamDestroy(stream))
#endif
#define adpind(A,i) ((A)->nx>1?(A)->p[i]:(A)->p[0])
#define MYSPARSE 0

/*
  Notice that the CUDA FFT 4.0 is not thread safe!. Our FFT is a walk around of
the problem by using mutex locking to makesure only 1 thread is calling FFT. */
extern pthread_mutex_t cufft_mutex;
#define CUFFT(plan,in,dir) ({CUDA_SYNC_STREAM; LOCK(cufft_mutex); int ans=cufftExecC2C(plan, in, in, dir); cudaStreamSynchronize(0); UNLOCK(cufft_mutex); if(ans) error("cufft failed with %d\n", ans);})


/* Private to accphi.cu. Do not include in maos*/
typedef struct{
    float (*loc)[2];//in device.
    float dx;
    int nloc;
}culoc_t;
/*
  We use a single map_t to contain all layers instead of using an array of map_t
  because we want to use layered texture. This preference can be retired since
  the speed is largely the same with layered texture or flat memory.
 */
typedef struct{
    cudaArray *ca;//3D array. for layered texture
    float **p;//float array.
    float *ht;
    float *vx;
    float *vy;
    float *ox;
    float *oy;
    float *dx;
    float *iac;
    int* cubic;
    int* nx;
    int* ny;
    int nlayer;
}cumap_t;


typedef struct{
    int *p;
    int *i;
    float *x;
    int nx;
    int ny;
    int nzmax;
}cusp;

void gpu_map2dev(cumap_t *dest, map_t **source, int nps, int type);
void gpu_sp2dev(cusp **dest, dsp *src);
void gpu_calc_ptt(double *rmsout, double *coeffout, 
		  const double ipcc, const dmat *imcc,
		  const float (*restrict loc)[2], 
		  const int nloc,
		  const float *restrict phi,
		  const float *restrict amp,
		  cudaStream_t stream
		  );
void gpu_calc_ngsmod(double *pttr_out, double *pttrcoeff_out,
		     double *ngsmod_out, int nmod,
		     double MCC_fcp, double ht, double scale,
		     double thetax, double thetay,
		     const double ipcc, const dmat *imcc,
		     const float (*restrict loc)[2], 
		     const int nloc,
		     const float *restrict phi,
		     const float *restrict amp,
		     cudaStream_t stream);
void gpu_loc2dev(float (* restrict *dest)[2], loc_t *src);
void gpu_dbl2dev(float * restrict *dest, double *src, int n);
void gpu_cmp2dev(fcomplex * restrict *dest, dcomplex *src, int n);
void gpu_dmat2dev(float * restrict *dest, dmat *src);
void gpu_dmat2cu(curmat *restrict *dest, dmat *src);
void gpu_dcell2cu(curcell *restrict *dest, dcell *src);
void gpu_cmat2dev(fcomplex * restrict *dest, cmat *src);
void gpu_dbl2flt(float * restrict *dest, double *src, int n);
void gpu_long2dev(int * restrict *dest, long *src, int n);
void gpu_spint2dev(int * restrict *dest, spint *src, int n);
void gpu_spint2int(int * restrict *dest, spint *src, int n);
void gpu_dev2dbl(double * restrict *dest, float *src, int n, cudaStream_t stream);
#if MYSPARSE
void cuspmul (float *y, cusp *A, float *x, float alpha, cudaStream_t stream);
void cusptmul(float *y, cusp *A, float *x, float alpha, cudaStream_t stream);
#else
void cuspmul (float *y, cusp *A, float *x, float alpha, cusparseHandle_t handle);
void cusptmul(float *y, cusp *A, float *x, float alpha, cusparseHandle_t handle);
#endif
__global__ void fscale_do(float *v, int n, float alpha);
void gpu_writeflt(float *p, int nx, int ny, const char *format, ...);
void gpu_writefcmp(fcomplex *p, int nx, int ny, const char *format, ...);
void gpu_writeint(int *p, int nx, int ny, const char *format, ...);
#endif