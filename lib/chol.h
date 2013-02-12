/*
  Copyright 2009-2012 Lianqi Wang <lianqiw@gmail.com> <lianqiw@tmt.org>
  
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

#ifndef AOS_CHOL_H
#define AOS_CHOL_H
#include "dsp.h"
#include "dmat.h"
#include "cmat.h"
#ifndef CHOLMOD_CORE_H
typedef struct cholmod_factor_struct cholmod_factor;
typedef struct cholmod_common_struct cholmod_common;
#endif
typedef struct spchol{
    cholmod_factor *L;
    cholmod_common *c;
    dsp   *Cl;/*The sparse matrix (lower). A=Cl*CL' with reordering.*/
    dsp   *Cu;/*The sparse matrix (upper). A=Cu'*Cu with reordering. Cu==CL'*/
    spint *Cp;/*The Permutation vector.*/
}spchol;
/* assume large file support.  If problems occur, compile with -DNLARGEFILE */
spchol* chol_factorize(dsp *A_in);
spchol* chol_factorize_cache(dsp *A_in);
void chol_solve(dmat **x, spchol *A, dmat *y);
dsp *chol_spsolve(spchol *A, const dsp *y);
void chol_free_do(spchol *A);
#define chol_free(A) ({chol_free_do(A);A=NULL;})
void chol_save(spchol *A, const char *format,...) CHECK_ARG(2);
spchol *chol_read(const char *format, ...) CHECK_ARG(1);
void chol_convert(spchol *A, int keep);
void chol_solve_lower(dmat **x, spchol *C, dmat *y);
void chol_solve_upper(dmat **x, spchol *C, dmat *y);
#endif
