/*
  Copyright 2009, 2010, 2011 Lianqi Wang <lianqiw@gmail.com> <lianqiw@tmt.org>
  
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
\file genotf.c
   Routines to generate short exposure OTFs of an aperture in present of
   atmosphere turbulence.
*/
#include "common.h"
#include "thread.h"
#include "daemonize.h"
#include "dmat.h"
#include "cmat.h"
#include "mathmisc.h"
#include "loc.h"
#include "genotf.h"
#include "sys/process.h"
/**
private data struct to mark valid pairs of points.  */
typedef struct T_VALID{
    long n;
    long (*loc)[2];
}T_VALID;
/**
 Wrap the data to genotf to have multi-thread capability.*/
typedef struct GENOTF_T{
    long isa;
#if USE_PTHREAD > 0
    pthread_mutex_t mutex_isa;
#endif
    cmat **otf;
    loc_t *loc;     /**<the common aperture grid*/
    const double *amp;    /**<The amplitude map of all the (sub)apertures*/
    const double *opdbias;/**<The static OPD bias. */
    const double *area;   /**<area of a (sub)aperture*/
    double thres;/**<threshold to consider an (sub)aperture as full*/
    double wvl;  /**<The wavelength. only needef if opdbias is not null*/
    long ncompx; /**<Size of OTF*/
    long ncompy; /**<Size of OTF*/
    long nsa;    /**<Number of (sub)apertures*/
    long pttr;   /**<Remove piston/tip/tilt*/
    const dmat *B;
    const T_VALID *pval;
    long isafull;
    const cmat *otffull;
}GENOTF_T;
/**
   Remove tip/tilt from the covariance matrix.
*/
static dmat* pttr_B(const dmat *B0,   /**<The B matrix. */
		    loc_t *loc,       /**<The aperture grid*/
		    const double *amp /**<The amplitude map*/
		   ){
    double *locx=loc->locx;
    double *locy=loc->locy;
    int nloc=loc->nloc;

    dmat *B2=dnew(nloc, nloc);
    PDMAT(B2, BP);
    PDMAT(B0, B);
  
    double *mod[3];
    dmat *mcc=dnew(3,3);//modal cross coupling matrix.
    PDMAT(mcc, cc);
 
    mod[0]=NULL;
    mod[1]=locx;
    mod[2]=locy;
    for(int im=0; im<3;im++){
	for(int jm=im;jm<3;jm++){
	    cc[jm][im]=dotdbl(mod[im], mod[jm], amp, nloc);
	    if(im!=jm)
		cc[im][jm]=cc[jm][im];
	}
    }
    dinvspd_inplace(mcc);
    dmat *M   =dnew(nloc, 3);//The tip/tilt modal matrix
    dmat *MW  =dnew(nloc, 3);//M*W
    dmat *MCC =dnew(3, nloc);//M*inv(M'*W*M)
    dmat *Mtmp=dnew(3, nloc);//B'*MW;
 
    for(long iloc=0; iloc<nloc; iloc++){
	M->p[iloc]=1;
    }
    memcpy(M->p+nloc, locx, nloc*sizeof(double));
    memcpy(M->p+nloc*2, locy, nloc*sizeof(double));
    for(long iloc=0; iloc<nloc; iloc++){
	MW->p[iloc]=amp[iloc];
	MW->p[iloc+nloc]=amp[iloc]*locx[iloc];
	MW->p[iloc+nloc*2]=amp[iloc]*locy[iloc];
    }
    /* MCC = - cci' *M' */
    dmm(&MCC, mcc, M,  "tt", -1);
    PDMAT(MCC, pMCC);
    /* Mtmp =  MW' * B  */
    dmm(&Mtmp, MW, B0, "tn", 1);
    /*Remove tip/tilt from left side*/
    PDMAT(Mtmp, pMtmp);
    for(long iloc=0; iloc<nloc; iloc++){
	double tmp1=pMtmp[iloc][0];
	double tmp2=pMtmp[iloc][1];
	double tmp3=pMtmp[iloc][2];
	for(long jloc=0; jloc<nloc; jloc++){
	    BP[iloc][jloc]=B[iloc][jloc]+
		(pMCC[jloc][0]*tmp1
		 +pMCC[jloc][1]*tmp2
		 +pMCC[jloc][2]*tmp3);
	}
    }
    /* Mtmp = MW' * BP' */
    dzero(Mtmp);
    dmm(&Mtmp, MW, B2, "tt", 1);
    /*Remove tip/tilt from right side*/
    for(long iloc=0; iloc<nloc; iloc++){
	double tmp1=pMCC[iloc][0];
	double tmp2=pMCC[iloc][1];
	double tmp3=pMCC[iloc][2];
	for(long jloc=0; jloc<nloc; jloc++){
	    BP[iloc][jloc]+=
		tmp1*pMtmp[jloc][0]
		+tmp2*pMtmp[jloc][1]
		+tmp3*pMtmp[jloc][2];
	}
    }
    dfree(mcc);
    dfree(M);
    dfree(MW);
    dfree(MCC);
    dfree(Mtmp);
    return B2;
}
/**
   Generate OTF from the B or tip/tilted removed B matrix.
*/
static void genotf_do(cmat **otf, long pttr, long notfx, long notfy, 
		      loc_t *loc, const double *amp, const double *opdbias, double wvl,
		      const dmat* B,  const T_VALID *pval){
    long nloc=loc->nloc;
    dmat *B2;
    if(pttr){//remove p/t/t from the B matrix
	B2=pttr_B(B,loc,amp);
    }else{
	B2=ddup(B);//duplicate since we need to modify it.
    }
    PDMAT(B2, BP);
    if(!*otf){
	*otf=cnew(notfx,notfy);
    }
    PCMAT(*otf,OTF);
    /*Do the exponential.*/
    double k2=pow(2*M_PI/wvl,2);
    double *restrict BPD=malloc(sizeof(double)*nloc);
    for(long iloc=0; iloc<nloc; iloc++){
	for(long jloc=0; jloc<nloc; jloc++){
	    BP[iloc][jloc]=exp(k2*BP[iloc][jloc]);
	}
	BPD[iloc]=pow(BP[iloc][iloc], -0.5);
    }
    double otfnorm=0;
    for(long iloc=0; iloc<nloc; iloc++){
	otfnorm+=amp[iloc]*amp[iloc];
    }

    otfnorm=1./otfnorm;
    struct T_VALID (*qval)[notfx]=(struct T_VALID (*)[notfx])pval;

    dcomplex wvk=2.*M_PI/wvl*I;
    for(long jm=0; jm<notfy; jm++){
	for(long im=0; im<notfx; im++){
	    long (*jloc)[2]=qval[jm][im].loc;
	    double tmp1,tmp2; dcomplex tmp3;
	    register dcomplex tmp=0.;
	    for(long iloc=0; iloc<qval[jm][im].n; iloc++){
		long iloc1=jloc[iloc][0];//iloc1 is continuous.
		long iloc2=jloc[iloc][1];//iloc2 is not continuous.
		tmp1=amp[iloc1]*BPD[iloc1]*BP[iloc1][iloc2];
		tmp2=amp[iloc2]*BPD[iloc2];
		tmp3=opdbias?cexp(wvk*(opdbias[iloc1]-opdbias[iloc2])):1;
		tmp+=tmp1*tmp2*tmp3;
	    }
	    OTF[jm][im]=tmp*otfnorm;
	}
    }
    free(BPD);
    dfree(B2);
}
/**
   A wrapper to execute pttr parallel in pthreads
 */
static void *genotf_wrap(GENOTF_T *data){
    long isa=0;
    const long nsa=data->nsa;
    cmat**otf=(cmat**)data->otf;
    loc_t *loc=data->loc;
    const long nxsa=loc->nloc;
    const double wvl=data->wvl;
    const long ncompx=data->ncompx;
    const long ncompy=data->ncompy;
    const double *area=data->area;
    const double thres=data->thres;
    const cmat *otffull=data->otffull;
    const double *amp=data->amp;
    const long pttr=data->pttr;
    const dmat *B=data->B;
    const T_VALID *pval=data->pval;
    while(LOCK(data->mutex_isa),isa=data->isa++,UNLOCK(data->mutex_isa),isa<nsa){
	if(!detached){
	    fprintf(stderr,"%6ld of %6ld\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b", isa,nsa);
	}
	const double *opdbiasi=NULL;
	if(data->opdbias){
	    opdbiasi=data->opdbias+isa*nxsa;
	}else{
	    opdbiasi=NULL;
	}
	if(otffull && (!area || area[isa]>thres)){
	    ccp(&otf[isa],otffull);//just copy the full array
	}else if(!area || area[isa]>0){ 
	    genotf_do(&otf[isa],pttr,ncompx,ncompy,loc,amp+isa*nxsa,opdbiasi,wvl,B,pval);
	}
    }
    return NULL;
}
/**
   Generate pairs of overlapping points for structure function.  

   2010-11-08: removed amp. It caused wrong otf because it uses the amp of the
   first subaperture to build pval, but this one is not fully illuminated. 
 */
static T_VALID *gen_pval(long notfx, long notfy, loc_t *loc, 
			 double dtheta, double wvl){
    double dux=1./(dtheta*notfx);
    double duy=1./(dtheta*notfy);
    long nloc=loc->nloc;
    double *locx=loc->locx;
    double *locy=loc->locy;
    const long pvaltot=notfx*notfy*nloc*2;
    long (*pval0)[2]=malloc(sizeof(long)*pvaltot);
    if(!pval0){
	error("malloc for %ld failed\n", pvaltot);
    }
    T_VALID *pval=malloc(sizeof(T_VALID)*notfx*notfy);
    T_VALID (*restrict qval)[notfx]=(T_VALID (*)[notfx])(pval);
    long count=0,count2;
    loc_create_map(loc);
    locmap_t *map=loc->map;
    long notfx2=notfx/2;
    long notfy2=notfy/2;
    double duxwvl=dux*wvl;
    double duywvl=duy*wvl;
    double dx1=1./loc->dx;
    long (*mapp)[map->nx]=(long(*)[map->nx])map->p;
    for(long jm=0; jm<notfy; jm++){
	long jm2=(jm-notfy2);//peak in the center
	//long jm2=jm<notfy2?jm:jm-notfy;//peak in the corner
	for(long im=0; im<notfx; im++){
	    long im2=(im-notfx2);
	    //long im2=im<notfx2?im:im-notfx;
	    count2=count;
	    for(long iloc=0; iloc<loc->nloc; iloc++){
		long iy=(long)round((locy[iloc]+jm2*duywvl-map->oy)*dx1);
		long ix=(int)round((locx[iloc]+im2*duxwvl-map->ox)*dx1);
		if (ix>=0 && ix<map->nx && iy>=0 && iy<map->ny) {
		    long iloc2=mapp[iy][ix];
		    if(iloc2--){
			pval0[count][0]=iloc;
			pval0[count][1]=iloc2;
			count++;
		    }
		}
	    }
	    qval[jm][im].loc=pval0+count2;
	    qval[jm][im].n=count-count2;
	}
    }
    loc_free_map(loc);
    //pval0=realloc(pval0, sizeof(int)*count*2); //do not realloc. will change position.
    return pval;
}
/**
   Generate the turbulence covariance matrix B with B(i,j)=-0.5*D(x_i, x_j). We
   are neglecting the DC part since the OTF only depends on the structure
   function.  */
static dmat* genotfB(loc_t *loc, double r0, double L0){
    (void)L0;
    long nloc=loc->nloc;
    double *locx=loc->locx;
    double *locy=loc->locy;
    dmat *B0=dnew(nloc, nloc);
    PDMAT(B0, B);
    const double coeff=6.88*pow(2*M_PI/0.5e-6,-2)*pow(r0,-5./3.)*(-0.5);
    for(long i=0; i<nloc; i++){
	for(long j=i; j<nloc; j++){
	    double rdiff2=pow(locx[i]-locx[j],2)+pow(locy[i]-locy[j],2);
	    B[j][i]=B[i][j]=coeff*pow(rdiff2,5./6.);
	}
    }
    return B0;
}
/**
   Generate OTFs for multiple (sub)apertures. ALl these apertures must share the
   same geometry, but may come with different amplitude map and OPD biasas. if
   pttr is 1, the OTF will have tip/tilt removed. make r0 to infinity to build
   diffraction limited OTF. make r0 to infinity and opdbias to none null to
   build OTF for a static map.*/
void genotf(cmat **otf,    /**<The otf array for output*/
	    loc_t *loc,    /**<the aperture grid (same for all apertures)*/
	    const double *amp,    /**<The amplitude map of all the (sub)apertures*/
	    const double *opdbias,/**<The static OPD bias (complex part of amp). */
	    const double *area,   /**<normalized area of the (sub)apertures*/
	    double thres,  /**<The threshold to consider a (sub)aperture as full*/
	    double wvl,    /**<The wavelength. only needef if opdbias is not null*/
	    double dtheta, /**<Sampling of PSF.*/
	    const dmat *cov,/**<The covariance. If not supplied use r0 for kolmogorov spectrum.*/
	    double r0,     /**<Fried parameter*/
	    double l0,     /**<Outer scale*/
	    long ncompx,   /**<Size of OTF*/
	    long ncompy,   /**<Size of OTF*/
	    long nsa,      /**<Number of (sub)apertures*/
	    long pttr      /**<Remove piston/tip/tilt*/
	     ){
    /*creating pairs of points that both exist with given separation*/
    T_VALID *pval=gen_pval(ncompx, ncompy, loc, dtheta, wvl);//returns T_VALID array.
    /* Generate the B matrix. */
    dmat *B=cov?(dmat*)cov:genotfB(loc, r0, l0);
    cmat *otffull=NULL;
    const long nloc=loc->nloc;
    long isafull=-1;
    if(!opdbias && nsa>1){
	double maxarea=0;
	for(long isa=0; isa<nsa; isa++){
	    if(area[isa]>maxarea){
		maxarea=area[isa];
		isafull=isa;
	    }
	}
	if(isafull>0){
	    genotf_do(&otffull,pttr,ncompx,ncompy,loc,amp+isafull*nloc,NULL,wvl,B,pval);
	}
    }
    
    GENOTF_T data;
    memset(&data, 0, sizeof(GENOTF_T));
    data.isa=0;
    PINIT(data.mutex_isa);
    data.otf=otf;
    data.loc=loc;
    data.amp=amp;
    data.opdbias=opdbias;
    data.area=area;
    data.thres=thres;
    data.wvl=wvl;
    data.ncompx=ncompx;
    data.ncompy=ncompy;
    data.nsa=nsa;
    data.pttr=pttr;//was missing.
    data.B=B;
    data.pval=pval;
    data.isafull=isafull;
    data.otffull=otffull;

    CALL(genotf_wrap, &data, NCPU, 1);
    cfree(otffull);
    if(!cov) dfree(B);
    free(pval[0].loc);
    free(pval);
}
