/*
  Copyright 2009-2022 Lianqi Wang <lianqiw-at-tmt-dot-org>

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

/**
 * \file gpu_math.cu
 * 
 * Wraps cuda routines for CPU data type.
 * */
#include "cublas.h"
#include "utils.h"
#include "gpu_math.h"
#if CUDA_VERSION>10000
/**
 * Compute svd
 * */
void gpu_dsvd(dmat **U_, dmat **S_, dmat **Vt_, const dmat *A_){
  int cuda_dedup_save=cuda_dedup; cuda_dedup=0;
  //NumArray<real, Gpu> U, S, Vt, A;//match cpu precision (double is slow)
  NumArray<Real, Gpu> U, S, Vt, A;//match GPU precision
  stream_t stream;
  cp2gpu(A, A_, stream);
  cusvd(U, S, Vt, A, stream); 
  cp2cpu(U_, U, stream);
  cp2cpu(S_, S, stream);
  //dmat *V_=NULL, *Vt2=NULL;
	//cp2cpu(&V_, V);
  //Vt2=dtrans(V_); dfree(V_);
  cp2cpu(Vt_, Vt, stream);
  //dfree(Vt2);
  cuda_dedup=cuda_dedup_save;
}
/**
 * Invert matrix (pow=-1) or raise power of a matrix with svd.
 * */
void gpu_dsvd_pow(dmat *A_, real pow, real thres){
  if(thres<0){
    error("negative thres is not supported\n");
  }
  int cuda_dedup_save=cuda_dedup; cuda_dedup=0;
  NumArray<Real, Gpu> A;//match GPU precision
  stream_t stream;
  cp2gpu(A, A_, stream);
  cusvd_pow(A, (Real)pow, (Real)thres, stream);
  cp2cpu(&A_, A, stream);
  cuda_dedup=cuda_dedup_save;
}
/**
 * matrix multplication in gpu
 */
void gpu_dgemm(dmat **C_, const real beta, const dmat *A_, const dmat *B_, const char trans[2], const real alpha){
  int cuda_dedup_save=cuda_dedup; cuda_dedup=0;
  NumArray<Real, Gpu>A,B,C;
  stream_t stream;
  cp2gpu(A, A_, stream);
  cp2gpu(B, B_, stream);
  if(*C_) cp2gpu(C, *C_, stream); 
  cugemm(C, (Real)beta, A, B, trans, (Real)alpha, stream);
  cp2cpu(C_,C,stream);
  cuda_dedup=cuda_dedup_save;
}

#endif
