#include <mex.h>
#include <math.h>
#include "interface.h"
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    enum{/*input */
	P_NX,
	P_NY,
	P_CX,
	P_CY,
	P_R,
	P_TOT,
    };
    enum{/*output */
	PL_MAP,
	PL_TOT,
    };
    if(nlhs!=PL_TOT || nrhs!=P_TOT){
	mexErrMsgTxt("Usage: [map]=mkcirmap(nx, ny, cx, cy, radius)\n");
    }
    int nx=(int)mxGetScalar(prhs[P_NX]);
    int ny=(int)mxGetScalar(prhs[P_NY]);
    double cx=mxGetScalar(prhs[P_CX])-1;//-1 to convert from matlab convention to C
    double cy=mxGetScalar(prhs[P_CY])-1;
    double R=mxGetScalar(prhs[P_R]);
    dmat *map=dnew(nx, ny);
    dcircle(map, cx, cy, R, 1);
    plhs[PL_MAP]=d2mx(map);
    dfree(map);
}