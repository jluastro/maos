/*
  Copyright 2009-2020 Lianqi Wang <lianqiw-at-tmt-dot-org>
  
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
#include <tgmath.h>
#include "drawdaemon.h"
/*
  Routines in this file handles I/O.

  Todo: fread does not block when there are no more data available, and simply
  return EOF. Consider changing to read, which blocks when no data available.
 */
int ndrawdata=0;
int count=0;
int byte_float=sizeof(float);

PNEW2(drawdata_mutex);
//This file does not link to math folder
void fmaxmin(const float *p, long n, float *pmax, float *pmin){
    float max=-INFINITY, min=INFINITY;
    for(long i=0; i<n; i++){
	if(!isnan(p[i])){//not NAN
	    if(p[i]>max){
		max=p[i];
	    }
	    if(p[i]<min){
		min=p[i];
	    }
	}
    }
    if(pmax) *pmax=max;
    if(pmin) *pmin=min;
}
/**
   convert float to int
*/
static unsigned int crp(float x, float x0){
    float res=1.5-4.*fabs(x-x0);
    if(res>1) res=1.;
    else if(res <0) res=0.;
    return (unsigned int)(res*255.);
}

/**
   convert float to char with color map*/
void flt2pix(long nx, long ny, int color, const float *restrict p,  void *pout, float *zlim){
    if(zlim[0]>=zlim[1]){
	fmaxmin(p, nx*ny, zlim+1, zlim);
    }
    round_limit(zlim, zlim+1, 0);
    float min=zlim[0];
    float max=zlim[1];
    if(color){/*colored */
	int *pi=(int*)pout;
	float scale,offset;
	if(fabs(max-min)>1.e-4*fabs(min)){
	    scale=1./(max-min);
	    offset=0;
	}else{
	    scale=0;
	    offset=0.5;
	}
	for(int i=0; i<nx*ny; i++){
	    if(isnan(p[i])){
		pi[i]=0;
	    }else{
		float x=(p[i]-min)*scale+offset;
		pi[i]=255<<24 | crp(x,0.75)<<16 | crp(x, 0.5)<<8 | crp(x, 0.25);
	    }
	}
    }else{/*b/w */
	unsigned char *pc=(unsigned char*)pout;
	float scale=255./(max-min);
	for(int i=0; i<nx*ny; i++){
	    pc[i]=(unsigned char)((p[i]-min)*scale);
	}
    }
}
#define STREADINT(p) if(streadint(sock, &p)) goto end;
#define STREAD(p,len) if(stread(sock,p,len)) goto end;
#define STREADSTR(p) if(streadstr(sock, &p)) goto end;
#define STREADFLT(p,len) if(stread(sock, p, len*byte_float)) goto end;	\
    if(byte_float!=4){							\
	for(int i=0; i<len; i++){					\
	    ((float*)p)[i]=(float)(((double*)p)[i]);			\
	}								\
    }									

void* listen_draw(void*dummy){
    (void)dummy;
    dbg("listen_draw is listening at %d\n", sock);
    //TIC;tic;
    static drawdata_t *drawdata=NULL;
    int cmd=0;
    while(!streadint(sock, &cmd)){
	//dbg("cmd=%d\n", cmd);
	sock_block=0;//Indicate connection is active
	switch (cmd){
	case DRAW_START:
	    //tic;
	    if(drawdata){
		warning("listen_draw: drawdata is not empty\n");
	    }
	    drawdata=mycalloc(1,drawdata_t);
	    LOCK(drawdata_mutex);
	    ndrawdata++;
	    UNLOCK(drawdata_mutex);
	    drawdata->zoomx=1;
	    drawdata->zoomy=1;
	    drawdata->square=1;/*default to square. */
	    drawdata->name=NULL;
	    drawdata->format=(cairo_format_t)0;
	    drawdata->gray=0;
	    drawdata->ticinside=1;
	    drawdata->legendbox=1;
	    drawdata->legendcurve=1;
	    drawdata->legendoffx=1;
	    drawdata->legendoffy=0;
	    drawdata->fig=NULL;
	    drawdata->xylog[0]='n';
	    drawdata->xylog[1]='n';
	    drawdata->cumulast=-1;/*mark as unknown. */
	    drawdata->limit_manual=0;
	    drawdata->time=myclockd();
	    break;
	case DRAW_DATA:/*image data. */
	    {
		int32_t header[2];
		STREAD(header, 2*sizeof(int32_t));
		drawdata->nx=header[0];
		drawdata->ny=header[1];
		int nx=drawdata->nx;
		int ny=drawdata->ny;
		drawdata->p0=malloc(nx*ny*byte_float);//use double to avoid overflow
		if(nx*ny>0){
		    STREADFLT(drawdata->p0, nx*ny);
		}
	    }
	    break;
	case DRAW_POINTS:
	    {
		//dbg("DRAW_POINTS\n");
		int nptsx, nptsy;
		int ipts=drawdata->npts;
		drawdata->npts++;
		STREADINT(nptsx);
		STREADINT(nptsy);
		STREADINT(drawdata->square);
		drawdata->grid=1;
		drawdata->pts=myrealloc(drawdata->pts, drawdata->npts,float*);
		drawdata->pts[ipts]=malloc(nptsx*nptsy*byte_float);
		drawdata->ptsdim=(int(*)[2])realloc(drawdata->ptsdim, drawdata->npts*sizeof(int)*2);
		drawdata->ptsdim[ipts][0]=nptsx;
		drawdata->ptsdim[ipts][1]=nptsy;
		if(nptsx*nptsy>0){
		    STREADFLT(drawdata->pts[ipts], nptsx*nptsy);
		    if(nptsx>50){
			if(!drawdata->icumu){
			    drawdata->icumu=nptsx/10;
			}
		    }
		}
		    
	    }
	    break;
	case DRAW_STYLE:
	    STREADINT(drawdata->nstyle);
	    drawdata->style=mycalloc(drawdata->nstyle,int32_t);
	    STREAD(drawdata->style, sizeof(int32_t)*drawdata->nstyle);
	    break;
	case DRAW_CIRCLE:
	    STREADINT(drawdata->ncir);
	    drawdata->cir=(float(*)[4])calloc(4*drawdata->ncir, byte_float);
	    STREADFLT(drawdata->cir,4*drawdata->ncir);
	    break;
	case DRAW_LIMIT:
	    drawdata->limit_data=malloc(4*byte_float);
	    STREADFLT(drawdata->limit_data, 4);
	    drawdata->limit_manual=1;
	    break;
	case DRAW_FIG:
	    STREADSTR(drawdata->fig);
	    break;
	case DRAW_NAME:
	    STREADSTR(drawdata->name);
	    break;
	case DRAW_TITLE:
	    STREADSTR(drawdata->title);
	    break;
	case DRAW_XLABEL:
	    STREADSTR(drawdata->xlabel);
	    break;
	case DRAW_YLABEL:
	    STREADSTR(drawdata->ylabel);
	    break;
	case DRAW_ZLIM:
	    drawdata->zlim=malloc(2*byte_float);
	    STREADFLT(drawdata->zlim, 2);
	    break;
	case DRAW_LEGEND:
	    drawdata->legend=mycalloc(drawdata->npts,char*);
	    for(int i=0; i<drawdata->npts; i++){
		STREADSTR(drawdata->legend[i]);
	    }
	    break;
	case DRAW_XYLOG:
	    STREAD(drawdata->xylog, sizeof(char)*2);
	    break;
	case DRAW_FINAL:
	    //dbg("client is done\n");
	    sock_block=1;
	    break;
	case DRAW_FLOAT:
	    STREADINT(byte_float);
	    if(byte_float>8){
		error("invalid byte_float=%d\n", byte_float);
	    }
	    break;
	case DRAW_END:
	    {
		if(drawdata->p0){/*draw image */
		    int nx=drawdata->nx;
		    int ny=drawdata->ny;
		    size_t size=0;
		    if(nx<=0 || ny<=0) error("Please call _DATA\n");
		    if(drawdata->gray){
			drawdata->format = (cairo_format_t)CAIRO_FORMAT_A8;
			size=1;
		    }else{
			drawdata->format = (cairo_format_t)CAIRO_FORMAT_ARGB32;
			size=4;
		    }
		    int stride=cairo_format_stride_for_width(drawdata->format, nx);
		    if(!drawdata->limit_manual){
			if(!drawdata->limit_data){
			    drawdata->limit_data=mycalloc(4,float);
			}
			drawdata->limit_data[0]=-0.5;
			drawdata->limit_data[1]=drawdata->nx-0.5;
			drawdata->limit_data[2]=-0.5;
			drawdata->limit_data[3]=drawdata->ny-0.5;
		    }
		    /*convert data from float to int/char. */
		    if(!drawdata->zlim){
			drawdata->zlim=mycalloc(2,float);
		    }
		    drawdata->p=(unsigned char*)calloc(nx*ny, size);
		    flt2pix(nx, ny, !drawdata->gray, drawdata->p0, drawdata->p, drawdata->zlim);
		    drawdata->image= cairo_image_surface_create_for_data 
			(drawdata->p, drawdata->format, nx, ny, stride);
		}
		if(drawdata->npts>0){
		    drawdata->cumuquad=1;
		    if(drawdata->nstyle>1){
			if(drawdata->nstyle!=drawdata->npts){
			    warning("nstyle must equal to npts\n");
			    drawdata->nstyle=0;/*disable it. */
			    free(drawdata->style);
			}
		    }
		}
		if(!drawdata->fig) drawdata->fig=strdup("unkown");
		drawdata_t **drawdatawrap=mycalloc(1,drawdata_t*);
		drawdatawrap[0]=drawdata;
		gdk_threads_add_idle(addpage, drawdatawrap);
		/*if(drawdata->p0){
		    toc("fifo_read image %dx%d", drawdata->nx, drawdata->ny);
		}else{
		    toc("fifo_read points");
		    }*/
		drawdata=NULL;
	    }
	    break;
	case -1:
	    goto end;/*read failed. */
	    break;
	default:
	    warning("Unknown cmd: %x\n", cmd);
	    /*{
	      static int errcount=0;
	      if(errcount++>10){
	      goto end;
	      }}*/
	    break;
	}/*switch */
	cmd=-1;
    }/*while */
 end:
    sock=-1;
    sock_block=1;
    warning("Read failed, stop listening.\n");
    return NULL;
}
