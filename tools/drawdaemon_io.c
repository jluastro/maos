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
#include <tgmath.h>
#include <errno.h>
#include "drawdaemon.h"
/*
  Routines in this file handles I/O.

  Todo: fread does not block when there are no more data available, and simply
  return EOF. Consider changing to read, which blocks when no data available.
 */
 //minimum internet maximum MTU is 576. IP header is 20-60. UDP header os 8. 508 is safest. We round up to 512.
#define UDP_PAYLOAD 512 //maximum size of UDP payload in bytes
#define UDP_HEADER 12 //size of UDP sub frame header in bytes
int ndrawdata=0;
int count=0;
int byte_float=sizeof(float);
udp_t udp_client={0};
int client_port=-1;//client udp port
in_addr_t client_addr;
int udp_sock=-1;//server udp socket

PNEW2(drawdata_mutex);
//This file does not link to math folder
void fmaxmin(const float* p, long n, float* pmax, float* pmin){
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
	else if(res<0) res=0.;
	return (unsigned int)(res*255.);
}

/**
   convert float to char with color map*/
void flt2pix(long nx, long ny, int color, const float* restrict p, void* pout, float* zlim){
	if(zlim[0]>=zlim[1]){
		fmaxmin(p, nx*ny, zlim+1, zlim);
	}
	round_limit(zlim, zlim+1, 0);
	float min=zlim[0];
	float max=zlim[1];
	if(color){/*colored */
		int* pi=(int*)pout;
		float scale, offset;
		if(fabs(max-min)>1.e-4*fabs(min)){
			scale=1./(max-min);
			offset=0;
		} else{
			scale=0;
			offset=0.5;
		}
		for(int i=0; i<nx*ny; i++){
			if(isnan(p[i])){
				pi[i]=0;
			} else{
				float x=(p[i]-min)*scale+offset;
				pi[i]=255<<24|crp(x, 0.75)<<16|crp(x, 0.5)<<8|crp(x, 0.25);
			}
		}
	} else{/*b/w */
		unsigned char* pc=(unsigned char*)pout;
		float scale=255./(max-min);
		for(int i=0; i<nx*ny; i++){
			pc[i]=(unsigned char)((p[i]-min)*scale);
		}
	}
}
void* listen_udp(void *dummy){
	(void)dummy;
	dbg("listen_dup listening at socket %d\n", udp_sock);
	void *buf=0; 
	size_t bufsize=0;
	int counter=0;
	do{
		counter=udp_recv(&udp_client, &buf, &bufsize);
		info("listen_udp: %lu bytes received with counter %d.\n", bufsize, counter);
	}while(counter>0);
	return NULL;
}
#define STREADINT(p) if(streadint(sock, &p)) {close(sock); sock=-1; goto retry;}
#define STREAD(p,len) if(stread(sock,p,len)) {close(sock); sock=-1; goto retry;}
#define STREADSTR(p) if(streadstr(sock, &p)) {close(sock); sock=-1; goto retry;}
#define STREADFLT(p,len) if(stread(sock, p, len*byte_float)) {close(sock); sock=-1; goto retry;}	\
    if(byte_float!=4){							\
	for(int i=0; i<len; i++){					\
	    ((float*)p)[i]=(float)(((double*)p)[i]);			\
	}								\
    }									
int sock;//socket 
int sock_idle=0;//1: no active drawing. 0: active drawing connection. -1: do not retry connection
void* listen_draw(void* user_data){
	char* str2=0;
	char *host=0;
	sock=strtol((char*)user_data, &str2, 10);
	if(str2!=user_data){//argument is a number
		if(sock<0){
			error("sock=%d is invalid\n", sock);
		}
		host=strdup(addr2name(socket_peer(sock)));
	} else{//not a number, hostname
		host=strdup((char*)user_data);
		sock=-1;
	}
retry:
	if(sock<0 && host && sock_idle!=-1){
		dbg_time("Connecting to %s\n", host);
		sock=scheduler_connect(host);
		if(sock==-1){
			warning("connect to %s failed\n", host);
			return NULL;
		}
		int cmd[2]={CMD_DISPLAY, 0};
		if(stwriteintarr(sock, cmd, 2)||streadintarr(sock, cmd, 1)||cmd[0]){
			warning("Failed to pass sock to scheduler.\n");
			close(sock);
			sock=-1;
			return NULL;
		}
	}
	
	if(sock>=0){
		if(socket_block(sock, 0) || socket_recv_timeout(sock, 0)){
			sock=-1;
		}
	}
	static drawdata_t* drawdata=NULL;
	int cmd=0;
	int nlen=0;
	if(sock!=-1) dbg("listen_draw is listening at %d\n", sock);
	while(sock!=-1){
		STREADINT(cmd);//will block if no data is available.
		sock_idle=0;//Indicate connection is active
		if(cmd==DRAW_ENTRY){//every message in new format start with DRAW_ENTRY.
			STREADINT(nlen);
			STREADINT(cmd);
		}
		switch(cmd){
		case DRAW_FRAME:{//in new format, every frame start with this. Place holder to handle UDP.
			int sizes[4];
			STREAD(sizes, sizeof(int)*4);
		};break;
		case DRAW_START:
			//tic;
			if(drawdata){
				warning("listen_draw: drawdata is not empty\n");
			}
			drawdata=mycalloc(1, drawdata_t);
			LOCK(drawdata_mutex);
			ndrawdata++;
			UNLOCK(drawdata_mutex);
			drawdata->zoomx=1;
			drawdata->zoomy=1;
			drawdata->square=-1;
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
			if(nx*ny>0){				
				drawdata->p0=malloc(nx*ny*byte_float);//use double to avoid overflow
				STREADFLT(drawdata->p0, nx*ny);
			}
			drawdata->square=1;//default to square for images.
		}
		break;
		case DRAW_SHM:/*no action*/
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
			drawdata->pts=myrealloc(drawdata->pts, drawdata->npts, float*);
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
			drawdata->style=mycalloc(drawdata->nstyle, int32_t);
			STREAD(drawdata->style, sizeof(int32_t)*drawdata->nstyle);
			break;
		case DRAW_CIRCLE:
			STREADINT(drawdata->ncir);
			drawdata->cir=(float(*)[4])calloc(4*drawdata->ncir, byte_float);
			STREADFLT(drawdata->cir, 4*drawdata->ncir);
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
			STREADFLT(drawdata->zlim, 2);
			break;
		case DRAW_LEGEND:
			drawdata->legend=mycalloc(drawdata->npts, char*);
			for(int i=0; i<drawdata->npts; i++){
				STREADSTR(drawdata->legend[i]);
			}
			break;
		case DRAW_XYLOG:
			STREAD(drawdata->xylog, sizeof(char)*2);
			break;
		case DRAW_FINAL:
			//dbg("client is done\n");
			sock_idle=1;
			break;
		case DRAW_FLOAT:
			STREADINT(byte_float);
			//dbg("byte_float=%d\n", byte_float);
			if(byte_float>8){
				error("invalid byte_float=%d\n", byte_float);
			}
			break;
		case DRAW_UDPPORT://received UDP port from client
		{
			STREADINT(client_port);
			client_addr=socket_peer(sock);
			info("received udp port %d fron client %s\n", client_port, addr2name(client_addr));
			
			if(udp_sock<=0){
				udp_sock=bind_socket(SOCK_DGRAM, 0, 0);
			}
			int server_port=socket_port(udp_sock);
			struct sockaddr_in name;
			name.sin_family=AF_INET;
			name.sin_addr.s_addr=client_addr;
			name.sin_port=htons(client_port);
			if(connect(udp_sock, (const struct sockaddr*)&name, sizeof(name))){
				warning("connect udp socket to client failed with error %d\n", errno);
			}else{
				//initial handshake with fixed buffer size of 64 ints. The length can not be increased.
				int cmd2[64]={0};
				cmd2[0]=DRAW_ENTRY;
				cmd2[1]=sizeof(int)*4;
				cmd2[2]=1;//protocol version
				cmd2[3]=server_port;
				cmd2[4]=UDP_PAYLOAD;
				cmd2[5]=UDP_HEADER;
				udp_client.header=UDP_HEADER;
				udp_client.payload=UDP_PAYLOAD;
				udp_client.peer_addr=name;
				udp_client.version=1;
				udp_client.sock=udp_sock;
				if(send(udp_sock, cmd2, sizeof(cmd2),0)<(ssize_t)sizeof(cmd2)){
					warning("write to client failed with error %d\n", errno);
				}else{
					thread_new(listen_udp, NULL);
				}
			}
		}
		break;
		case DRAW_END:
		{
			
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
			if(!drawdata->fig) drawdata->fig=strdup("unknown");
			drawdata_t** drawdatawrap=mycalloc(1, drawdata_t*);
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
			goto retry;/*read failed. */
			break;
		default:
			warning_time("Unknown cmd: %d with size %d\n", cmd, nlen);
			if(nlen){
				void* p=malloc(nlen);
				STREAD(p, nlen);
				free(p);
			}
		}/*switch */
		cmd=-1;
	}/*while */

	warning_time("Stop listening.\n");
	if(sock!=-1) close(sock);
	sock=-1;
	sock_idle=1;
	return NULL;
}
