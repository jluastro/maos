/*
  Copyright 2009-2016 Lianqi Wang <lianqiw-at-tmt-dot-org>
  
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
#include "interface.h"
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    enum{
	P_COEFF,
	P_DTHI,
	P_DTRAT,
	P_GWFS,
	P_RWFS,
	P_PROJ,
	P_TOT,
    };
    enum{
	PL_KALMAN,
	PL_TOT,
    };
    (void)nlhs;
    if(nrhs!=P_TOT){
	mexErrMsgTxt("Usage: kalman=sde_kalman_mex(coeff, dthi, dtrat, Gwfs, Rwfs, Proj)\n");
    }
    dmat *coeff  = mx2d(prhs[P_COEFF]);
    double dthi  = (double)mxGetScalar(prhs[P_DTHI]);
    dmat *dtrat  = mx2d(prhs[P_DTRAT]);
    dcell *Gwfs  = mx2dcell(prhs[P_GWFS]);
    if(dtrat->ny>1){
	if(dtrat->nx!=1){
	    mexErrMsgTxt("dtrat has unknown format\n");
	}
	dtrat->nx=dtrat->ny;
	dtrat->ny=1;
    }
    if(dtrat->nx!=Gwfs->nx){
	if(dtrat->nx!=1){
	    error("dtrat should have dimension %ldx1\n", Gwfs->nx);
	}
	dmat *dtrat2=dnew(Gwfs->nx, 1);
	dset(dtrat2, dtrat->p[0]);
	dfree(dtrat); dtrat=dtrat2;
    }
    dcell *Rwfs  = mx2dcell(prhs[P_RWFS]);
    dmat *Proj   = mx2d(prhs[P_PROJ]);
    kalman_t *kalman=sde_kalman(coeff, dthi, dtrat, Gwfs, Rwfs, Proj);
    int nfield=12;
    const char *fieldnames[]={"Ad","Cd","AdM","FdM","Qn","Rn","M","P", "dthi", "dtrat", "Gwfs", "Rwfs"};
    plhs[0]=mxCreateStructMatrix(1,1,nfield,fieldnames);
    int pos=0;
    mxSetFieldByNumber(plhs[0], 0, pos++, d2mx(kalman->Ad));
    mxSetFieldByNumber(plhs[0], 0, pos++, dcell2mx(kalman->Cd));
    mxSetFieldByNumber(plhs[0], 0, pos++, d2mx(kalman->AdM));
    mxSetFieldByNumber(plhs[0], 0, pos++, d2mx(kalman->FdM));
    mxSetFieldByNumber(plhs[0], 0, pos++, d2mx(kalman->Qn));
    mxSetFieldByNumber(plhs[0], 0, pos++, dcell2mx(kalman->Rn));
    mxSetFieldByNumber(plhs[0], 0, pos++, dcell2mx(kalman->M));
    mxSetFieldByNumber(plhs[0], 0, pos++, d2mx(kalman->P));
    mxSetFieldByNumber(plhs[0], 0, pos++, mxDuplicateArray(prhs[P_DTHI]));
    mxSetFieldByNumber(plhs[0], 0, pos++, mxDuplicateArray(prhs[P_DTRAT]));
    mxSetFieldByNumber(plhs[0], 0, pos++, mxDuplicateArray(prhs[P_GWFS]));
    mxSetFieldByNumber(plhs[0], 0, pos++, mxDuplicateArray(prhs[P_RWFS]));
    
    kalman_free(kalman);
}
