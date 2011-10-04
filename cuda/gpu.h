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
#ifndef AOS_CUDA_GPU_H
#define AOS_CUDA_GPU_H
#include "../lib/aos.h"
#include "../maos/parms.h"
#include "../maos/types.h"
/* Data for propagation to WFS */
typedef struct{
    dmat *phi;//return.
    int iwfs;
    int isim;
    const PARMS_T *parms;
    const POWFS_T *powfs;
    float atmalpha;
    float dmalpha;
}gpu_wfs_t;
void gpu_info(void);
int  gpu_init(int *gpus, int ngpu);
void gpu_cleanup(void);
void gpu_atm2gpu(map_t **atm, int nps);
void gpu_atm2gpu_new(map_t **atm, const PARMS_T *parms, int iseed, int isim);
void gpu_dmreal2gpu(map_t **dmreal, int ndm, DM_CFG_T *dmcfg);
void gpu_dmproj2gpu(map_t **dmproj, int ndm, DM_CFG_T *dmcfg);

void gpu_wfsgrad_init(const PARMS_T *parms, const POWFS_T *powfs);
void gpu_wfs_init_sim(const PARMS_T *parms, POWFS_T *powfs);
void gpu_wfsgrad_seeding(const PARMS_T *parms, const POWFS_T *powfs, rand_t *rstat);
void gpu_wfsgrad(thread_t *info);
void gpu_wfsgrad_save(SIM_T *simu);
void gpu_wfssurf2gpu(const PARMS_T *parms, POWFS_T *powfs);

void gpu_evlsurf2gpu(APER_T *aper);
void gpu_perfevl_init(const PARMS_T *parms, APER_T *aper);
void gpu_perfevl_ngsr(SIM_T *simu, double *cleNGSm);
void gpu_perfevl(thread_t *info);
void gpu_perfevl_save(SIM_T *simu);
void gpu_perfevl_init_sim(const PARMS_T *parms, APER_T *aper);
void gpu_setup_recon(const PARMS_T *parms, POWFS_T *powfs, RECON_T *recon);
void gpu_recon_reset(void);
void gpu_tomo(SIM_T *simu);
void gpu_fit(SIM_T *simu);
#endif

