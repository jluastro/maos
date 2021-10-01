/*
  Copyright 2009-2021 Lianqi Wang <lianqiw-at-tmt-dot-org>
  
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
#ifndef AOS_CUDA_UTILS_H
#define AOS_CUDA_UTILS_H
#include <typeinfo>
#include "common.h"
#include "types.h"
#include "cudata.h" 
class lock_t{
	pthread_mutex_t& mutex;
	int enable;
public:
	lock_t(pthread_mutex_t& _mutex, int _enable=1):mutex(_mutex), enable(_enable){
		if(enable) LOCK(mutex);
	}
	~lock_t(){
		if(enable) UNLOCK(mutex);
	}
};

extern int cuda_dedup; //Set to 1 during setup and 0 during simulation
/**
   Without type conversion. Enable asynchrous transfer. It is asynchrous only if
   called allocated pinned memory.
*/

template<typename M, typename N>
void type_convert(M* out, const N* in, int nx){
	for(int i=0; i<nx; i++){
		out[i]=static_cast<M>(in[i]);
	}
}
template<>
inline void type_convert<float2, double2>(float2* out, const double2* in, int nx){
	for(int i=0; i<nx; i++){
		out[i].x=static_cast<float>(in[i].x);
		out[i].y=static_cast<float>(in[i].y);
	}
}
template<>
inline void type_convert<double2, float2>(double2* out, const float2* in, int nx){
	for(int i=0; i<nx; i++){
		out[i].x=static_cast<double>(in[i].x);
		out[i].y=static_cast<double>(in[i].y);
	}
}

template<typename M, typename N>
void cp2gpu(M* dest, const N* src, int nx, int ny, cudaStream_t stream=0){
	/*{
		static int same_size=0, size_last=0;
		if(size_last==nx*ny*sizeof(N)){
			same_size++;
			if(same_size==10){
				error("cp2gpu: %d copies to GPU of size %ld KiB\n", same_size, size_last>>10);
			}
		}else{
			same_size=0;
		}
		dbg("cp2gpu: %d copies to GPU of size %ld KiB\n", same_size, size_last>>10);
		size_last=nx*ny*sizeof(N);
	}*/
	M* from=0;
	int free_from=0;
	if(cuglobal->memcount.count(dest)&&cuglobal->memcount[dest]>1){
		error("Should not copy to deduped pointer %p. Count=%d\n", dest, 
			cuglobal->memcount[dest]);
	}
	if(sizeof(M)!=sizeof(N)){
		long memsize=nx*ny*sizeof(M);
		if(memsize>20000000||!cudata){//Too large. Don't cache.
			from=(M*)malloc(memsize);
			free_from=1;
		} else{
			LOCK(cuglobal->memmutex);
			if(cuglobal->nmemcache<memsize){
				cuglobal->nmemcache=memsize;
				/*dbg("GPU%d: Enlarge mem cache to %ld: %p->",
				  current_gpu(), memsize, cuglobal->memcache);*/
				cuglobal->memcache=realloc(cuglobal->memcache, cuglobal->nmemcache);
				//dbg("%p\n", cuglobal->memcache);
			}
			from=(M*)cuglobal->memcache;
		}
		type_convert(from, src, nx*ny);
	} else{
		from=(M*)(src);
	}
	DO(cudaMemcpyAsync(dest, from, sizeof(M)*nx*ny, cudaMemcpyHostToDevice, stream));
	if(free_from){
		if(stream!=0){
			CUDA_SYNC_STREAM;
		}
		free(from);
	} else if(sizeof(M)!=sizeof(N)){
		UNLOCK(cuglobal->memmutex);
	}
}
/*Async copy does not make sense here because malloc pinned memory is too expensive.*/
template<typename M, typename N>
void cp2gpu_dedup(M** dest, const N* src, int nx, int ny, cudaStream_t stream=0){
	if(!src){
		error("src=null\n");
	}
	//dbg("cp2gpu_dedup: copy %p to GPU.\n", src);
	uint64_t key=0;
	int skip_copy=0;
	int record_mem=0;
	if(cuda_dedup&&!*dest){
		key=hashlittle(src, nx*ny*sizeof(N), nx*ny);
		key=hashlittle(&cudata, sizeof(void*), key);//put GPU index as part of fingerprint.
		lock_t tmp(cuglobal->memmutex);
		if(cuglobal->memhash.count(key)){
			*dest=(M*)cuglobal->memhash[key];
			if(cuglobal->memcount[*dest]){//valid memory
				cuglobal->memcount[*dest]++;
				skip_copy=1;//no need to copy again
				//dbg("cp2gpu_dedup: increase reference to data: %p\n", *dest);
			} else{
				cuglobal->memhash.erase(key);
				cuglobal->memcount.erase(*dest);
				warning("cp2gpu_dedup: remove invalid reference data: %p\n", *dest);
				*dest=0;
			}
		}
	} else if(!cuda_dedup&&*dest){
		//Avoid overriding previously referenced memory
		lock_t tmp(cuglobal->memmutex);
		if(cuglobal->memcount.count(*dest)&&cuglobal->memcount[*dest]>1){
			warning("cp2gpu_dedup: deferencing data: %p\n", *dest);
			cuglobal->memcount[*dest]--;
			*dest=0;
		}
	}

	if(!*dest){
		if(cuda_dedup) record_mem=1;
		*dest=(M*)new Gpu<M>[nx*ny];
	}
	if(!skip_copy){
		cp2gpu(*dest, src, nx, ny, stream);
	}
	if(record_mem){
		//dbg("cp2gpu_dedup: record reference to data: %p\n", *dest);
		lock_t tmp(cuglobal->memmutex);
		cuglobal->memhash[key]=*dest;
		cuglobal->memcount[*dest]=1;
	}
}

template<typename M, typename N> static inline void
cp2gpu(Array<M, Gpu>& dest, const N* src, int nx, int ny, cudaStream_t stream=0){
	if(!src||!nx||!ny) return;
	if(dest){
		if(dest.N()!=nx*ny){
			error("Array is %ldx%ld, input is %dx%d\n", dest.Nx(), dest.Ny(), nx, ny);
		}
		cp2gpu(dest(), src, nx, ny, stream);
	} else{
		M* tmp=0;
		cp2gpu_dedup(&tmp, src, nx, ny, stream);
		dest=Array<M, Gpu>(nx, ny, tmp, 1);
	}
}
static inline void cp2gpu_dedup(Real** dest, const dmat* src, cudaStream_t stream=0){
	if(!src) return;
	cp2gpu_dedup(dest, src->p, src->nx, src->ny, stream);
}
#if CPU_SINGLE==0
static inline void cp2gpu(curmat& dest, const dmat* src, cudaStream_t stream=0){
	if(!src) return;
	cp2gpu(dest, src->p, src->nx, src->ny, stream);
}
static inline void cp2gpu(cucmat& dest, const cmat* src, cudaStream_t stream=0){
	if(!src) return;
	cp2gpu(dest, src->p, src->nx, src->ny, stream);
}
#endif
static inline void cp2gpu(curmat& dest, const smat* src, cudaStream_t stream=0){
	if(!src) return;
	cp2gpu(dest, src->p, src->nx, src->ny, stream);
}


static inline void cp2gpu(cucmat& dest, const zmat* src, cudaStream_t stream=0){
	if(!src) return;
	cp2gpu(dest, src->p, src->nx, src->ny, stream);
}

void cp2gpu(cumapcell& dest, const mapcell* source);
void cp2gpu(cusp& dest, const dsp* src, int tocsr);
void cp2gpu(cusp& dest, const dspcell* src, int tocsr);
void cp2gpu(cuspcell& dest, const dspcell* src, int tocsr);
void cp2gpu(curmat& dest, const loc_t* src);
void cp2gpu(curcell& dest, const dcell* src);
void cp2gpu(cuccell& dest, const ccell* src);

void cuspmul(Real* y, const cusp& A, const Real* x, int ncol, char trans,
	Real alpha, stream_t& stream);

void gpu_write(const Real* p, int nx, int ny, const char* format, ...);
void gpu_write(const Comp* p, int nx, int ny, const char* format, ...);
void gpu_write(const int* p, int nx, int ny, const char* format, ...);
void add2cpu(float* restrict* dest,Real alpha, Real* src, Real beta, int n, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(smat** out, float alpha, const curmat& in, float beta, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(zmat** out, float alpha, const cucmat& in, float beta, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(scell** out, float alpha, const curcell& in, float beta, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(zcell **out, float alpha, const cuccell &in, float beta, cudaStream_t stream, pthread_mutex_t *mutex=0);
void add2cpu(dcell **out, real alpha, const curcell &in, real beta, cudaStream_t stream, pthread_mutex_t *mutex=0);
void add2cpu(ccell **out, real alpha, const cuccell &in, real beta, cudaStream_t stream, pthread_mutex_t *mutex=0);
#if CPU_SINGLE==0
void add2cpu(double* restrict* dest, double alpha, Real* src, double beta, int n, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(dmat** out, double alpha, const curmat& in, double beta, cudaStream_t stream, pthread_mutex_t* mutex=0);
void add2cpu(cmat** out, double alpha, const cucmat& in, double beta, cudaStream_t stream, pthread_mutex_t* mutex=0);
void cp2cpu(dmat** out, const curmat& in, cudaStream_t stream=0);
void cp2cpu(cmat** out, const cucmat& in, cudaStream_t stream=0);
#endif

void cp2cpu(smat** out, const curmat& in, cudaStream_t stream=0);
void cp2cpu(zmat** out, const cucmat& in, cudaStream_t stream=0);

void cp2cpu(scell** out, const curcell& in, cudaStream_t stream=0);
void cp2cpu(zcell** out, const cuccell& in, cudaStream_t stream=0);
void cp2cpu(dcell** out, const curcell& in, cudaStream_t stream=0);
void cp2cpu(ccell** out, const cuccell& in, cudaStream_t stream=0);

void zfarr_push_scale(struct zfarr *ca, int i, const curmat &A, Real scale, cudaStream_t stream=0);
void zfarr_push_scale(struct zfarr *ca, int i, const cucmat &A, Real scale, cudaStream_t stream=0);
void zfarr_push_scale(struct zfarr *ca, int i, const curcell &A, Real scale, cudaStream_t stream=0);
void zfarr_push_scale(struct zfarr *ca, int i, const cuccell &A, Real scale, cudaStream_t stream=0);
void drawopdamp_gpu(const char* fig, loc_t* loc, const curmat& opd,  cudaStream_t stream,
	const dmat* amp, real* zlim,
	const char* title, const char* xlabel, const char* ylabel,
	const char* format, ...) CHECK_ARG(10);
void drawpsf_gpu(const char* fig, curmat& psf, int count, cudaStream_t stream, int plotpsf,
	const char* title, const char* xlabel, const char* ylabel,
	const char* format, ...) CHECK_ARG(9);
#endif
