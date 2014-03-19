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

#ifndef AOS_LIB_CELL_H
#define AOS_LIB_CELL_H
#ifndef AOS_LIB_MATH_H
#error "Don't include this file directly"
#endif

#define AOS_CELL_DEF(X,Y,T,R) \
X(cell) *X(cellnew)(long nx, long ny);\
X(cell) *X(cellnew2)(const X(cell) *A);\
X(cell) *X(cellnew3)(long nx, long ny, long *nnx, long *nny);\
void X(cellinit)(X(cell) **A, long nx, long ny);\
void X(cellzero)(X(cell) *dc);\
void X(cellset)(X(cell)*dc, T alpha);\
void X(cellfree_do)(X(cell) *dc);\
X(cell) *X(celltrans)(const X(cell) *A);\
X(cell) *X(cellref)(const X(cell) *in);\
X(cell) *X(celldup)(const X(cell) *in);\
void X(cellcp)(X(cell)** out0, const X(cell) *in);\
R X(cellnorm2)(const X(cell) *in);\
void X(cellscale)(X(cell) *A, R w);\
X(cell) *X(cellreduce)(const X(cell) *A, int dim);\
X(cell) *X(cellcat)(const X(cell) *A, const X(cell) *B, int dim);\
X(cell) *X(cellcat_each)(const X(cell) *A, const X(cell) *B, int dim);\
void X(celldropempty)(X(cell) **A0, int dim);\
void X(celladd)(X(cell) **B0, R bc, const X(cell) *A,const R ac);\
T X(cellinn)(const X(cell)*A, const X(cell)*B);\
void X(cellcwm)(X(cell) *B, const X(cell) *A);\
void X(cellmm)(X(cell) **C0, const X(cell) *A, const X(cell) *B, const char trans[2], const R alpha);\
X(cell)* X(cellinvspd)(X(cell) *A);\
X(cell)* X(cellinv)(X(cell) *A);\
X(cell)* X(cellinvspd_each)(X(cell) *A);\
X(cell)* X(cellpinv)(const X(cell) *A, const X(cell) *wt, const Y(spcell) *Wsp);\
X(cell)* X(cellsvd_pow)(X(cell) *A, R power, R thres); \
void X(cellcwpow)(X(cell)*A, R power);\
X(mat) *X(cell2m)(const X(cell) *A);\
X(cell)* X(2cellref)(const X(mat) *A, long*dims, long ndim);\
void X(2cell)(X(cell) **B, const X(mat) *A, const X(cell) *ref);\
void X(celldropzero)(X(cell) *B, R thres);\
R X(celldiff)(const X(cell) *A, const X(cell) *B);\
int X(cellclip)(X(cell) *Ac, R min, R max);\
void X(celltikcr)(X(cell) *A, R thres);\
void X(cellmulsp)(X(cell) **C0, const X(cell) *A, const Y(spcell) *B, R alpha);\
void X(celladdI)(X(cell) *A, R a);\
X(cell) *X(cellsub)(const X(cell) *in, long sx, long nx, long sy, long ny);\
X(cell) *X(bspline_prep)(X(mat)*x, X(mat)*y, X(mat) *z);\
X(mat) *X(bspline_eval)(X(cell)*coeff, X(mat) *x, X(mat) *y, X(mat) *xnew, X(mat) *ynew);
#endif