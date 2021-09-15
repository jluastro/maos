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
#include "common.h"
#include "sim.h"
#include "sim_utils.h"
#include "ahst.h"
#include "powfs_utils.h"
#include "save.h"
#include "setup_recon.h"
#include "recon_utils.h"
#include "setup_powfs.h"
#include "pywfs.h"
#if USE_CUDA
#include "../cuda/gpu.h"
#endif
/**
   contains functions that computes WFS gradients in geometric or physical optics mode.
*/
#define TIMING 0
#if TIMING
#define TIM(A) real tk##A=myclockd()
#else
#define TIM(A)
#endif

/**
   Propagate atm onto WFS subaperture grid, and then to fine lenslet grid.
*/
void wfs_ideal_atm(sim_t* simu, dmat* opd, int iwfs, real alpha){
	const parms_t* parms=simu->parms;
	powfs_t* powfs=simu->powfs;
	const int ipowfs=parms->wfs[iwfs].powfs;
	const int jwfs=P(parms->powfs[ipowfs].wfsind, iwfs);
	const real hs=parms->wfs[iwfs].hs;
	const real hc=parms->wfs[iwfs].hc;
	if(parms->sim.wfsalias==2||parms->sim.idealwfs==2){
		loc_t* aloc=P(powfs[ipowfs].fit[jwfs].aloc, 0);
		dcell* wfsopd=dcellnew(1, 1); P(wfsopd, 0)=dnew(aloc->nloc, 1);
		fit_t* fit=&powfs[ipowfs].fit[jwfs];
		muv_solve(&wfsopd, &fit->FL, &fit->FR, 0);
		prop_nongrid(aloc, P(P(wfsopd, 0)), powfs[ipowfs].loc, P(opd), alpha, 0, 0, 1, 0, 0);
		dcellfree(wfsopd);
	} else{
		const int wfsind=P(parms->powfs[ipowfs].wfsind, iwfs);
		for(int idm=0; idm<parms->ndm; idm++){
			loc_t* loc=powfs[ipowfs].loc_dm?P(powfs[ipowfs].loc_dm, wfsind, idm):powfs[ipowfs].loc;
			const real ht=parms->dm[idm].ht+parms->dm[idm].vmisreg;
			real dispx=ht*parms->wfs[iwfs].thetax;
			real dispy=ht*parms->wfs[iwfs].thetay;
			//wfs is registered to pupil. wfs.hc only effects the cone effect.
			real scale=1.-(ht-hc)/hs;
			if(scale<0) continue;
			prop_grid(P(simu->dmprojsq, idm), loc, P(opd),
				alpha, dispx, dispy, scale, 0, 0, 0);
		}
	}
}

/**
   computes close loop and pseudo open loop gradidents for both gometric and
   physical optics WFS. Calls wfsints() to accumulate WFS subapertures images in
   physical optics mode.  */

void wfsgrad_iwfs(thread_t* info){
	sim_t* simu=(sim_t*)info->data;
	const int isim=simu->wfsisim;
	const int iwfs=info->start;
	const parms_t* parms=simu->parms;
	const int ipowfs=parms->wfs[iwfs].powfs;
	//if(isim<parms->powfs[ipowfs].step) return;
	assert(iwfs<parms->nwfs);
	/*
	  simu->gradcl is CL grad output (also for warm-restart of maxapriori
	  simu->gradacc is internal, to accumulate geometric grads.
	  do not accumulate opd. accumate ints for phy, g for GS
	*/
	/*input */

	mapcell* atm=simu->atm;
	const recon_t* recon=simu->recon;
	const powfs_t* powfs=simu->powfs;
	/*output */
	const int CL=parms->sim.closeloop;
	const int nps=parms->atm.nps;
	const real atmscale=simu->atmscale?P(simu->atmscale, isim):1;
	const real dt=parms->sim.dt;
	TIM(0);
	/*The following are truly constants for this powfs */
	const int imoao=parms->powfs[ipowfs].moao;
	const int wfsind=P(parms->powfs[ipowfs].wfsind, iwfs);
	const real hs=parms->wfs[iwfs].hs;
	const int dtrat=parms->powfs[ipowfs].dtrat;
	const int save_gradgeom=P(parms->save.gradgeom, iwfs);
	const int save_opd=P(parms->save.wfsopd, iwfs);
	const int save_ints=P(parms->save.ints, iwfs);
	const int noisy=parms->powfs[ipowfs].noisy;
	/*The following depends on isim */
	const int do_phy=simu->wfsflags[ipowfs].do_phy;
	const int do_pistat=simu->wfsflags[ipowfs].do_pistat;
	const int do_geom=(!do_phy||save_gradgeom||do_pistat)&&parms->powfs[ipowfs].type==WFS_SH;
	const dmat* realamp=powfs[ipowfs].realamp?P(powfs[ipowfs].realamp, wfsind):0;
	dmat* gradcalc=NULL;
	dmat** gradacc=&P(simu->gradacc, iwfs);
	dmat** gradout=&P(simu->gradcl, iwfs);
	dcell* ints=P(simu->ints, iwfs);
	dmat* opd=P(simu->wfsopd, iwfs);
	dzero(opd);
	if(isim%dtrat==0){
		dcellzero(ints);
		dzero(*gradacc);
	}
	/* Now begin ray tracing. */
	if(atm&&((!parms->sim.idealwfs&&!parms->powfs[ipowfs].lo)
		||(!parms->sim.wfsalias&&parms->powfs[ipowfs].lo))){
		for(int ips=0; ips<nps; ips++){
			thread_t* wfs_prop=simu->wfs_prop_atm[iwfs+parms->nwfs*ips];
			propdata_t* wfs_propdata=&simu->wfs_propdata_atm[iwfs+parms->nwfs*ips];
			wfs_propdata->phiout=opd;
			wfs_propdata->displacex1=-P(atm, ips)->vx*dt*isim;
			wfs_propdata->displacey1=-P(atm, ips)->vy*dt*isim;
			wfs_propdata->alpha=atmscale;
			/* have to wait to finish before another phase screen. */
			CALL_THREAD(wfs_prop, 0);
		}
	}
	/*
	   Propagate controllable component of atm (within range of DM) to wfs.
	   wfsalias: atm - controllable.
	   idealwfs: just controllable.
	*/
	/* timing: most expensive 0.10 per LGS for*/
	if(!parms->powfs[ipowfs].lo&&(parms->sim.wfsalias||parms->sim.idealwfs)){
		real alpha=parms->sim.idealwfs?1:-1;
		wfs_ideal_atm(simu, opd, iwfs, alpha);
	}


	if(simu->telws){/*Wind shake */
		real tmp=P(simu->telws, isim);
		real angle=simu->winddir?P(simu->winddir, 0):0;
		real ptt[3]={0, tmp*cos(angle), tmp*sin(angle)};
		loc_add_ptt(opd, ptt, powfs[ipowfs].loc);
	}

	real focus=wfsfocusadj(simu, iwfs);
	if(fabs(focus)>1e-20){
		loc_add_focus(opd, powfs[ipowfs].loc, focus);
	}

	/* Add surface error*/
	if(powfs[ipowfs].opdadd&&P(powfs[ipowfs].opdadd, wfsind)){
		dadd(&opd, 1, P(powfs[ipowfs].opdadd, wfsind), 1);
	}

	if(save_opd){
		zfarr_push(simu->save->wfsopdol[iwfs], isim, opd);
	}
	TIM(1);
	if(CL){
		wait_dmreal(simu, simu->wfsisim);
		for(int idm=0; idm<parms->ndm; idm++){
			thread_t* wfs_prop=simu->wfs_prop_dm[iwfs+parms->nwfs*idm];
			propdata_t* wfs_propdata=&simu->wfs_propdata_dm[iwfs+parms->nwfs*idm];
			wfs_propdata->phiout=opd;
			CALL_THREAD(wfs_prop, 0);
		}/*idm */
		real ptt[3]={0,0,0};
		if(simu->ttmreal){
			ptt[1]-=P(simu->ttmreal, 0);
			ptt[2]-=P(simu->ttmreal, 1);
		}
		//For dithering with downlink instead of uplink FSM 
		if(simu->fsmreal&&NE(simu->fsmreal, iwfs)&&!powfs[ipowfs].llt){
			ptt[1]-=P(P(simu->fsmreal, iwfs), 0);
			ptt[2]-=P(P(simu->fsmreal, iwfs), 1);
		}
		if(ptt[1]||ptt[2]){
			loc_add_ptt(opd, ptt, powfs[ipowfs].loc);
		}
	}
	if(parms->powfs[ipowfs].skip&&parms->tomo.ahst_idealngs==1){
		//apply ideal NGS modes to NGS WFS
		ngsmod2science(opd, powfs[ipowfs].loc, recon->ngsmod,
			parms->wfs[iwfs].thetax, parms->wfs[iwfs].thetay,
			PCOL(simu->cleNGSm, isim), -1);
	}
	if(imoao>-1){
		dmat** dmwfs=P(simu->dm_wfs);
		if(dmwfs[iwfs]){
			/* No need to do mis registration here since the MOAO DM is attached
			   to close to the WFS.*/
			prop_nongrid_pts(P(recon->moao[imoao].aloc, 0), P(dmwfs[iwfs]),
				powfs[ipowfs].pts, P(opd), -1, 0, 0, 1, 0, 0);
		}
	}

	if(parms->powfs[ipowfs].fieldstop>0&&parms->powfs[ipowfs].type==WFS_SH){
		locfft_fieldstop(powfs[ipowfs].fieldstop, opd, parms->powfs[ipowfs].wvlwts);
	}

	if(save_opd){
		zfarr_push(simu->save->wfsopd[iwfs], isim, opd);
	}
	if(parms->plot.run){
		drawopdamp("wfsopd", powfs[ipowfs].loc, opd, realamp, P(parms->dbg.draw_opdmax),
			"WFS OPD", "x (m)", "y (m)", "WFS %d", iwfs);
	}
	if(do_geom){
		/* Now Geometric Optics gradient calculations. if dtrat==1, we compute
		   gradients directly to gradacc, which is the same as gradcalc. If
		   dtrat>1, we compute gradients to gradcalc, and accumulate to
		   gradacc. gradcalc is used to shift pistat. We DONOT include gradoff
		   adjustment to gradref, but only do it on gradcl. This will make the
		   pistat always peak in center no matter what NCPA is present.
		*/
		if(!do_pistat||parms->powfs[ipowfs].pistatstc||dtrat==1){
			//we do not need separate gradcalc.
			gradcalc=dref(*gradacc);
		}//else: calculate first to gradcalc then add to gradacc
		if(parms->powfs[ipowfs].gtype_sim==GTYPE_Z){ /*compute ztilt. */
			pts_ztilt(&gradcalc, powfs[ipowfs].pts,
				PR(powfs[ipowfs].saimcc, wfsind, 0),
				P(realamp), P(opd));
		} else{/*G tilt */
			dspmm(&gradcalc, PR(powfs[ipowfs].GS0, wfsind, 0), opd, "nn", 1);
		}
		if(P(gradcalc)!=P(*gradacc)){
			dadd(gradacc, 1, gradcalc, 1);
		}
	}

	ccell* psfout=NULL;
	zfarr* psfoutzfarr=NULL;
	zfarr* ztiltoutzfarr=NULL;
	if(parms->powfs[ipowfs].psfout){
		psfout=P(simu->wfspsfout, iwfs);
		psfoutzfarr=simu->save->wfspsfout[iwfs];
		ztiltoutzfarr=simu->save->ztiltout[iwfs];
	}
	TIM(2);
	/* Now begin Physical Optics Intensity calculations */
	if(do_phy||psfout||do_pistat||parms->powfs[ipowfs].dither==1){
		dmat* lltopd=NULL;
		if(powfs[ipowfs].llt){//If there is LLT, apply FSM onto LLT
			if(powfs[ipowfs].llt->ncpa){
				lltopd=ddup(PR(powfs[ipowfs].llt->ncpa, wfsind, 0));
			} else{
				lltopd=dnew(NX(powfs[ipowfs].llt->pts), NX(powfs[ipowfs].llt->pts));
			}
			const long illt=P(parms->powfs[ipowfs].llt->i, wfsind);
			if(atm){/*LLT OPD */
				for(int ips=0; ips<nps; ips++){
					const real hl=P(atm, ips)->h;
					const real scale=1.-hl/hs;
					if(scale<0) continue;
					const real ox=P(parms->powfs[ipowfs].llt->ox, illt);
					const real oy=P(parms->powfs[ipowfs].llt->oy, illt);
					const real thetax=parms->wfs[iwfs].thetax-ox/hs;
					const real thetay=parms->wfs[iwfs].thetay-oy/hs;
					const real displacex=-P(atm, ips)->vx*isim*dt+thetax*hl+ox;
					const real displacey=-P(atm, ips)->vy*isim*dt+thetay*hl+oy;
					prop_grid_pts(P(atm, ips), powfs[ipowfs].llt->pts,
						P(lltopd), atmscale, displacex, displacey,
						scale, 1., 0, 0);
				}
			}
			real ttx=0, tty=0;//FSM + wind shake induced jitter
			if(NE(simu->fsmreal, iwfs)||do_pistat||parms->sim.idealfsm){
				if(do_pistat||parms->sim.idealfsm){
					/* remove tip/tilt completely */
					dmat* lltg=dnew(2, 1);
					pts_ztilt(&lltg, powfs[ipowfs].llt->pts,
						powfs[ipowfs].llt->imcc,
						P(powfs[ipowfs].llt->amp),
						P(lltopd));
					P(P(simu->fsmreal, iwfs), 0)=-P(lltg, 0);
					P(P(simu->fsmreal, iwfs), 1)=-P(lltg, 1);
					dfree(lltg);
				}
				ttx=P(P(simu->fsmreal, iwfs), 0);
				tty=P(P(simu->fsmreal, iwfs), 1);
			}
			if(simu->telws){
				real tmp=P(simu->telws, isim)*parms->powfs[ipowfs].llt->ttrat;
				real angle=simu->winddir?P(simu->winddir, 0):0;
				ttx+=tmp*cos(angle);
				tty+=tmp*sin(angle);
			}
			if(simu->llt_tt&&P(simu->llt_tt, iwfs)){
				ttx+=P(P(simu->llt_tt, iwfs), isim);//put all to x direction.
			}
			if(ttx!=0||tty!=0){ /* add tip/tilt to llt opd */
				real ptt[3]={0, ttx, tty};
				loc_add_ptt(lltopd, ptt, powfs[ipowfs].llt->loc);
			}
			if(save_opd){
				zfarr_push(simu->save->wfslltopd[iwfs], isim, lltopd);
			}
		}
		if(parms->powfs[ipowfs].type==WFS_SH){//SHWFS
			wfsints_t* intsdata=simu->wfs_intsdata+iwfs;
			intsdata->ints=ints;
			intsdata->psfout=psfout;
			intsdata->pistatout=P(simu->pistatout, iwfs);
			if(parms->powfs[ipowfs].pistatout){
				intsdata->gradref=gradcalc;
			}
			intsdata->opd=opd;
			intsdata->lltopd=lltopd;
			intsdata->isim=isim;
			CALL_THREAD(simu->wfs_ints[iwfs], 0);
			dfree(lltopd);
			intsdata->lltopd=0;
			intsdata->opd=0;
			if(psfout){
				zfarr_push(psfoutzfarr, isim, psfout);
				zfarr_push(ztiltoutzfarr, isim, *gradacc);
			}
		} else{//Pywfs
			pywfs_fft(&P(ints, 0), powfs[ipowfs].pywfs, opd);
			dscale(P(ints, 0), parms->wfs[iwfs].sigsim);
		}
	}
	TIM(3);
	if(simu->wfsflags[ipowfs].gradout){
		if(do_phy){
			/* In Physical optics mode, do integration and compute
			   gradients. The matched filter are in x/y coordinate even if
			   radpix=1. */
			if(save_ints){
				zfarr_push(simu->save->intsnf[iwfs], isim, ints);
			}
			if(noisy){/*add noise */
				if(P(parms->save.gradnf, iwfs)){//save noise free gradients
					if(parms->powfs[ipowfs].type==WFS_SH){
						shwfs_grad(gradout, P(ints), parms, powfs, iwfs, parms->powfs[ipowfs].phytype_sim);
					} else{
						pywfs_grad(gradout, powfs[ipowfs].pywfs, P(ints, 0));
					}
					zfarr_push(simu->save->gradnf[iwfs], isim, *gradout);
				}
				const real rne=parms->powfs[ipowfs].rne;
				const real bkgrnd=parms->powfs[ipowfs].bkgrnd*dtrat;
				const real bkgrndc=bkgrnd*parms->powfs[ipowfs].bkgrndc;
				dmat** bkgrnd2=NULL;
				dmat** bkgrnd2c=NULL;
				if(powfs[ipowfs].bkgrnd){
					bkgrnd2=PCOLR(powfs[ipowfs].bkgrnd, wfsind);
				}
				if(powfs[ipowfs].bkgrndc){
					bkgrnd2c=PCOLR(powfs[ipowfs].bkgrndc, wfsind);
				}
				for(int isa=0; isa<NX(ints); isa++){
					dmat* bkgrnd2i=(bkgrnd2)?bkgrnd2[isa]:NULL;
					dmat* bkgrnd2ic=(bkgrnd2c)?bkgrnd2c[isa]:NULL;
					addnoise(P(ints, isa), &simu->wfs_rand[iwfs],
						bkgrnd, bkgrndc, bkgrnd2i, bkgrnd2ic, parms->powfs[ipowfs].qe, rne, 1.);
				}
				if(save_ints){
					zfarr_push(simu->save->intsny[iwfs], isim, ints);
				}
			}
			if(parms->powfs[ipowfs].i0save==2){
				dcelladd(&P(simu->ints, iwfs), 1, ints, 1);
			}
			if(parms->powfs[ipowfs].dither==1&&isim>=parms->powfs[ipowfs].dither_ogskip
				&&parms->powfs[ipowfs].type==WFS_SH
				&&(parms->powfs[ipowfs].dither_amp==0||parms->powfs[ipowfs].phytype_sim2==PTYPE_MF)){
				 /*Collect statistics with dithering*/
				dither_t* pd=simu->dither[iwfs];
				dcelladd(&pd->imb, 1, ints, 1.);
				if(parms->powfs[ipowfs].dither_amp){//when dither_amp==0, only accumulate i0
					real cs, ss;
					dither_position(&cs, &ss, parms->sim.alfsm, parms->powfs[ipowfs].dtrat,
						parms->powfs[ipowfs].dither_npoint, isim, pd->deltam);
					//accumulate for matched filter
					
					dcelladd(&pd->imx, 1, ints, cs);
					dcelladd(&pd->imy, 1, ints, ss);
				}
			}

			if(parms->powfs[ipowfs].type==WFS_SH){
				shwfs_grad(gradout, P(ints), parms, powfs, iwfs, parms->powfs[ipowfs].phytype_sim);
			} else{
				pywfs_grad(gradout, powfs[ipowfs].pywfs, P(ints, 0));
			}
		} else{
			/* geomtric optics accumulation mode. scale and copy results to output. */
			dcp(gradout, *gradacc);
			if(dtrat!=1){
				dscale(*gradout, 1./dtrat);/*average */
			}
			if(P(parms->save.gradnf, iwfs)){
				zfarr_push(simu->save->gradnf[iwfs], isim, *gradout);
			}

			if(noisy&&!parms->powfs[ipowfs].usephy){
				const dmat* neasim=PR(powfs[ipowfs].neasim, wfsind, 0);//neasim is the LL' decomposition
				addnoise_grad(*gradout, neasim, &simu->wfs_rand[iwfs]);
			}
		}
		if(save_gradgeom&&do_phy){
			dmat* gradtmp=NULL;
			dadd(&gradtmp, 1, *gradacc, 1./dtrat);
			zfarr_push(simu->save->gradgeom[iwfs], isim, gradtmp);/*noise free. */
			dfree(gradtmp);
		}
	}//dtrat_out
	dfree(gradcalc);
	TIM(4);
#if TIMING==1
	info2("WFS %d grad timing: atm %.2f dm %.2f ints %.2f grad %.2f\n", iwfs, tk1-tk0, tk2-tk1, tk3-tk2, tk4-tk3);
#endif
}
/**
   Demodulate the dithering signal to determine the amplitude. Remove trend (detrending) if detrend is set.
*/
static real calc_dither_amp(dmat* signal, /**<array of data. nmod*nsim */
	long dtrat,   /**<skip columns due to wfs/sim dt ratio*/
	long npoint,  /**<number of points during dithering*/
	int detrend   /**<flag for detrending (remove linear signal)*/
){
	const long nmod=NX(signal);
	long nframe=(signal->ny-1)/dtrat+1;//number of actual frames
	real slope=0;//for detrending
	long offset=(nframe/npoint-1)*npoint;//number of WFS frame separations between first and last cycle
	if(detrend&&offset){//detrending
		for(long ip=0; ip<npoint; ip++){
			for(long im=0; im<nmod; im++){
				long i0=ip*dtrat*nmod+im;
				long i1=(ip+offset)*dtrat*nmod+im;
				slope+=P(signal, i1)-P(signal, i0);
			}
		}
		slope/=(npoint*nmod*offset);
		//dbg("slope=%g. npoint=%ld, nmod=%ld, nframe=%ld, offset=%ld\n", slope, npoint, nmod, nframe, offset);
	}
	real anglei=M_PI*2/npoint;
	real ipv=0, qdv=0;
	switch(nmod){
	case 1://single mode dithering
		for(int iframe=0; iframe<nframe; iframe++){
			real angle=anglei*iframe;//position of dithering
			real cs=cos(angle);
			real ss=sin(angle);
			real mod=P(signal, iframe*dtrat)-slope*iframe;
			ipv+=(mod*cs);
			qdv+=(mod*ss);
		}
		break;
	case 2://tip and tilt dithering
		for(int iframe=0; iframe<nframe; iframe++){
			real angle=anglei*iframe;//position of dithering
			real cs=cos(angle);
			real ss=sin(angle);
			real ttx=P(signal, iframe*dtrat*2)-slope*iframe;
			real tty=P(signal, iframe*dtrat*2+1)-slope*iframe;
			ipv+=(ttx*cs+tty*ss);
			qdv+=(ttx*ss-tty*cs);
		}
		break;
	default:
		error("Invalid nmod");

	}
	real a2m=sqrt(ipv*ipv+qdv*qdv)/nframe;
	return a2m;
}

/*Compute global tip/tilt error for each WFS*/
static void wfsgrad_fsm(sim_t* simu, int iwfs){
	const parms_t* parms=simu->parms;
	recon_t* recon=simu->recon;
	const int ipowfs=parms->wfs[iwfs].powfs;
	const int isim=simu->wfsisim;
	/*Uplink FSM*/
	const int ind=parms->recon.glao?(ipowfs+ipowfs*parms->npowfs):(iwfs+iwfs*parms->nwfs);
	dmat* PTT=recon->PTT?(P(recon->PTT, ind)):0;
	if(!PTT){
		error("powfs %d has FSM, but PTT is empty\n", ipowfs);
	}
	/* Compute FSM error. */
	simu->fsmerr=simu->fsmerr_store;
	dmm(&P(simu->fsmerr, iwfs), 0, PTT, P(simu->gradcl, iwfs), "nn", 1);
	//2021-09-16: drift signal is treated as bias. do not zero fsmerr_drift
	dadd(&P(simu->fsmerr, iwfs), 1, P(simu->fsmerr_drift, iwfs), 1);
	//Save data
	P(P(simu->fsmerrs, iwfs), 0, isim)=P(P(simu->fsmerr, iwfs), 0);
	P(P(simu->fsmerrs, iwfs), 1, isim)=P(P(simu->fsmerr, iwfs), 1);
}
static void wfsgrad_ttf_drift(dmat *grad, sim_t *simu, int iwfs, int remove){
	//gain can be set to 1 if the rate is slower than the main tip/tilt and focus control rate.
	const parms_t* parms=simu->parms;
	recon_t* recon=simu->recon;
	const int ipowfs=parms->wfs[iwfs].powfs;
	const int iwfsr=parms->recon.glao?ipowfs:iwfs;

	if(parms->powfs[ipowfs].trs){
		/* Update T/T drift signal to prevent matched filter from drifting*/
		dmat* tt=dnew(2, 1);
		dmat* PTT=P(recon->PTT, iwfsr, iwfsr);
		dmm(&tt, 0, PTT, grad, "nn", 1);
		if(remove){
			dmat *TT=P(recon->TT, iwfsr, iwfsr);
			dmm(&grad, 1, TT, tt, "nn", -1);
		}
		P(P(simu->fsmerr_drift, iwfs), 0)=P(tt, 0);
		P(P(simu->fsmerr_drift, iwfs), 1)=P(tt, 1);
		if(iwfs==P(parms->powfs[ipowfs].wfs, 0)){
			dbg("Step %5d: wfs %d uplink drift control error is (%.3f, %.3f)mas\n",
			simu->wfsisim, iwfs, P(tt, 0)*206265000, P(tt, 1)*206265000);
		}
		dfree(tt);
	}
	//Output focus error in ib to trombone error signal.
	if(parms->powfs[ipowfs].llt){
		dmat* focus=dnew(1, 1);
		dmat* RFlgsg=P(recon->RFlgsg, iwfs, iwfs);
		dmm(&focus, 0, RFlgsg, grad, "nn", 1);
		if(remove){
			dmm(&grad, 1, P(recon->GFall, iwfs), focus, "nn", -1);
		}
		P(simu->zoomerr_drift, iwfs)=P(focus, 0);
		if(iwfs==P(parms->powfs[ipowfs].wfs, 0)||!parms->powfs[ipowfs].zoomshare){
			double deltah=P(focus, 0)*2*pow(parms->powfs[ipowfs].hs, 2);
			dbg("Step %5d: wfs %d trombone drift control error is %.3f m\n",
			simu->wfsisim, iwfs, deltah);
		}
		dfree(focus);
	}
}
/**
   Accumulate dithering parameters
   - Every step: accumulate signal for phase detection.
   - At PLL output: determine input/output amplitude of dithering signal.
   - At Gain output:determine matched filter i0, gx, gy, or CoG gain.
   - Subtract t/t from gradients for non-comon-path (TT) dithering.

*/
static void wfsgrad_dither(sim_t* simu, int iwfs){
	const parms_t* parms=simu->parms;
	recon_t* recon=simu->recon;
	powfs_t* powfs=simu->powfs;
	const int ipowfs=parms->wfs[iwfs].powfs;
	const int iwfsr=parms->recon.glao?ipowfs:iwfs;
	const int isim=simu->wfsisim;
	const int pllrat=parms->powfs[ipowfs].dither_pllrat;
	if(!parms->powfs[ipowfs].dither||isim<parms->powfs[ipowfs].dither_pllskip){
		return;
	}
	real cs, ss; //Current phase of tip/tilt dithering signal
	dither_t* pd=simu->dither[iwfs];
	if(parms->powfs[ipowfs].dither==1&&parms->powfs[ipowfs].dither_amp){ //T/T dithering.
		//Current dithering signal phase
		dither_position(&cs, &ss, parms->sim.alfsm, parms->powfs[ipowfs].dtrat,
			parms->powfs[ipowfs].dither_npoint, isim, pd->deltam);

		/* Use delay locked loop to determine the phase of actual
		dithering signal (position of LGS spot averaged over a WFS
		integration period) using measured signal (WFS global
		tip/tilt). In actual system, the LGS uplink propagation
		needs to be accounted.
		*/
		real err;
		err=(-ss*(P(P(simu->fsmerr, iwfs), 0))
			+cs*(P(P(simu->fsmerr, iwfs), 1)))/(parms->powfs[ipowfs].dither_amp);
		pd->delta+=parms->powfs[ipowfs].dither_gpll*(err/pllrat);

		//For SHWFS CoG gaim update.
		if(parms->powfs[ipowfs].type==WFS_SH&&parms->powfs[ipowfs].phytype_sim2!=PTYPE_MF&&isim>=parms->powfs[ipowfs].dither_ogskip){
			const int nsa=powfs[ipowfs].saloc->nloc;
			if(!pd->ggm){
				pd->ggm=dnew(nsa*2, 1);
			}
			for(int isa=0; isa<nsa; isa++){
				P(pd->ggm, isa)+=cs*P(P(simu->gradcl, iwfs), isa);
				P(pd->ggm, isa+nsa)+=ss*P(P(simu->gradcl, iwfs), isa+nsa);
			}
		}
	} else if(parms->powfs[ipowfs].dither>1){ //DM dithering.
		dmat* tmp=0;
		const int idm=parms->idmground;
		//Input dither signal
		dmm(&tmp, 0, P(recon->dither_ra, idm, idm), P(simu->dmreal, idm), "nn", 1);
		P(P(pd->mr, 0), isim)=P(tmp, 0);
		//Measured dither signal
		dmm(&tmp, 0, P(recon->dither_rg, iwfs, iwfs), P(simu->gradcl, iwfs), "nn", 1);
		P(P(pd->mr, 1), isim)=P(tmp, 0);
		dfree(tmp);
	}
	if(simu->wfsflags[ipowfs].pllout&&parms->powfs[ipowfs].dither_amp){
		//Synchronous detection of dither signal amplitude in input (DM) and output (gradients).
		//The ratio between the two is used for (optical) gain adjustment.
		const int npoint=parms->powfs[ipowfs].dither_npoint;
		const int ncol=(pllrat-1)*parms->powfs[ipowfs].dtrat+1;
		if(parms->powfs[ipowfs].dither==1){//TT
			//dbg("deltam=%g is updated to %g+%g=%g\n", pd->deltam, pd->delta, pd->deltao, pd->delta+pd->deltao);
			pd->deltam=pd->delta+(pd->deltao*parms->powfs[ipowfs].dither_gdrift);//output PLL
			dmat* tmp=0;
			const int detrend=parms->powfs[ipowfs].llt?0:1;
			tmp=drefcols(P(simu->fsmcmds, iwfs), simu->wfsisim-ncol+1, ncol);
			pd->a2m=calc_dither_amp(tmp, parms->powfs[ipowfs].dtrat, npoint, detrend);
			dfree(tmp);
			tmp=drefcols(P(simu->fsmerrs, iwfs), simu->wfsisim-ncol+1, ncol);
			pd->a2me=calc_dither_amp(tmp, parms->powfs[ipowfs].dtrat, npoint, detrend);
			dfree(tmp);
		} else if(parms->powfs[ipowfs].dither>1){//DM
			dmat* tmp=0;
			tmp=drefcols(P(pd->mr, 0), simu->wfsisim-ncol+1, ncol);//DM
			pd->a2m=calc_dither_amp(tmp, parms->powfs[ipowfs].dtrat, npoint, 1);
			dfree(tmp);
			tmp=drefcols(P(pd->mr, 1), simu->wfsisim-ncol+1, ncol);//Grad
			pd->a2me=calc_dither_amp(tmp, parms->powfs[ipowfs].dtrat, npoint, 1);
			dfree(tmp);
		}
		//Print PLL phase
		if(iwfs==P(parms->powfs[ipowfs].wfs, 0)){
			const real anglei=(2*M_PI/parms->powfs[ipowfs].dither_npoint);
			const real scale=parms->powfs[ipowfs].dither==1?(1./parms->powfs[ipowfs].dither_amp):1;
			info2("Step %5d wfs %d PLL: delay=%.2f frame, dither amplitude=%.2fx, estimate=%.2fx\n",
				isim, iwfs, pd->deltam/anglei, pd->a2m*scale, pd->a2me*scale);
		}
		if(simu->resdither){
			int ic=simu->wfsflags[ipowfs].pllind;
			P(P(simu->resdither, iwfs), 0, ic)=pd->deltam;
			P(P(simu->resdither, iwfs), 1, ic)=pd->a2m;
			P(P(simu->resdither, iwfs), 2, ic)=pd->a2me;
		}
	}
	
	if(simu->wfsflags[ipowfs].ogacc){//Gain update statistics
		/*if(parms->dbg.gradoff_reset==2 && simu->gradoffisim0<=0 
			&& parms->powfs[ipowfs].phytype_sim2==PTYPE_MF){//trigger accumulation of gradoff
			simu->gradoffisim0=simu->wfsisim;
			simu->gradoffisim=simu->wfsisim;
		}*/
		if(parms->powfs[ipowfs].dither==1){//TT Dither
			real scale1=1./pllrat;
			real amp=pd->a2m;
			real scale2=amp?(scale1*2./(amp)):0;
			if(pd->imb){//computer i0, gx, gy for matched filter
				dcellscale(pd->imb, scale1);
				//Accumulate data for matched filter
				dcelladd(&pd->i0, 1, pd->imb, 1);//imb was already scaled
				dcellzero(pd->imb);
				if(parms->powfs[ipowfs].dither_amp){
					dcelladd(&pd->gx, 1, pd->imx, scale2);
					dcelladd(&pd->gy, 1, pd->imy, scale2);
				}
				dcellzero(pd->imx);
				dcellzero(pd->imy);
			} else if(pd->ggm&&parms->powfs[ipowfs].dither_amp){//cog
				dadd(&pd->gg0, 1, pd->ggm, scale2);
				dzero(pd->ggm);
			}
		}
	} 

	if(parms->powfs[ipowfs].dither==1&&parms->powfs[ipowfs].dither_amp){
		/* subtract estimated tip/tilt dithering signal to avoid perturbing the loop or dithering pattern.*/
		real amp=pd->a2me;
		real tt[2]={-cs*amp, -ss*amp};
		if(parms->powfs[ipowfs].trs){
			//info("fsmerr: %g %g %g %g\n", P(P(simu->fsmerr,iwfs),0), P(P(simu->fsmerr,iwfs),1), -tt[0], -tt[1]);
			if(!amp){//no estimate yet, do not close up FSM loop.
				P(P(simu->fsmerr, iwfs), 0)=0;
				P(P(simu->fsmerr, iwfs), 1)=0;
			} else{
				P(P(simu->fsmerr, iwfs), 0)+=tt[0];
				P(P(simu->fsmerr, iwfs), 1)+=tt[1];
			}
		}
		//also remove from gradient measurements.
		dmulvec(P(P(simu->gradcl, iwfs)), P(recon->TT, iwfsr, iwfsr), tt, 1);
	}
}

/**
   Accomplish two tasks:
   1) Use LPF'ed LGS focus measurement to drive the trombone.
   2) HPF LGS focus on gradients to remove sodium range variation effact.

   We trust the focus measurement of the LGS WFS at high temporal frequency
   which NGS cannot provide due to low frame rate. After the HPF on lgs
   gradients, our system is NO LONGER affected by sodium layer variation.

   if sim.mffocus==1: The HPF is applied to each LGS WFS independently. This largely
   removes the effect of differential focus. powfs.dfrs is no longer needed. (preferred).

   if sim.mffocus==2: We apply a LPF on the average focus from six LGS WFS, and
   then remove this value from all LGS WFS. The differential focus is still
   present and powfs.dfrs need to be set to 1 to handle it in tomography. This
   is the original focus tracking method, and is no longer recommended.
*/
static void wfsgrad_lgsfocus(sim_t* simu){
	const parms_t* parms=simu->parms;
	const recon_t* recon=simu->recon;
	extern int update_etf;

	dcell* LGSfocus=simu->LGSfocus;//computed in wfsgrad_post from gradcl.

	for(int ipowfs=0; ipowfs<parms->npowfs; ipowfs++){
		const int do_phy=simu->wfsflags[ipowfs].do_phy;
		/*New plate mode focus offset for LGS WFS. Not really needed*/
		if(!simu->wfsflags[ipowfs].gradout||!parms->powfs[ipowfs].llt
			||simu->wfsisim<parms->powfs[ipowfs].step||!do_phy){
			continue;
		}

		if(parms->tomo.ahst_focus==2
			&&simu->Mint_lo&&P(simu->Mint_lo->mint, 1)&&simu->wfsflags[ipowfs].gradout){
			 /*When tomo.ahst_focus>0, the first plate scale mode contains focus for
			   lgs. But it turns out to be not necessary to remove it because the
			   HPF in the LGS path removed the influence of this focus mode. set
			   tomo.ahst_focus=2 to enable adjust gradients.*/

			real scale=simu->recon->ngsmod->scale;
			int indps=simu->recon->ngsmod->indps;
			dmat* mint=P(P(simu->Mint_lo->mint, 0), 0);//2018-12-11: changed first p[1] to p[0]
			real focus=P(mint, indps)*(scale-1);
			for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
				int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
				dadd(&P(simu->gradcl, iwfs), 1, P(recon->GFall, iwfs), focus);
			}
		}


		const int zoomset=(parms->powfs[ipowfs].zoomset
			&&simu->wfsisim==parms->sim.start
			&&parms->powfs[ipowfs].phytype_sim!=PTYPE_MF);

		real lgsfocusm=0;
		if(parms->powfs[ipowfs].zoomshare||parms->sim.mffocus==2){
			lgsfocusm=0;
			for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
				int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
				lgsfocusm+=P(P(LGSfocus, iwfs), 0);
			}
			lgsfocusm/=parms->powfs[ipowfs].nwfs;
		}

		/*Here we set trombone position according to focus in the first
		  measurement. And adjust the focus content of this
		  measurement. This simulates the initial focus acquisition
		  step. No need if start with pre-built matched filter.*/
		for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
			int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
			real zoomerr=parms->powfs[ipowfs].zoomshare?lgsfocusm:P(P(LGSfocus, iwfs), 0);
			if(zoomset){
				P(simu->zoomint, iwfs)=zoomerr;
				if(jwfs==0||!parms->powfs[ipowfs].zoomshare){
					info2("wfs %d: Move trombone by %.1f m.\n", iwfs, 
						P(simu->zoomint, iwfs)*2*pow(parms->powfs[ipowfs].hs, 2));
				}
				update_etf=1;//this is important in bootstraping.
			} else if(parms->powfs[ipowfs].zoomgain>0){
				//Trombone from gradients. always enable
				P(simu->zoomavg, iwfs)+=zoomerr;//zoom averager.
				if(simu->wfsflags[ipowfs].zoomout){
					/*zoom error is zero order hold even if no output from averager*/
					real zoomout=P(simu->zoomavg, iwfs)/parms->powfs[ipowfs].zoomdtrat;
					//drift signal is used as bias. do not zero zoomerr_drift
					P(simu->zoomerr, iwfs)=zoomout+P(simu->zoomerr_drift, iwfs);
					P(simu->zoomavg, iwfs)=0;
					if(jwfs==0||!parms->powfs[ipowfs].zoomshare){
						dbg("wfs %d: trombone averager output %.1f m\n", iwfs, zoomout*2*pow(parms->powfs[ipowfs].hs, 2));
					}
				}
			}
			if(parms->sim.mffocus){//Focus HPF
				real infocus=parms->sim.mffocus==2?lgsfocusm:P(P(LGSfocus, iwfs), 0);
				//In RTC. LPF can be put after using the value to put it off critical path.
				real lpfocus=parms->sim.lpfocushi;
				P(simu->lgsfocuslpf, iwfs)=P(simu->lgsfocuslpf, iwfs)*(1-lpfocus)+infocus*lpfocus;
				dadd(&P(simu->gradcl, iwfs), 1, P(recon->GFall, iwfs), -P(simu->lgsfocuslpf, iwfs));
			}
		}
	}

	/*The zoomerr is prescaled, and ZoH. moved from filter.c as it only relates to WFS.*/
	//Do not trigger update_etf here as it will interfere with i0 accumulation.
	for(int ipowfs=0; ipowfs<parms->npowfs; ipowfs++){
		if(simu->wfsflags[ipowfs].zoomout&&parms->powfs[ipowfs].zoomgain>0){
			real zoomgain=parms->powfs[ipowfs].zoomgain;
			if(parms->powfs[ipowfs].zoomshare){//average zoomerr
				average_powfs(simu->zoomerr, parms->powfs[ipowfs].wfs, 1);
			}
			real dhc=0;
			for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
				int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
				real errsig=P(simu->zoomerr, iwfs)*zoomgain;
				P(simu->zoomint, iwfs)+=errsig;
				P(simu->zoomerr, iwfs)=0;
				dhc+=errsig;
				if(simu->zoompos&&P(simu->zoompos, iwfs)){
					P(P(simu->zoompos, iwfs), simu->wfsflags[ipowfs].zoomout-1)=P(simu->zoomint, iwfs);
				}
			}
			dhc=(dhc/parms->powfs[ipowfs].nwfs)*2*pow(parms->powfs[ipowfs].hs, 2);
			//only force update ETF if dh changes by more than 100 (1% of the sodium profile depth).
			if(parms->powfs[ipowfs].llt->coldtrat==0&& fabs(dhc)>parms->dbg.na_thres){//otherwise, wait until next sodium profile column
				update_etf=1;
			}
		}
	}
}

/**
   Every operation here should be in the Simulator not the Controller
*/
void wfsgrad_post(thread_t* info){
	sim_t* simu=(sim_t*)info->data;
	const parms_t* parms=simu->parms;
	//Postprocessing gradients
	const int isim=simu->wfsisim;
	for(int iwfs=info->start; iwfs<info->end; iwfs++){
#if USE_CUDA
		if(parms->gpu.wfs){
			gpu_wfsgrad_sync(simu, iwfs);
		}
#endif
		const int ipowfs=parms->wfs[iwfs].powfs;
		if(isim<parms->powfs[ipowfs].step) continue;
		const int do_phy=simu->wfsflags[ipowfs].do_phy;
		dmat* gradcl=P(simu->gradcl, iwfs);
		/* copy fsmreal to output  */
		if(NE(simu->fsmreal, iwfs)){
			P(P(simu->fsmcmds, iwfs), 0, isim)=P(P(simu->fsmreal, iwfs), 0);
			P(P(simu->fsmcmds, iwfs), 1, isim)=P(P(simu->fsmreal, iwfs), 1);
		}
		if(simu->wfsflags[ipowfs].gradout){
			if(parms->plot.run){
				/*drawgrad("Gcl", simu->powfs[ipowfs].saloc, gradcl,
					parms->plot.grad2opd, parms->powfs[ipowfs].trs, P(parms->dbg.draw_gmax),
					"WFS Closeloop Gradients", "x (m)", "y (m)", "Gcl %d", iwfs);*/
				if(do_phy){
					drawints("Ints", simu->powfs[ipowfs].saloc, P(simu->ints, iwfs), NULL,
						"WFS Subaperture Images", "x", "y", "wfs %d", iwfs);
				}
			}
			//scaling gradcl
			if(P(simu->gradscale, iwfs)){
				dcwm(gradcl, P(simu->gradscale, iwfs));
			} else{
				dscale(gradcl, parms->powfs[ipowfs].gradscale);
			}
			if(P(simu->gradoff, iwfs)){
				dadd(&P(simu->gradcl, iwfs), 1, P(simu->gradoff, iwfs), -parms->dbg.gradoff_scale);
			}
			if(parms->dbg.gradoff){
				info_once("Add dbg.gradoff to gradient vector\n");
				int icol=(simu->wfsisim+1)%NY(parms->dbg.gradoff);
				dadd(&P(simu->gradcl, iwfs), 1, P(parms->dbg.gradoff, iwfs, icol), -1);
			}

			if(do_phy){
				if(NE(simu->fsmerr_store, iwfs)){
					wfsgrad_fsm(simu, iwfs);
				}
				if(parms->powfs[ipowfs].dither){
					wfsgrad_dither(simu, iwfs);
				}
				if(!parms->powfs[ipowfs].trs&&parms->powfs[ipowfs].skip!=2&&simu->fsmerr){
					dzero(P(simu->fsmerr, iwfs));//do not close fsm loop when t/t is used for AO.
				}
			}
			if(parms->powfs[ipowfs].llt){
				dmm(&P(simu->LGSfocus, iwfs), 0, P(simu->recon->RFlgsg, iwfs, iwfs), P(simu->gradcl, iwfs), "nn", 1);
			}
			if(P(parms->save.grad, iwfs)){
				zfarr_push(simu->save->gradcl[iwfs], isim, gradcl);
			}
		}
	}//for iwfs
}
static void gradoff_acc(sim_t *simu, int ipowfs){
	(void)ipowfs;
	if(simu->parms->dbg.gradoff_reset==2 && simu->gradoffisim0 > 0){//accumulate gradoff before updating it.
		int nsim=(simu->wfsisim-simu->gradoffisim);
		if(nsim){
			dcelladd(&simu->gradoffacc, 1, simu->gradoff, nsim);
			info("step %d: gradoff is accumulated with factor %d\n", simu->wfsisim, nsim);
		}
		simu->gradoffisim=simu->wfsisim;
	}
}
/**
	Controls gradient drift of individual subapertures due to dithering for matched filter
*/
static void wfsgrad_sa_drift(sim_t* simu, int ipowfs){
	const parms_t* parms=simu->parms;
	const powfs_t* powfs=simu->powfs;
	if(parms->powfs[ipowfs].dither_gdrift==0||parms->powfs[ipowfs].phytype_sim!=PTYPE_MF) return;
	dmat* goff=0;
	intstat_t* intstat=simu->powfs[ipowfs].intstat;
	const int isim=simu->wfsisim;
	gradoff_acc(simu, ipowfs);
	
	for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
		int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
		const int nsa=NX(intstat->i0);
		//outer loop to prevent i0 from drifting 
		//Compute CoG of i0 + goff and drive it toward gradncpa with low gain (0.1)
		if(1){
			dzero(goff);
			shwfs_grad(&goff, PCOL(intstat->i0, jwfs), parms, powfs, iwfs, PTYPE_COG);//force cog
			dadd(&goff, 1, P(simu->gradoff, iwfs), 1);
			if(simu->gradoffdrift){//cog boot strapped
				dadd(&goff, 1, P(simu->gradoffdrift, iwfs), -1);
			} else if(simu->powfs[ipowfs].gradncpa){//cmf boot strapped, gradncpa is cog of i0
				dadd(&goff, 1, P(simu->powfs[ipowfs].gradncpa, jwfs), -1);
			}
			dadd(&P(simu->gradoff, iwfs), 1, goff, -parms->powfs[ipowfs].dither_gdrift);
			if(jwfs==0){
				dbg("Step %5d: powfs %d gradient drift control\n", simu->wfsisim, ipowfs);
			}
		}
		if(1){
			//outer loop to prevent gx/gy direction from drifting.
			//It computes CoG of shifted images (i0+gx/gy) and make sure the angle stays the same.
			//may not be necessary.
			dmat* i0sx=0, * i0sy=0;
			real theta=0;
			const real gyoff=M_PI*0.5;
			const real gshift=parms->powfs[ipowfs].pixtheta*0.1;
			const real cogthres=parms->powfs[ipowfs].cogthres;
			const real cogoff=parms->powfs[ipowfs].cogoff;
			
			for(int isa=0; isa<nsa; isa++){
				real g0[2], gx[2], gy[2];
				dcp(&i0sx, PR(intstat->i0, isa, jwfs));
				dcog(g0, i0sx, 0., 0., cogthres, cogoff, 0);
				dcp(&i0sy, PR(intstat->i0, isa, jwfs));
				dadd(&i0sx, 1, PR(intstat->gx, isa, jwfs), gshift);
				dadd(&i0sy, 1, PR(intstat->gy, isa, jwfs), gshift);
				dcog(gx, i0sx, 0., 0., cogthres, cogoff, 0);
				dcog(gy, i0sy, 0., 0., cogthres, cogoff, 0);

				//Works in both x/y and r/a coordinate.
				theta+=(atan2(gx[1]-g0[1], gx[0]-g0[0])+atan2(gy[1]-g0[1], gy[0]-g0[0])-gyoff);
			}
			theta*=0.5/nsa;
			simu->dither[iwfs]->deltao=-theta;
			dfree(i0sx);
			dfree(i0sy);
			if(jwfs==0){
				dbg("Step %5d: wfs %d angle drift control deltao is %g.\n", isim, iwfs, simu->dither[iwfs]->deltao);
			}
		}
	}
	dfree(goff);
	if(parms->save.gradoff||parms->save.dither){
		writebin(simu->gradoff, "extra/gradoff_%d_drift", isim);
	}
}
/**
   Dither update: zoom corrector, matched filter, gain ajustment, TWFS.
*/
static void wfsgrad_dither_post(sim_t* simu){
	powfs_t* powfs=simu->powfs;
	const parms_t* parms=simu->parms;
	const int isim=simu->wfsisim;
	for(int ipowfs=0; ipowfs<parms->npowfs; ipowfs++){
		if(!parms->powfs[ipowfs].dither) continue;
		if(isim<parms->powfs[ipowfs].step) continue;
		if((isim+1)%parms->powfs[ipowfs].dtrat!=0) continue;
		const int nwfs=parms->powfs[ipowfs].nwfs;

		if(simu->wfsflags[ipowfs].ogout){//This is matched filter or cog update
			const int nsa=powfs[ipowfs].saloc->nloc;
			const real scale1=(real)parms->powfs[ipowfs].dither_pllrat/(real)parms->powfs[ipowfs].dither_ograt;
			
			if(parms->powfs[ipowfs].dither_amp==0||(parms->powfs[ipowfs].phytype_sim2==PTYPE_MF)){
				if(parms->powfs[ipowfs].dither_amp==0){
					info2("Step %5d: Update sodium fit for powfs %d\n", isim, ipowfs);
				}else{
					info2("Step %5d: Update matched filter for powfs %d\n", isim, ipowfs);
				}
				//For matched filter
				if(!powfs[ipowfs].intstat){
					powfs[ipowfs].intstat=mycalloc(1, intstat_t);
				}
				intstat_t* intstat=powfs[ipowfs].intstat;
				parms->powfs[ipowfs].radgx=0;//ensure derivate is interpreted as along x/y.
				int pixpsax=powfs[ipowfs].pixpsax;
				int pixpsay=powfs[ipowfs].pixpsay;
				if(!intstat->i0||NY(intstat->i0)!=nwfs){
					dcellfree(intstat->i0);
					intstat->i0=dcellnew_same(nsa, nwfs, pixpsax, pixpsay);
				}
				if(!intstat->gx||NY(intstat->gx)!=nwfs){
					dcellfree(intstat->gx);
					dcellfree(intstat->gy);
					intstat->gx=dcellnew_same(nsa, nwfs, pixpsax, pixpsay);
					intstat->gy=dcellnew_same(nsa, nwfs, pixpsax, pixpsay);
				}
				real g2=parms->powfs[ipowfs].dither_glpf;
				if(simu->wfsflags[ipowfs].ogout*g2<1){//not enough accumulations yet.
					g2=1./(simu->wfsflags[ipowfs].ogout);
				}
				gradoff_acc(simu, ipowfs);
				if(g2<1){
					info("Applying LPF with gain %.2f to i0/gx/gy update at update cycle %d\n", g2, simu->wfsflags[ipowfs].ogout);
				}
				for(int jwfs=0; jwfs<nwfs; jwfs++){
					int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
					dither_t* pd=simu->dither[iwfs];
					//Scale the output due to accumulation
					//TODO: remove the LPF which is not useful. 
					//TODO: combine pd->i0 with instat->i0
					//TODO: accumulate directly to instat->i0 instead of to pd->imx
					for(int isa=0; isa<nsa; isa++){
						dadd(&P(intstat->i0, isa, jwfs), 1-g2, P(pd->i0, isa), scale1*g2);
						if(parms->powfs[ipowfs].dither_amp){
							dadd(&P(intstat->gx, isa, jwfs), 1-g2, P(pd->gx, isa), scale1*g2);
							dadd(&P(intstat->gy, isa, jwfs), 1-g2, P(pd->gy, isa), scale1*g2);
						}
					}
					dcellzero(pd->i0);
					dcellzero(pd->gx);
					dcellzero(pd->gy);
					if(parms->powfs[ipowfs].dither_amp){
						if(parms->dbg.gradoff_reset==0){
							if(jwfs==0) info("Step %5d: powfs%d reducing gradoff by grad of i0.\n", isim, ipowfs);
							dmat* goff=0;
							/*Compute the gradient of i0 using old gradient
							algorithm and subtract from the gradient offset to
							prevent sudden jump of gradient measurement.*/
							shwfs_grad(&goff, PCOL(intstat->i0, jwfs), parms, powfs, iwfs, parms->powfs[ipowfs].phytype_sim);
							dadd(&P(simu->gradoff, iwfs), 1, goff, -1);
							dfree(goff);
							if(parms->powfs[ipowfs].dither_glpf!=1){
								warning("when dbg.gradoff_reset is enabled, dither_glpf should be 1.\n");
							}
						} else if(parms->dbg.gradoff_reset == 1){
							if(jwfs==0) info("Step %5d: powfs%d resetting gradoff to 0.\n", isim, ipowfs);
							dzero(P(simu->gradoff, iwfs));
						} else if(parms->dbg.gradoff_reset==2){
							if(jwfs==0) info("Step %5d: powfs%d reducing gradoff by its average.\n", isim, ipowfs);
							int nacc=simu->gradoffisim-simu->gradoffisim0;
							if(jwfs==0) info("Step %5d: powfs%d gradoffacc is scaled by 1/%d\n", isim, ipowfs, nacc);
							dscale(P(simu->gradoffacc,iwfs), 1./nacc);
							dadd(&P(simu->gradoff, iwfs), 1, P(simu->gradoffacc,iwfs), -1);
						}
					}
				}
				if(parms->powfs[ipowfs].dither_amp==0){
					dmat* sodium=0;
					dcell* grad=0;
					int use_i0=parms->powfs[ipowfs].phytype_sim2==PTYPE_MF?1:0;
					fit_sodium_profile_wrap(&sodium, &grad, intstat->i0, parms, powfs, ipowfs, 1, use_i0, 1);
					for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
						int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
						dcelladd(&grad, -1, powfs[ipowfs].gradncpa, 1);
						wfsgrad_ttf_drift(P(grad, jwfs), simu, iwfs, 1);
						//use focus, tip/tilt to control drift and remove from gradient
						if(parms->powfs[ipowfs].phytype_sim2==PTYPE_COG){
							if(jwfs==0) dbg("in cog mode, gradoff+=(g_ncpa-grad)\n");
							dadd(&P(simu->gradoff, iwfs), 1, P(grad, jwfs), 1);
						} else if(parms->powfs[ipowfs].phytype_sim2==PTYPE_MF){
							if(jwfs==0) dbg("in cmf mode, gradoff is reset to 0, and ncpa is used to create i0 with new sodium profile\n");
							dzero(P(simu->gradoff, iwfs));
						}
					}
					if(parms->save.dither){
						writebin(grad, "extra/powfs%d_NaFit_grad_%d", ipowfs, isim);
						writebin(sodium, "extra/powfs%d_NaFit_sodium_%d", ipowfs, isim);
					}
					dcellfree(grad);
					dfree(sodium);
				}else{
					//tip/tilt and focus drift control.
					//we use gain of 1 as this output is slower than gradient based control.
					dmat* ibgrad=0;
					for(int jwfs=0; jwfs<parms->powfs[ipowfs].nwfs; jwfs++){
						int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
						shwfs_grad(&ibgrad, PCOL(intstat->i0, jwfs), parms, powfs, iwfs, PTYPE_COG);
						wfsgrad_ttf_drift(ibgrad, simu, iwfs, 0);
					}
					dfree(ibgrad);
				}
				//there is no need to reset trombone error signal
				if((parms->save.gradoff||parms->save.dither)&&parms->dbg.gradoff_reset!=1){
					writebin(simu->gradoff, "extra/gradoff_%d_dither", isim);
				}
				if(parms->dbg.gradoff_reset==2){
					dcellzero(simu->gradoffacc);
					simu->gradoffisim0=isim;
				}
				if(!simu->gradoffdrift&&parms->powfs[ipowfs].dither_amp){
					//Use gradoff before adjustment is not good. There are difference between i and i0.
					if(parms->powfs[ipowfs].phytype_sim==PTYPE_MF){
						info2("Step %5d: powfs%d set gradoffdrift to cog of initial i0\n", isim, ipowfs);
					}else{
						info2("Step %5d: powfs%d set gradoffdrift to cog of created i0 + gradoff\n", isim, ipowfs);
					}
					simu->gradoffdrift=dcellnew(nwfs, 1);
					for(int jwfs=0; jwfs<nwfs; jwfs++){
						int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
						shwfs_grad(&P(simu->gradoffdrift, iwfs), PCOLR(powfs[ipowfs].intstat->i0, jwfs), parms, powfs, iwfs, PTYPE_COG);
						if(parms->powfs[ipowfs].phytype_sim!=PTYPE_MF){
							dadd(&P(simu->gradoffdrift, iwfs), 1, P(simu->gradoff, iwfs), 1);//added on 2021-08-11
						}
					}
				}

				if(parms->powfs[ipowfs].phytype_sim!=parms->powfs[ipowfs].phytype_sim2){
					//the following parms changes need to be moved to simu. It affects the next seed.
					parms->powfs[ipowfs].phytype_sim=parms->powfs[ipowfs].phytype_sim2;
					parms->powfs[ipowfs].phytype_recon=parms->powfs[ipowfs].phytype_sim;
					info2("Step %5d: powfs %d changed to %s\n", isim, ipowfs,
						parms->powfs[ipowfs].phytype_sim==PTYPE_MF?"matched filter":"CoG");
				}
				//Generating matched filter
				if(parms->powfs[ipowfs].neareconfile||parms->powfs[ipowfs].phyusenea){
					dbg("Step %5d: powfs %d disable neareconfile and phyusenea\n", isim, ipowfs);
					parms->powfs[ipowfs].neareconfile=NULL;
					parms->powfs[ipowfs].phyusenea=0;
				}
				if(parms->powfs[ipowfs].phytype_sim2==PTYPE_MF){
					parms->powfs[ipowfs].phytype_recon=PTYPE_MF;//Make sure nea is used for reconstruction.
					genmtch(parms, powfs, ipowfs);
#if USE_CUDA
					if(parms->gpu.wfs){
						gpu_wfsgrad_update_mtche(parms, powfs, ipowfs);
					}
#endif
				}
				if(parms->save.dither>1){
					writebin(intstat->i0, "extra/powfs%d_i0_%d", ipowfs, isim);
					if(parms->powfs[ipowfs].phytype_sim2==PTYPE_MF){
						writebin(intstat->gx, "extra/powfs%d_gx_%d", ipowfs, isim);
						writebin(intstat->gy, "extra/powfs%d_gy_%d", ipowfs, isim);
						writebin(intstat->mtche, "extra/powfs%d_mtche_%d", ipowfs, isim);
						writebin(powfs[ipowfs].sanea, "extra/powfs%d_sanea_%d", ipowfs, isim);
					}
				}

				if(!parms->powfs[ipowfs].lo&&parms->recon.alg==0){//no need to update LSR.
					simu->tomo_update=2;
				}
				if(parms->powfs[ipowfs].dither_gdrift>0&&parms->powfs[ipowfs].dither_amp){
					wfsgrad_sa_drift(simu, ipowfs);
				}
			} else if(parms->powfs[ipowfs].dither_amp){
				if(parms->powfs[ipowfs].phytype_sim!=parms->powfs[ipowfs].phytype_sim2){
					error("Does not support switching to CoG.\n");
				}
				//For CoG gain
				for(int jwfs=0; jwfs<nwfs; jwfs++){
					int iwfs=P(parms->powfs[ipowfs].wfs, jwfs);
					dither_t* pd=simu->dither[iwfs];
					const int ng=powfs[ipowfs].saloc->nloc*2;
					if(!P(simu->gradscale, iwfs)){
						P(simu->gradscale, iwfs)=dnew(ng, 1);
						dset(P(simu->gradscale, iwfs), parms->powfs[ipowfs].gradscale);
					}
					real mgold=dsum(P(simu->gradscale, iwfs))/ng;
					real mgnew;
					const char* ogtype=0;
					//gg0 is output/input of dither dithersig.
					if(!pd->gg0||parms->powfs[ipowfs].dither_ogsingle){//single gain for all subapertures. For Pyramid WFS
						ogtype="globally";
						real gerr=pd->a2me/pd->a2m;
#define HIA_G_UPDATE 0
#if HIA_G_UPDATE //HIA method.
						real adj=parms->powfs[ipowfs].dither_gog*mgold*(1-gerr);
						dadds(P(simu->gradscale, iwfs), adj);
						mgnew=mgold+adj;
						while(mgnew<0){//prevent negative gain
							adj*=0.5;
							dadds(P(simu->gradscale, iwfs), -adj);
							mgnew+=-adj;
						}
#else
						real adj=pow(gerr, -parms->powfs[ipowfs].dither_gog);
						dscale(P(simu->gradscale, iwfs), adj);
						mgnew=mgold*adj;
#endif
					} else{//separate gain for each gradient. For shwfs.
						ogtype="on average";
						dscale(pd->gg0, scale1); //Scale value at end of accumulation
						for(long ig=0; ig<ng; ig++){
#if HIA_G_UPDATE
							real adj=parms->powfs[ipowfs].dither_gog*mgold*(1.-P(pd->gg0, ig));
							P(P(simu->gradscale, iwfs), ig)+=adj;
							while(P(P(simu->gradscale, iwfs), ig)<0){
								adj*=0.5;
								P(P(simu->gradscale, iwfs), ig)+=-adj;
							}
#else
							if(P(pd->gg0, ig)>0.01){//skip weakly determined subapertures.
								P(P(simu->gradscale, iwfs), ig)*=pow(P(pd->gg0, ig), -parms->powfs[ipowfs].dither_gog);
							}
#endif
						}
						mgnew=dsum(P(simu->gradscale, iwfs))/ng;
						dzero(pd->gg0);
					}
					info2("Step %5d wfs %d CoG gain adjusted from %g to %g %s.\n",
						isim, iwfs, mgold, mgnew, ogtype);
					if(simu->resdither){
						int ic=simu->wfsflags[ipowfs].pllind;
						P(P(simu->resdither, iwfs), 3, ic)=mgnew;
					}
					//adjust WFS measurement dither dithersig by gain adjustment. used for dither t/t removal from gradients.
					pd->a2me*=(mgnew/mgold);//Adjust for updated gain
					dcellscale(powfs[ipowfs].sanea, pow(mgnew/mgold, 2));
					if(parms->save.dither){
						writebin(P(simu->gradscale, iwfs), "extra/gradscale_wfs%d_%d", iwfs, isim);
					}
				}
			}
		}
	}
}
/**
   TWFS has output. Accumulate result to simu->gradoff. It is put in wfsgrad.c
   instead of recon.c to avoid race condition because it updates simu->gradoff.
*/
void wfsgrad_twfs_recon(sim_t* simu){
	const parms_t* parms=simu->parms;
	const int itpowfs=parms->itpowfs;
	if(simu->wfsflags[itpowfs].gradout){
		info2("Step %5d: TWFS[%d] has output with gain %g\n", simu->wfsisim, itpowfs, simu->eptwfs);
		gradoff_acc(simu, parms->ilgspowfs);//todo: improve ipowfs index.
		const int nlayer=PN(parms->recon.twfs_ipsr);
		dcell* Rmod=0;
		//Build radial mode error using closed loop TWFS measurements from this time step.
		dcellmm(&Rmod, simu->recon->RRtwfs, simu->gradcl, "nn", 1);
		if(simu->wfsflags[itpowfs].gradout<5&&parms->itwfssph>-1){
			dbg("Step %5d: TWFS output %d spherical mode (%d) gain is boosted from %g to %g\n",
				simu->wfsisim, simu->wfsflags[itpowfs].gradout, parms->itwfssph, parms->sim.eptwfs, parms->sim.eptsph);
			for(int ilayer=0; ilayer<nlayer; ilayer++){
				P(P(Rmod, ilayer), parms->itwfssph)*=(parms->sim.eptsph/simu->eptwfs);
			}
		}
		zfarr_push(simu->save->restwfs, simu->wfsflags[itpowfs].gradout-1, Rmod);
		
		for(int iwfs=0; iwfs<parms->nwfs; iwfs++){
			int ipowfs=parms->wfs[iwfs].powfs;
			if(parms->powfs[ipowfs].llt){
				for(int ilayer=0; ilayer<nlayer; ilayer++){
					dmm(&P(simu->gradoff, iwfs), 1, P(simu->recon->GRall, iwfs, ilayer), P(Rmod, ilayer), "nn", -simu->eptwfs);
				}

				if(parms->plot.run){
					extern int draw_single;
					int draw_single_save=draw_single;
					draw_single=0;
					drawgrad("Goff", simu->powfs[ipowfs].saloc, P(simu->gradoff, iwfs),
						parms->plot.grad2opd, parms->powfs[ipowfs].trs, P(parms->dbg.draw_gmax),
						"WFS Offset", "x (m)", "y (m)", "Gtwfs %d", iwfs);
					draw_single=draw_single_save;
				}
			}
		}
		if(parms->save.gradoff){
			writebin(simu->gradoff, "extra/gradoff_%d_twfs", simu->wfsisim);
		}
		dcellfree(Rmod);
	}
}
/**
   Calls wfsgrad_iwfs() to computes WFS gradient in parallel.
   It also includes operations on Gradients before tomography.
*/
void wfsgrad(sim_t* simu){
	real tk_start=PARALLEL==1?simu->tk_0:myclockd();
	const parms_t* parms=simu->parms;
	if(parms->sim.idealfit||parms->sim.evlol||parms->sim.idealtomo) return;
	// call the task in parallel and wait for them to finish. It may be done in CPU or GPU.
	if(1!=PARALLEL||parms->tomo.ahst_idealngs==1||!parms->gpu.wfs){
		CALL_THREAD(simu->wfsgrad_pre, 0);
	}///else: already called by sim.c
	CALL_THREAD(simu->wfsgrad_post, 0);
	wfsgrad_dither_post(simu);//must be before wfsgrad_lgsfocus because wfsgrad_lgsfocus runs zoom integrator.
	if(parms->nlgspowfs){//high pass filter lgs focus to remove sodium range variation effect
		wfsgrad_lgsfocus(simu);
	}
	if(parms->itpowfs!=-1){
		wfsgrad_twfs_recon(simu);
	}
	if(parms->plot.run){
		for(int iwfs=0; iwfs<parms->nwfs; iwfs++){
			int ipowfs=parms->wfs[iwfs].powfs;
			drawgrad("Gcl", simu->powfs[ipowfs].saloc, P(simu->gradcl, iwfs),
				parms->plot.grad2opd, parms->powfs[ipowfs].trs, P(parms->dbg.draw_gmax),
				"WFS Closeloop Gradients Calibrated", "x (m)", "y (m)", "Gcal %d", iwfs);
		}
	}
	//todo: split filter_fsm to per WFS.
	filter_fsm(simu);
	if(1+simu->wfsisim==parms->sim.end){
#if USE_CUDA
		if(parms->gpu.wfs){
			gpu_save_pistat(simu);
		} else
#endif
			save_pistat(simu);
	}
	simu->tk_wfs=myclockd()-tk_start;
}
