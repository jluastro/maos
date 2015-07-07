/*
  Copyright 2009-2015 Lianqi Wang <lianqiw@gmail.com> <lianqiw@tmt.org>
  
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
#ifndef AOS_CUDA_TOMO_H
#define AOS_CUDA_TOMO_H
#include "solve.h"
#include "recon_base.h"
#include "prop_wrap.h"
#include "fdpcg.h"
namespace cuda_recon{
typedef struct GPU_GP_T{
    int ipowfs;
    int nwfs; //number of wfs in this group
    int jwfs; //wfs index in this group.
    int (*saptr)[2];//index in ploc for lower left corner of subaperture
    Real *PTT;
    Real *PDF;
    Real *PDFTT;
    Real dsa;
    int nsa;
    short2 *GPp;
    Real GPscale;
    int pos;
    int nxp;
    Real dxp, dyp;/*pmap dx*/
    Real oxp, oyp;/*pmap origin*/
    const Real(*neai)[3];
    GPU_GP_T(){
	memset(this, 0, sizeof(*this));	
    }
}GPU_GP_T;
struct LAP_T{
    int nxps,nyps;
    Real l2c;
    int zzi;
    Real zzv;
};
class cutomo_grid:public cusolve_r, public cucg_t{
    curecon_geom *grid;
    /*Temporary data*/
    curcell opdwfs;
    curcell grad;  
    curmat ttf;

    /*Configuration data*/
    curcell neai;
    curcell PTT;  /**< Global tip/tilt */
    curcell PDF;  /**< Differential focus removal */
    curcell PDFTT;/**<Coupling between DF and TT*/
    cucell<int> saptr;
    cucell<short2> GPp;
    Real *GPscale;
    cuspcell GP;
    int ptt;       /**< piston/tip/tilt removal in L()*/
    int nwfs;
    map_ray hx;
    cumat<GPU_GP_T>gpdata;
    cumat<LAP_T> lap;
    void init(const PARMS_T *parms, const RECON_T *recon, const POWFS_T *powfs);
    void do_gp(curcell &grad, const curcell &opdwfs, int ptt, stream_t &stream);
    void do_gpt(curcell &opdwfs, curcell &grad, int ptt, stream_t &stream);
    cutomo_grid &operator=(const cutomo_grid &);
public:
    cutomo_grid(const PARMS_T *parms=0, const RECON_T *recon=0, 
		const POWFS_T *powfs=0, curecon_geom *_grid=0);
    void init_hx(const PARMS_T *parms, const RECON_T *recon);
    void update_fdpcg(FDPCG_T *fdpcg){
	dynamic_cast<cufdpcg_t*>(precond)->update(fdpcg);
    }
    virtual void R(curcell &out, Real beta, 
		   curcell &xin, Real alpha, stream_t &stream);
    virtual void L(curcell &out, Real beta, 
		   const curcell &xin, Real alpha, stream_t &stream);
    virtual void Rt(curcell &out, Real beta, 
		    curcell &xin, Real alpha, stream_t &stream);
    ~cutomo_grid(){
	delete[] GPscale;
    }
};

}//namespace
#endif
