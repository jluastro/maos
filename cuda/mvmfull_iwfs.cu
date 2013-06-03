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
#include <errno.h>
#include "utils.h"
#include "curmat.h"
#include "cucmat.h"
/**
\file mvmfull_iwfs.cu

Test MVM for a single WFS. Using two GPUs, without networking.
This is not part of maos executable. Called by test/test_gpu executable.


two ways: 
1) broadcast gradients to both GPUs with each GPU handle part of actuators. (not tested yet)
2) partition gradients to GPUs with GPU handle all actuators. (selected)

Use schedtool -a 0x1 PID to let the exe only run one CPU 0. This prevents jitter.

Optimal timing (low jitter) achieved with multimv_do:
cassiopeia with GTX 580:nsm=1, mtch_ngrid=90, nover=9 1.1ms
orion with single GTX 590: nsm=1, mtch_ngrid=90, nover=9, 1.3ms
kepler with K20: nsm=2, mtch_ngrid=20, nover=3 1.13ms
cassiopeia with single GTX590
*/

#define TIMING 0

#if TIMING 
unsigned int event_flag=cudaEventDefault;
#else
unsigned int event_flag=cudaEventDisableTiming;
#endif
typedef struct{
    curmat *cumvm;//active mvm control matrix
    curmat *cumvm_next;//inactive mvm control matrix.
    curmat *cumvm1;
    curmat *cumvm2;
    curmat *mtch;
    curmat *pix;//pixels. Each sa has 15x6=90 pixels.
    curmat *grad;
    curmat *act;
    stream_t *stream_p;//pixels
    stream_t *stream_g;//grads
    stream_t *stream_a;//act
    stream_t *stream_mvm;//mvm
    int ism;//index of stream for mvm
    int count;
    int gpu;//Which GPU this data is for
    int istep;//Which time step we are in
    int copy_mvm;//1: need to copy mvm.
    int ic;//the column that we are copying.
    cudaEvent_t *event_p;
    cudaEvent_t *event_g;
    cudaEvent_t event_pall;
    event_t *event_w;
#if TIMING
    cudaEvent_t event0;
    cudaEvent_t *event0_p;
    cudaEvent_t *event0_g;
    cudaEvent_t event0_a;
    cudaEvent_t event_a;
    cudaEvent_t *event0_a2;
    cudaEvent_t *event_a2;
    cudaEvent_t event0_mvm;
    cudaEvent_t event_mvm;
#endif
}GPU_DATA_T;
/*Does matched filter*/
static void __global__ mtch_do(const float *mtch, const float *pix, float *grad, int pixpsa, int nsa){
    extern __shared__ float cum[];//for cumulation and reduction
    float *cumi=cum+threadIdx.y*blockDim.x;//2 padding for easy reduction
    int ig=threadIdx.y+blockDim.y*blockIdx.x;
    const float *mtchi=mtch+ig*pixpsa;
    const float *pixi=pix+ig/2*pixpsa;
    if(ig>nsa*2) return;//over range
    //sum 3 times for 90 pixels.
    cumi[threadIdx.x]=0;
    if(threadIdx.x<30){
	cumi[threadIdx.x]=mtchi[threadIdx.x]*pixi[threadIdx.x]
	    +mtchi[threadIdx.x+30]*pixi[threadIdx.x+30]
	    +mtchi[threadIdx.x+60]*pixi[threadIdx.x+60];
    }
    //reduction
    for(int step=16;step>0;step>>=1){
	if(threadIdx.x<step){
	    cumi[threadIdx.x]+=cumi[threadIdx.x+step];
	}
    }
    if(threadIdx.x==0){
	grad[ig]=cumi[0];
    }
}
/*
__global__ static void 
multimv_do(const float *restrict mvm, ATYPE *restrict a, const GTYPE *restrict g, int nact, int ng){
    extern __shared__ float acc[];
    int iact=threadIdx.x+blockIdx.x*blockDim.x;
    int nset=(blockDim.x*gridDim.x+nact-1)/nact;
    if(blockDim.x*gridDim.x<nset*nact){
	//drop partial set
	nset--;
    }
    const int iset=iact/nact;
    if(iset>=nset) return;
    iact=iact-nact*iset;
    acc[threadIdx.x]=0;
    const int igi=(iset*ng)/nset;
    const int ngi=((iset+1)*ng)/nset;
    for(int ig=igi; ig<ngi; ig++){
	register float mvmi=mvm[nact*ig+iact];
	acc[threadIdx.x]+=mvmi*(float)(g[ig]);
    }
    atomicAdd(&a[iact], (ATYPE)acc[threadIdx.x]);
    }*/

/*__global__ static void mvm_g_mul_do(const float *restrict mvm, ATYPE *restrict a, const GTYPE *restrict g, int nact, int ng){
    extern __shared__ float acc[];
    int iact=threadIdx.x+blockIdx.x*blockDim.x;
    if(iact<nact){
	acc[threadIdx.x]=0;
	for(int ig=0; ig<ng; ig++){
	    register float mvmi=mvm[nact*ig+iact];
	    acc[threadIdx.x]+=mvmi*(float)(g[ig]);
	}
	a[iact]+=(ATYPE)acc[threadIdx.x];
    }
    }*/

/**
   A standalone routine that testes applying MVM for a single WFS and update mvm.
   The orderig of gradients are like xyxyxy instead of normal xxxyyy.

   Important: 
   1) Only page locked host memory can do async memcpy that overallps with computation
   2) Has to be Portable for multiple GPUs to do async memcpy concurrently.
   
*/
void mvmfull_iwfs(int *gpus, int ngpu, int nstep){
    int nsm=2;
    info("Using %d gpus. nstep=%d\n", ngpu, nstep);
    int nstep0=20;//for warm up
#if 1
    //const int nact=7673;//total
    const int nact=6981;//active
    const int nsa=2895;//total. all subaps transported to GPU.
#else
    const int nact=6981;//active
    const int nsa=2700;//active
#endif
    int ng=nsa*2;
    const int pixpsa=90;//Change this need to change kernel mtch_do
    const int pixpsa2=71;//average number of pixels, used for 10GbE
    smat *mvm1=snew(nact, ng);
    smat *mvm2=snew(nact, ng);
    smat *pix1=snew(pixpsa, nsa);
    smat *pix2=snew(pixpsa, nsa);
    smat *mtch=snew(pixpsa, ng);
    rand_t srand;
    seed_rand(&srand, 1);
    srandu(mvm1,1, &srand);
    srandu(mvm2,1,&srand);
    srandu(pix1,50, &srand);
    srandu(pix2,50, &srand);
    smat *mvm=mvm1;
    smat *pix=pix2;
    scell *dmres=scellnew(ngpu, 1);
    spagelock(pix1, pix2, mvm1, mvm2, mtch, dmres, NULL);

    int port=20000;
    int sock=-1;
    int ready=1;

    int mtch_ngrid=50;//30;//can change to utilize GPU fully. 16 is good for cassiopeia
    const int mtch_dimx=32;//must launch 32 threads so that they belong to single wrap.
    const int mtch_dimy=12;//4 subapertures, 8 gradients


    {
	char *MVM_NSM=getenv("MVM_NSM");
	if(MVM_NSM){
	    nsm=strtol(MVM_NSM, NULL, 10);
	    info2("nsm is set to %d\n", nsm);
	}
	char *MVM_NGRID=getenv("MVM_NGRID");
	if(MVM_NGRID){
	    mtch_ngrid=strtol(MVM_NGRID, NULL, 10);
	    info2("mtch_ngrid is set to %d\n", mtch_ngrid);
	}
    }
    const int sastep=mtch_dimy*mtch_ngrid/2;
    {
	char *MVM_CLIENT=getenv("MVM_CLIENT");
	if(MVM_CLIENT){
	    char *MVM_PORT=getenv("MVM_PORT");
	    if(MVM_PORT){
		port=strtol(MVM_PORT, NULL, 10);
	    }
	    info2("Connecting to server %s\n", MVM_CLIENT);
	    sock=connect_port(MVM_CLIENT, port, 0 ,1);
	    if(sock!=-1) {
		info2("Connected");
		int cmd[6];
		cmd[0]=nact;
		cmd[1]=nsa;
		cmd[2]=sastep;
		cmd[3]=pixpsa2;
		cmd[4]=nstep;
		cmd[5]=nstep0;
		if(stwriteintarr(sock, cmd, 6)){
		    close(sock); sock=-1;
		    warning("Failed: %s\n", strerror(errno));
		}
	    } else {
		info2("Failed\n");
	    }
	}
    }


    int nc=10;//each time copy nc column of mvm.
    GPU_DATA_T *data=(GPU_DATA_T*)calloc(ngpu, sizeof(GPU_DATA_T));
    const int sect_gpu=(nsa+sastep*ngpu-1)/(sastep*ngpu);
    for(int igpu=0; igpu<ngpu; igpu++){
	cudaSetDevice(gpus[igpu]);
	data[igpu].cumvm1=curnew(mvm1->nx, ng);
	data[igpu].cumvm2=curnew(mvm2->nx, ng);
	data[igpu].cumvm=data[igpu].cumvm1;
	data[igpu].cumvm_next=data[igpu].cumvm2;
	cp2gpu(&data[igpu].cumvm1, mvm);
	data[igpu].pix=curnew(pixpsa, nsa);
	data[igpu].mtch=curnew(pixpsa, nsa*2);
	cp2gpu(&data[igpu].mtch, mtch);
	data[igpu].grad=curnew(ng, 1);
	data[igpu].act=curnew(mvm1->nx, 1);
	data[igpu].stream_p=new stream_t;
	data[igpu].stream_g=new stream_t;
	data[igpu].stream_a=new stream_t[nsm];
	data[igpu].event_w=new event_t[nsm];
	data[igpu].stream_mvm=new stream_t;
	data[igpu].gpu=gpus[igpu];
#if TIMING
	cudaEventCreateWithFlags(&data[igpu].event0, event_flag);
	data[igpu].event0_g=new cudaEvent_t[sect_gpu];
	data[igpu].event0_p=new cudaEvent_t[sect_gpu];
	data[igpu].event0_a2=new cudaEvent_t[sect_gpu];
	data[igpu].event_a2=new cudaEvent_t[sect_gpu];

	for(int i=0; i<sect_gpu; i++){
	    cudaEventCreateWithFlags(&data[igpu].event0_g[i],event_flag);
	    cudaEventCreateWithFlags(&data[igpu].event0_p[i],event_flag);
	    cudaEventCreateWithFlags(&data[igpu].event0_a2[i],event_flag);
	    cudaEventCreateWithFlags(&data[igpu].event_a2[i],event_flag);
	}
	cudaEventCreateWithFlags(&data[igpu].event0_mvm,event_flag);
	cudaEventCreateWithFlags(&data[igpu].event_mvm,event_flag);
	cudaEventCreateWithFlags(&data[igpu].event0_a,event_flag);
	cudaEventCreateWithFlags(&data[igpu].event_a,event_flag);
#endif
	data[igpu].event_g=new cudaEvent_t[sect_gpu];
	data[igpu].event_p=new cudaEvent_t[sect_gpu];
	for(int i=0; i<sect_gpu; i++){
	    cudaEventCreateWithFlags(&data[igpu].event_g[i],event_flag);
	    cudaEventCreateWithFlags(&data[igpu].event_p[i],event_flag);
	}
	cudaEventCreateWithFlags(&data[igpu].event_pall,event_flag);
	dmres->p[igpu]=snew(nact, 1);
	spagelock(dmres->p[igpu], NULL);
	/*
	DO(cudaMemcpyAsync(data[igpu].pix->p, pix->p, 2*nsa*pixpsa,
			   cudaMemcpyHostToDevice, *data[igpu].stream_p));
	cudaMemcpyAsync(dmres->p[igpu]->p, data[igpu].act->p, nact*sizeof(float), 
			cudaMemcpyDeviceToHost, data[igpu].stream_a[0]);
	CUDA_SYNC_DEVICE;
	*/
    }
    smat *timing=snew(nstep, 1);
    smat *timing_mvmu=snew(nstep, 1);
    smat *timing_sock=snew(nstep, 1);
    smat *result=snew(nstep, 1);
    cudaProfilerStart();
    TIC;
    if(sock!=-1 && stwriteint(sock, ready)){
	warning("error send ready signal: %s\n", strerror(errno));
	close(sock); sock=-1;
    }
    info2("Ready\n");
    for(int jstep=-nstep0; jstep<nstep; jstep++){
	//run 20 frames to warm up before timing.
	int istep=jstep<0?0:jstep;
	if(sock!=-1){//start signal
	    timing_sock->p[istep]=0;
	}
	tic;
#if TIMING
	if(istep%8000==0)
#else
	    if(0)
#endif
	    {//need to update MVM
		if(mvm==mvm1){//switch mvm on host.
		    mvm=mvm2;
		}else{
		    mvm=mvm1;
		}
		for(int igpu=0; igpu<ngpu; igpu++){
		    data[igpu].copy_mvm=1;
		    if(data[igpu].ic!=0){
			warning("Sync error, skip update request at step %d\n", istep);
		    }
		}
	    }
	for(int igpu=0; igpu<ngpu; igpu++){
	    data[igpu].ism=-1;
	    data[igpu].count=0;
	    data[igpu].istep=istep;
#if TIMING
	    //beginning of each GPU operation.
	    DO(cudaEventRecord(data[igpu].event0, data[igpu].stream_a[0]));
#endif
	}
	if(sock==-1){
	    if(pix==pix1){
		pix=pix2;
	    }else{
		pix=pix1;
	    }
	}
	for(int isa=0, igpu=0; isa<nsa; isa+=sastep, igpu=((igpu+1)%ngpu)){
	    cudaSetDevice(gpus[igpu]); 
	    GPU_DATA_T *datai=&data[igpu];
	    int nleft=(nsa-isa)<sastep?(nsa-isa):sastep;
	    //One stream handling the memcpy
#if TIMING
	    DO(cudaEventRecord(datai->event0_p[datai->count], datai->stream_p[0]));
#endif
	    void *pcur=pix->p+isa*pixpsa;
	    if(sock!=-1){
		//pcur=pix->p;//temporary. always use the same buffer
		//manually use 2 byte.
		double tmp0=myclockd();
		if(stread(sock, pcur, 2*nleft*pixpsa2)){
		    warning("failed: %s\n", strerror(errno));
		    close(sock); sock=-1;
		}
		timing_sock->p[istep]+=myclockd()-tmp0;
	    }
	    DO(cudaMemcpyAsync(datai->pix->p+isa*pixpsa, pcur, 2*nleft*pixpsa,
			       cudaMemcpyHostToDevice, *datai->stream_p));
	    //Recored the event when the memcpy is finished
	    DO(cudaEventRecord(datai->event_p[datai->count], datai->stream_p[0]));
	    //Start matched filter when pixel transfer is done.
	    DO(cudaStreamWaitEvent(datai->stream_g[0], datai->event_p[datai->count], 0));
#if TIMING
	    DO(cudaEventRecord(datai->event0_g[datai->count], datai->stream_g[0]));    
#endif
	    mtch_do<<<mtch_ngrid, dim3(mtch_dimx, mtch_dimy), 
		mtch_dimx*mtch_dimy*sizeof(float), datai->stream_g[0]>>>
	       (datai->mtch->p+isa*2*pixpsa, datai->pix->p+isa*pixpsa, 
		datai->grad->p+isa*2, pixpsa, nleft);
	    //Record the event when matched filter is done
	    DO(cudaEventRecord(datai->event_g[datai->count], datai->stream_g[0]));

	    //Another stream does the matrix vector multiplication. Wait for the event before executing.
	    //The stream stream will wait only for the completion of the most recent host call to cudaEventRecord() on event
	    datai->ism=(datai->ism+1)%nsm;
	    
	    cudaStreamWaitEvent(datai->stream_a[datai->ism], datai->event_g[datai->count], 0);
#if TIMING
	    DO(cudaEventRecord(datai->event0_a2[datai->count], datai->stream_a[datai->ism]));    
#endif
#if 0
	    DO(cublasSgemv(datai->stream_a[datai->ism], CUBLAS_OP_N, nact, nleft*2, &one, datai->cumvm->p+nact*isa*2, nact, datai->grad->p+isa*2, 1, &one, datai->act->p, 1));
#else
	    {
		const int naeach=128;
		int nover=1+istep/50;//14;
		char *MVM_NOVER=getenv("MVM_NOVER");
		if(MVM_NOVER){
		    nover=strtol(MVM_NOVER, NULL, 10);
		}
		const int nblock=(nact*nover+naeach-1)/naeach;
		multimv_do<<<nblock, naeach, sizeof(float)*naeach, datai->stream_a[datai->ism]>>>
		    (datai->cumvm->p+nact*isa*2, datai->act->p, datai->grad->p+isa*2, 
		     nact, nleft*2);
	    }
#endif
	    DO(cudaEventRecord(datai->event_w[datai->ism], datai->stream_a[datai->ism]));
#if TIMING
	    DO(cudaEventRecord(datai->event_a2[datai->count], datai->stream_a[datai->ism])); 
#endif
	    datai->count++;
	}
	for(int igpu=0; igpu<ngpu; igpu++){
	    GPU_DATA_T *datai=&data[igpu];
	    //Record an event when pixel tranporting is over. So we can start transporting mvm matrix.
	    DO(cudaEventRecord(datai->event_pall, datai->stream_p[0]));
	}
	//Queue copying MVM matrix to second slot.
	for(int igpu=0; igpu<ngpu; igpu++){
	    GPU_DATA_T *datai=&data[igpu];
	    if(datai->copy_mvm){
		int done=0, nleft;
		if(mvm->ny-datai->ic < nc){
		    done=1;
		    nleft=mvm->ny-datai->ic;
		}else{
		    nleft=nc;
		}
		//wait for mvm application to finish before copying.
		//DO(cudaStreamWaitEvent(datai->stream_mvm[0], datai->event_pall, 0));
#if TIMING
		DO(cudaEventRecord(datai->event0_mvm, datai->stream_mvm[0]));	
#endif
		DO(cudaMemcpyAsync(datai->cumvm_next->p+datai->ic*mvm->nx, 
				   mvm->p+datai->ic*mvm->nx, sizeof(float)*mvm->nx*nleft, 
				   cudaMemcpyHostToDevice, datai->stream_mvm[0]));
#if TIMING
		DO(cudaEventRecord(datai->event_mvm, datai->stream_mvm[0]));
#endif
		datai->ic+=nleft;
		if(done){
		    datai->ic=0;
		    datai->copy_mvm=0;
		    curmat *tmp=datai->cumvm;
		    datai->cumvm=datai->cumvm_next;
		    datai->cumvm_next=tmp;
		    info2("gpu %d switched over at step %d\n", datai->gpu, datai->istep);
		}
	    }
	}
	//Copy DM commands back to CPU
	for(int igpu=0; igpu<ngpu; igpu++){
	    GPU_DATA_T *datai=&data[igpu];
	    cudaSetDevice(gpus[igpu]); 
	    for(int ism=1; ism<nsm; ism++){
		DO(cudaStreamWaitEvent(datai->stream_a[0], datai->event_w[ism], 0));
	    }
#if TIMING
	    DO(cudaEventRecord(datai->event0_a, datai->stream_a[0]));
#endif
	    cudaMemcpyAsync(dmres->p[igpu]->p, datai->act->p, nact*sizeof(float), cudaMemcpyDeviceToHost, datai->stream_a[0]);
#if TIMING
	    DO(cudaEventRecord(datai->event_a, datai->stream_a[0]));//record event when all act are copied so mvm can start.
#endif
	}
	//CPU sums them together. sync first gpu
	data[0].stream_a[0].sync();
	//sum other GPUs
	for(int igpu=1; igpu<ngpu; igpu++){
	    cudaSetDevice(gpus[igpu]); 
	    data[igpu].stream_a[0].sync();
	    for(int iact=0; iact<nact; iact++){
		dmres->p[0]->p[iact]+=dmres->p[igpu]->p[iact];
	    }
	}
	if(sock!=-1){
	    double tmp0=myclockd();
	    if(stwrite(sock, dmres->p[0]->p, sizeof(float)*nact)){
		warning("error write dmres: %s\n", strerror(errno));
		close(sock); sock=-1;
	    }
	    if(streadint(sock, &ready)){//acknowledgement.
		warning("error read ack failed: %s\n", strerror(errno));
		close(sock), sock=-1;
	    }
	    timing_sock->p[istep]+=myclockd()-tmp0;
	    timing->p[istep]=ready*1.e-6;
	}else{
	    timing->p[istep]=toc3;//do not tic.
	}
	result->p[istep]=dmres->p[0]->p[nact/2];
	usleep(50);//yield
	/*info2("\rStep %d takes %.0f us", istep, timing->p[istep]*1e6);
	if(timing->p[istep]>1.25e-3){
	    info2("\n");
	}	
	*/
	//Wait for MVM matrix copy to finish and time.
	for(int igpu=0; igpu<ngpu; igpu++){
	    GPU_DATA_T *datai=&data[igpu];
	    cudaSetDevice(datai->gpu);
	    cudaMemsetAsync(datai->act->p, 0, nact*sizeof(float), datai->stream_a[datai->ism]);
	    datai->stream_a[datai->ism].sync();
	    datai->stream_mvm->sync();
	}
	timing_mvmu->p[istep]=toc3;
#if TIMING 
	if(istep<100){
	    for(int igpu=0; igpu<ngpu; igpu++){
		cudaSetDevice(gpus[igpu]); 
		GPU_DATA_T *datai=data+igpu;
		const int count=datai->count;
		smat *tim=snew(count*6+4,2);
		PSMAT(tim,ptim);
		int ic;
		for(ic=0; ic<count; ic++){
		    cudaEventElapsedTime(&ptim[0][ic*6+0], datai->event0, datai->event0_p[ic]);//start of mtch
		    cudaEventElapsedTime(&ptim[0][ic*6+1], datai->event0, datai->event_p[ic]);//end of mtch
		    cudaEventElapsedTime(&ptim[0][ic*6+2], datai->event0, datai->event0_g[ic]);//start of g
		    cudaEventElapsedTime(&ptim[0][ic*6+3], datai->event0, datai->event_g[ic]);//end of g
		    cudaEventElapsedTime(&ptim[0][ic*6+4], datai->event0, datai->event0_a2[ic]);//start of mvm
		    cudaEventElapsedTime(&ptim[0][ic*6+5], datai->event0, datai->event_a2[ic]);//end of mvm
		    ptim[1][ic*6]=1;
		    ptim[1][ic*6+1]=1;
		    ptim[1][ic*6+2]=2;
		    ptim[1][ic*6+3]=2;
		    ptim[1][ic*6+4]=3;
		    ptim[1][ic*6+5]=3;
		}
		cudaEventElapsedTime(&ptim[0][ic*6+0], datai->event0, datai->event0_a);//start of a copy
		cudaEventElapsedTime(&ptim[0][ic*6+1], datai->event0, datai->event_a);//end of a copy
		cudaEventElapsedTime(&ptim[0][ic*6+2], datai->event0, datai->event0_mvm);//start of mvm copy
		cudaEventElapsedTime(&ptim[0][ic*6+3], datai->event0, datai->event_mvm);//end of mvm copy
		ptim[1][ic*6+0]=4;
		ptim[1][ic*6+1]=4;
		ptim[1][ic*6+2]=5;
		ptim[1][ic*6+3]=5;
		swrite(tim, "timing2_%dgpu%d_step%d", ngpu, igpu, istep);
		sfree(tim);
	    }
	}
#endif
    }
    cudaProfilerStop();
    //swrite(dmres->p[0], "dmres");

    swrite(timing, "timing_%s_%dgpu", myhostname(), ngpu);
    swrite(timing_mvmu, "timing_mvmu_%s_%dgpu", myhostname(), ngpu);
    swrite(timing_sock, "timing_sock_%s_%dgpu", myhostname(), ngpu);
    spageunlock(pix1, pix2, mvm1, mvm2, NULL);
    
    sfree(mvm1);
    sfree(mvm2);
    sfree(pix1);
    sfree(pix2);
    sfree(mtch);
    scellfree(dmres);
    sfree(timing);
    sfree(timing_mvmu);
    sfree(timing_sock);
    sfree(result);
    for(int igpu=0; igpu<ngpu; igpu++){
	cudaSetDevice(gpus[igpu]);
	delete data[igpu].cumvm1;
	delete data[igpu].cumvm2;
	delete data[igpu].pix;
	delete data[igpu].mtch;
	delete data[igpu].grad;
	delete data[igpu].act;
	delete data[igpu].stream_p;
	delete data[igpu].stream_g;
	delete[] data[igpu].stream_a;
	delete[] data[igpu].event_w;
	delete data[igpu].stream_mvm;
	delete[] data[igpu].event_g;
	delete[] data[igpu].event_p;
	cudaDeviceReset();
    }
    free(data);
  
}