/*
  Copyright 2009-2013 Lianqi Wang <lianqiw@gmail.com> <lianqiw@tmt.org>
  
  This file is part of Multithreaded Adaptive Optics Simulator (MAOS).

  MAOS is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  MAOS is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  MAOS.  If not, see <http://www.gnu.org/licenses/>.
*/
extern "C"
{
#include <cuda.h>
#include "gpu.h"
}
#include "utils.h"
#include "curmat.h"
#include "pcg.h"

#include "recon.h" /*for  debugging */

#define PRINT_RES 0

/* dest = a/b Do not use restric because dest and b maybe the same*/
__global__ static void div_do(float *dest, const float * a, const float *b){
    dest[0]=a[0]/b[0];
    if(!isfinite(dest[0])) dest[0]=0;
}
/* dest = a/b;then b=a*/
__global__ static void div_assign_do(float * dest, const float * a, float * b){
    dest[0]=a[0]/b[0];
    b[0]=a[0];
    if (!isfinite(dest[0])) dest[0]=0;
}


/**
   res points to a scalar in device memory. **The caller has to zero it**
*/
void curcellinn_add(float *restrict res, const curcell *A, const curcell *B, cudaStream_t stream){
    //cudaMemsetAsync(res, 0,sizeof(float), stream);
    if(A->m && B->m){
	const int n=A->m->nx*A->m->ny;
	inn_wrap(res, A->m->p, B->m->p, n, stream);
    }else{
	for(int i=0; i<A->nx*A->ny; i++){
	    const curmat *a=A->p[i];
	    const curmat *b=B->p[i];
	    const int n=a->nx*a->ny;
	    inn_wrap(res,a->p,b->p,n,stream);
	}
    }
}

/* dest = sqrt(a/b); */
__global__ static void div_sqrt_do(float * dest, const float * a, const float * b){
    dest[0]=sqrt(a[0]/b[0]);
}
/**Only use kernels to avoid synchronization*/
static inline void pcg_residual(float *r0r0, float *rkzk, float *rr0, curcell *r0, 
				int precond, cudaStream_t stream){
    if(precond){
	/*r0r0=r0'*r0; */
	curcellinn_add(r0r0, r0, r0, stream);
	/*r0r0=sqrt(r0r0/rr0); */
	div_sqrt_do<<<1,1,0,stream>>>(r0r0, r0r0, rr0);
    }else{
	/*r0r0=sqrt(rkzk/rr0); */
	div_sqrt_do<<<1,1,0,stream>>>(r0r0, rkzk, rr0);
    }
}
#define DEBUG_OPDR 1
#define TIMING 0
/**
   The PCG algorithm. Copy of lib/pcg.c, but replacing dcell with curcell.
   Timing: 
   curcellinn implemented with blas takes 0.457 ms each call.
   curcellinn implemented with kernel takes 0.193 ms each call with 256x32 split. 0.158 with 128x16 split. 0.137 with 64x64 split.
   curcelladd takes 0.048 ms per call.
   return non-zero during unconvergence.

   TODO: With Compute Capability of 4.0, all operations can be done in one big kernel, which launches other kernels.
 */
float gpu_pcg(curcell **px, 
	      G_CGFUN Amul, const void *A, 
	      G_PREFUN Mmul, const void *M, 
	      const curcell *b, CGTMP_T *cg_data, int warm, int maxiter,
	      stream_t &stream, double cgthres){
#if TIMING 
#define NEVENT 16
#define RECORD(i) DO(cudaEventRecord(event[i], stream))
    static cudaEvent_t event[NEVENT]={0};
    float times[NEVENT];
    if(!event[0]){
	for(int i=0; i<NEVENT; i++){
	    DO(cudaEventCreate(&event[i]));
	}
    }
#else
#define RECORD(n)
#endif
    RECORD(0);
    curcell *&r0=cg_data->r0;
    curcell *&z0=cg_data->z0;/*Is reference or preconditioned value. */
    curcell *&p0=cg_data->p0;
    curcell *&Ap=cg_data->Ap;
    float residual=-1;/*Only useful if PRINT_RES is set*/
    int ntot=maxiter*2+3;
    if(!cg_data->store){
	DO(cudaMalloc(&cg_data->store, ntot*sizeof(float)));
    }
    float *store=cg_data->store;
    DO(cudaMemsetAsync(store, 0, ntot*sizeof(float),stream));
    float *rr0=store; store++;
    float *rkzk=store; store++;
    float *bk  =store; store++;
    float *rmzm=store; store+=maxiter;
    float *ak  =store; store+=maxiter;
    curcellinn_add(rr0, b, b, stream);//rr0=b*b; initial residual norm
    float *diff;
    cudaMallocHost(&diff, sizeof(float)*(maxiter+1));//Only this enables async transfer
    memset(diff, 0, sizeof(float)*(maxiter+1));
#if PRINT_RES == 2
    fprintf(stderr, "GPU %sCG %d:",  Mmul?"P":"", maxiter);
#endif
    /*computes r0=b-A*x0 */
    if(!*px){
	*px=curcellnew(b);
    }else if(!warm){
	curcellzero(*px, stream);
    }
    curcell *x0=*px;
#if DEBUG_OPDR == 1  && 0
    curcell *x0save=curcellnew(x0);
#endif
    int kover=0; //k overflows maxit
    for(int k=0; ; k++){
	if(k==maxiter){
	    k=0;//reset 
	    kover++;
	    memset(diff, 0, sizeof(float)*(maxiter+1));
	    DO(cudaMemsetAsync(rr0+1, 0, (ntot-1)*sizeof(float),stream));
	}
	if(k%500==0){/*initial or re-start every 500 steps*/
	    RECORD(1);
	    /*computes r0=b-A*x0 */
	    curcellcp(&r0, b, stream);/*r0=b; */
	    RECORD(2);
	    Amul(&r0, 1, A, x0, -1, stream);/*r0=r0+(-1)*A*x0 */ 
	    RECORD(3);
	    if(Mmul){/*z0=M*r0*/
		Mmul(&z0,M,r0,stream);
	    }else{
		z0=r0;
	    }
	    RECORD(4);
	    curcellcp(&p0, z0, stream);
	    RECORD(5);
	    if(k!=0){
		cudaMemsetAsync(rkzk, 0, sizeof(float), stream);
	    }
	    curcellinn_add(rkzk, r0, z0, stream);//9
	    RECORD(6);
#if TIMING 
	    CUDA_SYNC_STREAM;
	    for(int i=1; i<7; i++){
		DO(cudaEventElapsedTime(&times[i], event[i-1], event[i]));
		times[i]*=1e3;
	    }
	    info2("CG %d k=0 Prep %3.0f CP %3.0f Amul %3.0f Mmul %3.0f cp %3.0f inn %3.0f \n", 
		  maxiter, times[1], times[2], times[3], times[4], times[5], times[6]);
#endif
	}
	if (PRINT_RES){
	    pcg_residual(&diff[k], rkzk, rr0, r0, Mmul?1:0, stream);
	}
	RECORD(7);
#if PRINT_RES == 2
	info2("%.5f ", diff[k]);
#endif	
	/*Ap=A*p0*/
	Amul(&Ap, 0, A, p0, 1, stream); RECORD(8);
	/*ak[k]=rkzk/(p0'*Ap); */
	curcellinn_add(ak+k, p0, Ap, stream);	RECORD(9);
	div_do<<<1,1,0,stream>>>(ak+k, rkzk, ak+k);
	/*x0=x0+ak[k]*p0 */
#if DEBUG_OPDR == 1 && 0
	curcellcp(&x0save, x0, stream);
#endif
	curcelladd(&x0, p0, ak+k, 1, stream);
	RECORD(10);
	/*Stop CG when 1)max iterations reached or 2)residual is below cgthres (>0), which ever is higher.*/
	if((kover || k+1==maxiter) && (cgthres<=0 || diff[k]<cgthres) ||kover>=3){
#if DEBUG_OPDR == 1 //for debugging a recent error. compute the residual for the last step
	    curcelladd(&r0, Ap, ak+k, -1, stream);
	    pcg_residual(&diff[k+1], NULL, rr0, r0, 1, stream);
	    if(maxiter==30){
		static int counter=0;
		warning_once("Remove after debugging\n");
		float opdrmax=curcellmax(x0, stream);
		if(opdrmax>6e-6){
		    //CUDA_SYNC_STREAM;
		    curcellwrite(x0, "pcg_x0_%d", counter);
		    //curcellwrite(x0save, "pcg_x0save_%d", counter);
		    //curcelladd(&x0save, p0, ak+k, 1, stream);
		    //curcellwrite(x0save, "pcg_x0saveadd_%d", counter);
		    gpu_write(ak, maxiter, 1, "pcg_ak_%d", counter);
		    curcellwrite(Ap, "pcg_Ap_%d", counter);
		    curcellwrite(p0, "pcg_p0_%d", counter);
		    curcellwrite(r0, "pcg_r0_%d", counter);
		    float alpha;
		    cudaMemcpy(&alpha, ak+k, sizeof(float), cudaMemcpyDeviceToHost);
		    info("alpha=%g\n", alpha);
		    counter++;
		}
	    }
#endif
#if TIMING 
	    CUDA_SYNC_STREAM;
	    for(int i=7; i<11; i++){
		DO(cudaEventElapsedTime(&times[i], event[i-1], event[i]));
		times[i]*=1e3;
	    }
	    info2("CG %d k=%d Amul %3.0f inn %3.0f add %3.0f add %3.0f\n",
		  maxiter, k, times[8], times[9], times[10], times[11]);
#endif
	    break;
	}
	/*r0=r0-ak[k]*Ap */
	curcelladd(&r0, Ap, ak+k, -1, stream);
	RECORD(11);
	/*preconditioner */
	if(Mmul) {/*z0=M*r0*/
	    Mmul(&z0,M,r0, stream);
	}
	RECORD(12);
	/*rmzm[k]=r0'*z0 for next step*/
	curcellinn_add(rmzm+k, r0, z0, stream);
	RECORD(13);
	/*bk=rmzm/rkzk; rkzk=rmzm*/
	div_assign_do<<<1,1,0,stream>>>(bk, rmzm+k, rkzk);
	/*p0=bk*p0+z0 */
	curcelladd(&p0, bk, z0, stream);
	RECORD(14);
#if TIMING 
	CUDA_SYNC_STREAM;
	for(int i=7; i<15; i++){
	    DO(cudaEventElapsedTime(&times[i], event[i-1], event[i]));
	    times[i]*=1e3;
	}
	info2("CG %d k=%d Amul %3.0f inn %3.0f add %3.0f add %3.0f Mmul %3.0f inn %3.0f add %3.0f\n",
	      maxiter, k, times[8], times[9], times[10], times[11], times[12], times[13], times[14]);
#endif
    }
    RECORD(15);
    /* Instead of check in the middle, we only copy the last result. Improves performance by 20 nm !!!*/
    CUDA_SYNC_STREAM;
#if PRINT_RES == 1
    fprintf(stderr, "GPU %sCG %2d: %.5f ==> %.5f\n", Mmul?"P":"",maxiter, diff[0], diff[maxiter-1]);
#elif PRINT_RES==2
    fprintf(stderr, "\n");
#endif
#if DEBUG_OPDR
    residual=diff[maxiter];
#else
    residual=diff[maxiter-1];
#endif
#if TIMING 
    DO(cudaEventElapsedTime(&times[15], event[0], event[15]));
    times[15]*=1e3;
    info2("CG %d total %4.0f\n", maxiter, times[15]);
#endif
    cudaFreeHost(diff);
#if DEBUG_OPDR == 1 && 0
    delete x0save;
#endif
    return residual;
}
