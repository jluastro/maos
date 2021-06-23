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
/*
  A monitor that monitors the progress of all running processes in all machines.

  The widgets duplicates and manages the string.
  Set NOTIFY_IS_SUPPORTED to 0 is libnotify is not installed.

  Todo:
  1) Log the activities into a human readable file
  2) Store the activity list into file when quit and reload later
*/

#include <errno.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <glib/gprintf.h>
#include <pthread.h>
#include <fcntl.h>

#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifndef GTK_WIDGET_VISIBLE
#define GTK_WIDGET_VISIBLE gtk_widget_get_visible
#endif
#include "monitor.h"
#if MAC_INTEGRATION //In newer GTK>3.6, using GtkApplication instead of this extension.
#include <gtkosxapplication.h>
#endif
#if WITH_NOTIFY
#include <libnotify/notify.h>
static int notify_daemon=1;
#endif

GdkPixbuf* icon_main=NULL;
GdkPixbuf* icon_finished=NULL;
GdkPixbuf* icon_failed=NULL;
GdkPixbuf* icon_running=NULL;
GdkPixbuf* icon_waiting=NULL;
GdkPixbuf* icon_cancel=NULL;
GdkPixbuf* icon_save=NULL;
GdkPixbuf* icon_skip=NULL;
GdkPixbuf* icon_clear=NULL;
GdkPixbuf* icon_connect=NULL;

GtkWidget* notebook=NULL;
GtkWidget** pages;
GtkWidget* window=NULL;
//static GtkWidget** tabs;
static GtkWidget** titles;
static GtkWidget** cmdconnect;
GtkTextBuffer** buffers;
double* usage_cpu, * usage_cpu2;
//double *usage_mem, *usage_mem2;
static GtkWidget** prog_cpu;
//static GtkWidget **prog_mem;
PangoAttrList* pango_active, * pango_down;
#if GTK_MAJOR_VERSION<3
static GtkStatusIcon* status_icon=0;
GdkColor blue;
GdkColor green;
GdkColor red;
GdkColor yellow;
GdkColor white;
GdkColor color_even;
GdkColor color_odd;
#endif
GtkWidget* toptoolbar;
#if GTK_MAJOR_VERSION>=3 
//GtkCssProvider *provider_prog;
GtkCssProvider* provider_red;
GtkCssProvider* provider_blue;
#include "gtk3-css.h"
#endif
int* hsock;
#define MAX_HOST 20

#define dialog_msg(A...) {				\
	GtkWidget *dialog0=gtk_message_dialog_new	\
	    (GTK_WINDOW(window),		\
	     GTK_DIALOG_DESTROY_WITH_PARENT,		\
	     GTK_MESSAGE_INFO,				\
	     GTK_BUTTONS_CLOSE,				\
	     A);					\
	gtk_dialog_run(GTK_DIALOG(dialog0));		\
	gtk_widget_destroy(dialog0);			\
    }

/**
   The number line pattern determines how dash is drawn for gtktreeview. the
   first number is the length of the line, and second number of the length
   of blank after the line.

   properties belonging to certain widget that are descendent of GtkWidget, need
to specify the classname before the key like GtkTreeView::allow-rules */
#if GTK_MAJOR_VERSION<3
static const gchar* rc_string_widget=
{

"style \"widget\" {               \n"
"font_name = \"Sans 8\""
	"}\n"
"class \"GtkWidget\" style \"widget\" \n"
};
static const gchar* rc_string_treeview=
{
"style \"solidTreeLines\"{                       \n"
/*" GtkTreeView::grid-line-pattern=\"\111\111\"\n" */
" GtkTreeView::grid-line-width   = 1             \n"
" GtkTreeView::allow-rules       = 1             \n"
/*" GtkTreeView::odd-row-color     = \"#EFEFEF\"   \n"
  " tkTreeView::even-row-color    = \"#FFFFFF\"   \n"*/
" GtkTreeView::horizontal-separator = 0          \n"
" GtkTreeView::vertical-separator = 0            \n"
"}\n                                             \n"
"class \"GtkTreeView\" style \"solidTreeLines\"  \n"
};

static const gchar* rc_string_entry=
{
"style \"entry\" {               \n"
/*"base[NORMAL] = \"white\"        \n"
  "bg[SELECTED] = \"#0099FF\"         \n"*/
"xthickness   = 0                \n"
"ythickness   = 0                \n"
"GtkEntry::inner-border    = {1,1,1,1}     \n"
"GtkEntry::progress-border = {0,0,0,0}     \n"
"GtkEntry::has-frame       = 0 \n"
"}\n"
"class \"GtkEntry\" style \"entry\" \n"
};
#endif
/**
   Enables the notebook page for a host*/
gboolean host_up(gpointer data){
	int ihost=GPOINTER_TO_INT(data);
	gtk_widget_set_sensitive(cmdconnect[ihost], 0);
	gtk_widget_hide(cmdconnect[ihost]);
	gtk_label_set_attributes(GTK_LABEL(titles[ihost]), pango_active);
	return 0;
}
/**
   Disables the notebook page for a host*/
gboolean host_down(gpointer data){
	int ihost=GPOINTER_TO_INT(data);
	gtk_button_set_label(GTK_BUTTON(cmdconnect[ihost]), "Click to connect");
	gtk_widget_show_all(cmdconnect[ihost]);
	gtk_widget_set_sensitive(cmdconnect[ihost], 1);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(prog_cpu[ihost]), 0);
	//gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(prog_mem[ihost]), 0);
	gtk_label_set_attributes(GTK_LABEL(titles[ihost]), pango_down);
	return 0;
}
/**
* determine host index from name
*/
int host2i(const char *hostn){
	for(int ihost=0; ihost<nhost; ihost++){
		if(!strcmp(hosts[ihost], hostn)){
			return ihost;
		}
	}
	warning("host %s is not found\n", hostn);
	return -1;
}
/**
   modifies the color of progress bar*/
static void modify_bg(GtkWidget* widget, int type){
#if GTK_MAJOR_VERSION>=3 
	GtkCssProvider* provider;
	switch(type){
	case 1:
		provider=provider_blue;
		break;
	case 2:
		provider=provider_red;
		break;
	default:
		provider=NULL;
	}
	GtkStyleContext* context=gtk_widget_get_style_context(widget);
	gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);
#else
	GdkColor* color;
	switch(type){
	case 1:
		color=&blue;
		break;
	case 2:
		color=&red;
		break;
	default:
		color=NULL;
	}
	gtk_widget_modify_bg(widget, GTK_STATE_SELECTED, color);
	gtk_widget_modify_bg(widget, GTK_STATE_PRELIGHT, color);
#endif
}
/**
   updates the progress bar for a job*/
gboolean update_progress(gpointer input){
	int ihost=GPOINTER_TO_INT(input);
	double last_cpu=usage_cpu2[ihost];
	//double last_mem=usage_mem2[ihost];
	usage_cpu2[ihost]=usage_cpu[ihost];
	//usage_mem2[ihost]=usage_mem[ihost];
	if(GTK_WIDGET_VISIBLE(window)){
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(prog_cpu[ihost]), usage_cpu[ihost]);
		//gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(prog_mem[ihost]), usage_mem[ihost]);
		if(usage_cpu[ihost]>=0.8&&last_cpu<0.8){
			modify_bg(prog_cpu[ihost], 2);
		} else if(usage_cpu[ihost]<0.8&&last_cpu>=0.8){
			modify_bg(prog_cpu[ihost], 1);
		}
			/*
		if(usage_mem[ihost]>=0.8 && last_mem<0.8){
			modify_bg(prog_mem[ihost],2);
		}else if(usage_mem[ihost]<0.8 && last_mem>=0.8){
			modify_bg(prog_mem[ihost],1);
				}*/
	}
	return 0;
}

void notify_user(proc_t* p){
	if(p->status.done||p->status.info==S_START) return;
#if WITH_NOTIFY
	if(!notify_daemon) return;
	if(p->status.info==p->oldinfo) return;
	static NotifyNotification* notify_urgent=NULL, * notify_normal=NULL, * notify_low=NULL;
	if(!notify_urgent){
#if !defined(NOTIFY_CHECK_VERSION) || !NOTIFY_CHECK_VERSION(0,7,0)
	/*newer versions doesnot have _new_with_status_icon */
		notify_low=notify_notification_new_with_status_icon("Low", NULL, NULL, status_icon);
		notify_normal=notify_notification_new_with_status_icon("Normal", NULL, NULL, status_icon);
		notify_urgent=notify_notification_new_with_status_icon("Urgent", NULL, "error", status_icon);
#endif
		if(!notify_low){
			notify_low=notify_notification_new("Low", NULL, NULL);
			notify_normal=notify_notification_new("Normal", NULL, NULL);
			notify_urgent=notify_notification_new("Urgent", NULL, NULL);
		}
		notify_notification_set_icon_from_pixbuf(notify_low, icon_main);
		notify_notification_set_timeout(notify_low, NOTIFY_EXPIRES_DEFAULT);
		notify_notification_set_urgency(notify_low, NOTIFY_URGENCY_LOW);

		notify_notification_set_icon_from_pixbuf(notify_normal, icon_main);
		notify_notification_set_timeout(notify_normal, NOTIFY_EXPIRES_DEFAULT);
		notify_notification_set_urgency(notify_normal, NOTIFY_URGENCY_NORMAL);

		notify_notification_set_timeout(notify_urgent, NOTIFY_EXPIRES_NEVER);
		notify_notification_set_urgency(notify_urgent, NOTIFY_URGENCY_CRITICAL);

	}
	static char summary[80];
	NotifyNotification* notify;
	switch(p->status.info){
	case S_START:
		notify=notify_low;
		snprintf(summary, 80, "Job Started on %s", hosts[p->hid]);
		break;
	case S_FINISH:
		notify=notify_normal;
		snprintf(summary, 80, "Job Finished on %s", hosts[p->hid]);
		break;
	case S_CRASH:
		notify=notify_urgent;
		snprintf(summary, 80, "Job Crashed on %s!", hosts[p->hid]);
		break;
	case S_KILLED:
		notify=notify_urgent;
		snprintf(summary, 80, "Job is Killed on %s!", hosts[p->hid]);
		break;
	default:
		warning("Invalid status: %d\n", p->status.info);
		return;
	}
	notify_notification_update(notify, summary, p->path, NULL);
	notify_notification_show(notify, NULL);
	p->oldinfo=p->status.info;
#endif
}
/**
   quite the program
*/
static void quitmonitor(GtkWidget* widget, gpointer data){
	(void)widget;
	(void)data;
#if WITH_NOTIFY
	if(notify_daemon)
		notify_uninit();
#endif
	add_host_wrap(-2);
	if(gtk_main_level()>0){
		gtk_main_quit();
	}
	exit(0);
}
/**
   update the job count
*/
gboolean update_title(gpointer data){
	int id=GPOINTER_TO_INT(data);
	char tit[40];
	if(id<nhost){
		snprintf(tit, 40, "%s (%d)", hosts[id], nproc[id]);
	} else{
		snprintf(tit, 40, "%s", "All");
		gtk_label_set_attributes(GTK_LABEL(titles[id]), pango_active);
	}
	gtk_label_set_text(GTK_LABEL(titles[id]), tit);
	return 0;
}
/**
   respond to the kill job event
*/
void kill_job(proc_t* p){
	GtkWidget* dia=gtk_message_dialog_new
	(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION,
		GTK_BUTTONS_NONE,
		"Kill job %d on server %s?",
		p->pid, hosts[p->hid]);
	if(p->status.info==S_WAIT){
		gtk_dialog_add_buttons(GTK_DIALOG(dia), "Kill Job", 0, "Cancel", 2, NULL);
	} else{
		gtk_dialog_add_buttons(GTK_DIALOG(dia), "Kill Job", 0, "Remote Display", 1, "Cancel", 2, NULL);
	}
	int result=gtk_dialog_run(GTK_DIALOG(dia));
	gtk_widget_destroy(dia);
	switch(result){
	case 0:
		if(scheduler_cmd(p->hid, p->pid, CMD_KILL)){
			warning("Failed to kill the job\n");
		}
		p->status.info=S_TOKILL;
		refresh(p);
		break;
	case 1:
	{
		scheduler_display(p->hid, p->pid);
	}
	break;
	}
}
void kill_job_event(GtkWidget* btn, GdkEventButton* event, proc_t* p){
	(void)btn;
	if(event->button==1){
		kill_job(p);
	}
}
static void kill_all_jobs(GtkButton* btn, gpointer data){
	(void)btn;
	(void)data;
	int this_host=gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook))-1;

	GtkWidget* dia=gtk_message_dialog_new
	(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION,
		GTK_BUTTONS_NONE,
		"Kill all jobs on server %s or all servers?", hosts[this_host]);
	gtk_dialog_add_buttons(GTK_DIALOG(dia), "This server", 1, "All servers", 2, "Cancel", 0, NULL);
	int result=gtk_dialog_run(GTK_DIALOG(dia));
	gtk_widget_destroy(dia);
	if(result){
		for(int ihost=0; ihost<nhost; ihost++){
			if(result==1&&ihost!=this_host){
				continue;
			}
			for(proc_t* iproc=pproc[ihost]; iproc; iproc=iproc->next){
				if(iproc->hid==ihost&&(iproc->status.info<11)){
					if(scheduler_cmd(iproc->hid, iproc->pid, CMD_KILL)){
						warning("Failed to kill the job\n");
					}
					iproc->status.info=S_TOKILL;
					refresh(iproc);
				}
			}
		}
	}
}
#if GTK_MAJOR_VERSION < 3
static void status_icon_on_click(void* widget,
	gpointer data){
	(void)widget;
	(void)data;
	static int cx=0, cy=0;
	static int x=0, y=0;
	int force_show=GPOINTER_TO_INT(data);
	if(GTK_WIDGET_VISIBLE(window)&&!force_show){
		gtk_window_get_size(GTK_WINDOW(window), &x, &y);
		gtk_window_get_position(GTK_WINDOW(window), &cx, &cy);
		gtk_widget_hide(window);
	} else{
		if(x&&y){
			gtk_window_set_default_size(GTK_WINDOW(window), x, y);
			gtk_window_move(GTK_WINDOW(window), cx, cy);
		}
		gtk_widget_show(window);
		//gtk_window_deiconify(GTK_WINDOW(window));
	}
}
static void status_icon_on_popup(GtkStatusIcon* status_icon0, guint button,
	guint32 activate_time, gpointer user_data){
	(void)status_icon0;
	(void)button;
	(void)activate_time;
	/*gtk_menu_popup(GTK_MENU(user_data),NULL,NULL,
		   gtk_status_icon_position_menu,
		   status_icon0,button,activate_time);*/
	status_icon_on_click(NULL, user_data);
}
#endif
static void create_status_icon(){
#if GTK_MAJOR_VERSION < 3
	status_icon=gtk_status_icon_new_from_pixbuf(icon_main);
	//g_signal_connect(GTK_STATUS_ICON(status_icon),"activate", G_CALLBACK(status_icon_on_click),0);//useless.
	g_signal_connect(GTK_STATUS_ICON(status_icon), "popup-menu", G_CALLBACK(status_icon_on_popup), 0);
#endif
	/*GtkWidget *menu, *menuItemShow,*menuItemExit;
	menu=gtk_menu_new();
	menuItemShow=gtk_menu_item_new_with_label("Show/Hide");
	menuItemExit=gtk_menu_item_new_with_mnemonic("_Exit");
	g_signal_connect(G_OBJECT(menuItemShow),"activate",
			 G_CALLBACK(status_icon_on_click),NULL);
	g_signal_connect(G_OBJECT(menuItemExit),"activate",
			 G_CALLBACK(quitmonitor),NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),menuItemShow);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),menuItemExit);
	gtk_widget_show_all(menu);
	g_signal_connect(GTK_STATUS_ICON(status_icon),
			 "popup-menu",G_CALLBACK(trayIconPopup),menu);
#if GTK_MAJOR_VERSION >=3 || GTK_MINOR_VERSION>=16
	gtk_status_icon_set_tooltip_text(status_icon, "MAOS Job Monitoring");
#endif
	gtk_status_icon_set_visible(status_icon, TRUE);*/
}

/*
static gboolean delete_window(void){
	if(status_icon && gtk_status_icon_is_embedded(status_icon)){
	gtk_widget_hide(window);
	return TRUE;//do not quit
	}else{
		return FALSE;//quit.
	}
}*/
/*
static gboolean
window_state_event(GtkWidget *widget,GdkEventWindowState *event,gpointer data){
	(void)data;
	if(event->changed_mask == GDK_WINDOW_STATE_ICONIFIED
	   && (event->new_window_state == GDK_WINDOW_STATE_ICONIFIED
	   || event->new_window_state ==
	   (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_MAXIMIZED)))
	{
		gtk_widget_hide (GTK_WIDGET(widget));
	}
	return TRUE;
	}*/
static int test_jobs(int status, int flag){
	switch(flag){
	case 1://finished
		return status==S_FINISH;
		break;
	/*case 2://skipped
		return status==S_FINISH&&frac<1;
		break;*/
	case 3://crashed
		return status==S_CRASH||status==S_KILLED||status==S_TOKILL;
		break;
	case 4://all that is not running or pending
		return status==S_FINISH||status==S_CRASH||status==S_KILLED||status==S_TOKILL;
		break;
	default:
		return 0;
	}
}
static void clear_jobs(GtkButton* btn, gpointer flag){
	(void)btn;
	//int ihost=gtk_notebook_get_current_page (GTK_NOTEBOOK(notebook));
	for(int ihost=0; ihost<nhost; ihost++){
		if(!pproc[ihost]) continue;
		int sock=hsock[ihost];
		if(sock<0) continue;
		int cmd[2];
		cmd[0]=CMD_REMOVE;
		for(proc_t* iproc=pproc[ihost]; iproc; iproc=iproc->next){
			if(test_jobs(iproc->status.info, GPOINTER_TO_INT(flag))){
				cmd[1]=iproc->pid;
				if(stwriteintarr(sock, cmd, 2)){
					warning("write to socket %d failed\n", sock);
					break;
				}
			}
		}
	}
}

static void save_all_jobs(GtkButton* btn, gpointer data){
	(void)btn;
	(void)data;
	char* fnall=NULL;
	char* tm=strtime();
	for(int ihost=0; ihost<nhost; ihost++){
		if(!pproc[ihost]) continue;
		char fn[PATH_MAX];
		const char* host=hosts[ihost];
		FILE* fp[2];
		snprintf(fn, PATH_MAX, "%s/maos_%s_%s.done", HOME, host, tm);
		fp[0]=fopen(fn, "w");
		snprintf(fn, PATH_MAX, "%s/maos_%s_%s.wait", HOME, host, tm);
		fp[1]=fopen(fn, "w");

		if(fnall){
			fnall=stradd(fnall, "\n", fn, NULL);
		} else{
			fnall=strdup(fn);
		}
		char* lastpath[2]={NULL,NULL};
		for(proc_t* iproc=pproc[ihost]; iproc; iproc=iproc->next){
			char* spath=iproc->path;
			char* pos=NULL;
			int id;
			pos=strstr(spath, "/maos ");
			if(!pos){
				pos=strstr(spath, "/skyc ");
			}
			if(iproc->status.info>10){
				id=0;
			} else{
				id=1;
			}
			if(pos){
				pos[0]='\0';
				if(!lastpath[id]||strcmp(lastpath[id], spath)){//a different folder.
					free(lastpath[id]); lastpath[id]=strdup(spath);
					fprintf(fp[id], "cd %s\n", spath);
				}
				pos[0]='/';
				fprintf(fp[id], "%s\n", pos+1);
			} else{
				fprintf(fp[id], "%s\n", spath);
			}
		}
		fclose(fp[0]);
		fclose(fp[1]);
		free(lastpath[0]);
		free(lastpath[1]);
	}
	dialog_msg("Jobs saved to \n%s", fnall);
	free(fnall);
	free(tm);
}
GtkWidget* monitor_new_entry_progress(void){
	GtkWidget* prog=gtk_entry_new();
	gtk_editable_set_editable(GTK_EDITABLE(prog), FALSE);
	gtk_entry_set_width_chars(GTK_ENTRY(prog), 12);
#if GTK_MAJOR_VERSION>=3 || GTK_MINOR_VERSION >= 18
	gtk_widget_set_can_focus(prog, FALSE);
#else
	g_object_set(prog, "can-focus", FALSE, NULL);
#endif

	/*gtk_widget_modify_base(prog,GTK_STATE_NORMAL, &white);
	gtk_widget_modify_bg(prog,GTK_STATE_SELECTED, &blue);
	*/
#if GTK_MAJOR_VERSION==2 && GTK_MINOR_VERSION >= 10 || GTK_MAJOR_VERSION==3 && GTK_MINOR_VERSION < 4
	gtk_entry_set_inner_border(GTK_ENTRY(prog), 0);
#endif
	gtk_entry_set_has_frame(GTK_ENTRY(prog), 0);
	gtk_entry_set_alignment(GTK_ENTRY(prog), 0.5);
	return prog;
}
GtkWidget* monitor_new_progress(int vertical, int length){
	GtkWidget* prog=gtk_progress_bar_new();
	if(vertical){
#if GTK_MAJOR_VERSION>=3 
		gtk_orientable_set_orientation(GTK_ORIENTABLE(prog),
			GTK_ORIENTATION_VERTICAL);
		gtk_progress_bar_set_inverted(GTK_PROGRESS_BAR(prog), TRUE);

#else
		gtk_progress_bar_set_orientation(GTK_PROGRESS_BAR(prog),
			GTK_PROGRESS_BOTTOM_TO_TOP);
#endif
		gtk_widget_set_size_request(prog, 8, length);
		g_object_set(G_OBJECT(prog), "show-text", FALSE, NULL);
	} else{
		gtk_widget_set_size_request(prog, length, 12);
	}
	return prog;
}
/*
  Try to connect to hosts in response to button click.
 */
static void add_host_event(GtkButton* button, gpointer data){
	(void)button;
	int ihost=GPOINTER_TO_INT(data);
	if(ihost>-1&&ihost<nhost){
		add_host_wrap(ihost);//only this host
	} else{
		for(ihost=0; ihost<nhost; ihost++){
			add_host_wrap(ihost);//all host
		}
	}
}
static GtkToolItem* new_toolbar_item(const char* iconname, GdkPixbuf* iconbuf, const char* cmdname, void(*func)(GtkButton*, gpointer data), int data){
	GtkToolItem* item;
	GtkWidget* image;
	if(iconbuf){
		image=gtk_image_new_from_pixbuf(iconbuf);
	} else{
		image=gtk_image_new_from_icon_name(iconname, GTK_ICON_SIZE_SMALL_TOOLBAR);
	}
	item=gtk_tool_button_new(image, cmdname);
	gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(item), cmdname);
	g_signal_connect(item, "clicked", G_CALLBACK(func), GINT_TO_POINTER(data));
	return item;
}

int main(int argc, char* argv[]){
	if(0){
		char* fnlog=stradd(TEMP, "/monitor.log", NULL);
		//info("Check %s for log.\n", fnlog);
		if(!freopen(fnlog, "w", stdout)){
			warning("Unable to redirect output to %s\n", fnlog);
		} else{
			setbuf(stdout, NULL);
		}
		free(fnlog);
	}
#if GLIB_MAJOR_VERSION<3 && GLIB_MINOR_VERSION<32
	if(!g_thread_supported()){
		g_thread_init(NULL);
		gdk_threads_init();
	}
#endif
	gtk_init(&argc, &argv);
#if WITH_NOTIFY
	if(!notify_init("AOS Notification")){
		notify_daemon=0;
	}
#endif

	if(argc>1){
		for(int i=1; i<argc; i++){
			if(isdigit((int)argv[i][0])){
				PORT=strtol(argv[i], NULL, 10);
			} else if(isalnum((int)argv[i][0])){
				parse_host(argv[i]);
			}
		}
	} else if(nhost==1){
		info("Using '%s host1 host2' to monitor other machines, or put their hostnames in ~/.aos/hosts\n", argv[0]);
	}
#if GDK_MAJOR_VERSION < 3
	gdk_color_parse("#EE0000", &red);
	gdk_color_parse("#00CC00", &green);
	gdk_color_parse("#FFFF33", &yellow);
	gdk_color_parse("#FFFFFF", &white);
	gdk_color_parse("#0099FF", &blue);
	gdk_color_parse("#FFFFAB", &color_even);
	gdk_color_parse("#FFFFFF", &color_odd);
#endif
	icon_main=gdk_pixbuf_new_from_resource("/maos/icon-monitor.png", NULL);
	icon_finished=gdk_pixbuf_new_from_resource("/maos/icon-finished.png", NULL);
	icon_failed=gdk_pixbuf_new_from_resource("/maos/icon-error.png", NULL);
	icon_running=gdk_pixbuf_new_from_resource("/maos/icon-play.png", NULL);
	icon_skip=gdk_pixbuf_new_from_resource("/maos/icon-skip.png", NULL);
	icon_waiting=gdk_pixbuf_new_from_resource("/maos/icon-waiting.png", NULL);
	icon_cancel=gdk_pixbuf_new_from_resource("/maos/icon-cancel.png", NULL);
	icon_save=gdk_pixbuf_new_from_resource("/maos/icon-save.png", NULL);
	icon_clear=gdk_pixbuf_new_from_resource("/maos/icon-clear.png", NULL);
	icon_connect=gdk_pixbuf_new_from_resource("/maos/icon-connect.png", NULL);

	create_status_icon();
	window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "MAOS Monitor");
	gtk_window_set_icon(GTK_WINDOW(window), icon_main);

#if GTK_MAJOR_VERSION<3
	gtk_rc_parse_string(rc_string_widget);
	gtk_rc_parse_string(rc_string_treeview);
	gtk_rc_parse_string(rc_string_entry);
	//    GtkStyle *style=gtk_widget_get_style(window);
#else
	/*trough is the main background. progressbar is the sizable bar.*/

	const gchar* prog_blue=".progressbar{"
		"background-image:-gtk-gradient(linear,left bottom, right bottom, from (#0000FF), to (#0000FF));\n}";
	const gchar* prog_red=".progressbar{"
		"background-image:-gtk-gradient(linear,left bottom, right bottom, from (#FF0000), to (#FF0000));\n}";

		//    provider_prog=gtk_css_provider_new();
		//gtk_css_provider_load_from_data(provider_prog, prog_style, strlen(prog_style), NULL);
	provider_blue=gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider_blue, prog_blue, strlen(prog_blue), NULL);
	provider_red=gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider_red, prog_red, strlen(prog_red), NULL);
	/*Properties not belonging to GtkWidget need to begin with -WidgetClassName*/
	/*four sides: 4 values:top right bottom left;3 values:top horizontal bottom;2 values:vertical horizontal;1 value:all*/
	GtkCssProvider* provider_default=gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider_default, all_style, strlen(all_style), NULL);
	GtkStyleContext* all_context=gtk_widget_get_style_context(window);
	GdkScreen* screen=gtk_style_context_get_screen(all_context);
	gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider_default), GTK_STYLE_PROVIDER_PRIORITY_USER);

#endif

	GtkWidget* vbox=gtk_vbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER(window), vbox);
	{
		toptoolbar=gtk_toolbar_new();
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("computer", icon_connect, "Connect", add_host_event, -1), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), gtk_separator_tool_item_new(), -1);
		//gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("media-skip-forward", icon_skip, "Clear skipped jobs", clear_jobs, 2), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("object-select", icon_finished, "Clear finished jobs", clear_jobs, 1), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("dialog-error", icon_failed, "Clear crashed jobs", clear_jobs, 3), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("edit-clear-all", icon_clear, "Clear all jobs", clear_jobs, 4), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), gtk_separator_tool_item_new(), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("process-stop", icon_cancel, "Kill all jobs", kill_all_jobs, -1), -1);
		gtk_toolbar_insert(GTK_TOOLBAR(toptoolbar), new_toolbar_item("media-floppy", icon_save, "Save jobs to file", save_all_jobs, -1), -1);

		gtk_widget_show_all(toptoolbar);
		gtk_box_pack_start(GTK_BOX(vbox), toptoolbar, FALSE, FALSE, 0);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(toptoolbar), GTK_ICON_SIZE_MENU);
	}

	notebook=gtk_notebook_new();
	//gtk_widget_show(notebook);
	gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

	gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);


	//g_signal_connect(window, "delete_event", G_CALLBACK (delete_window), NULL);
	g_signal_connect(window, "destroy", G_CALLBACK(quitmonitor), NULL);
	//g_signal_connect(G_OBJECT (window), "window-state-event", G_CALLBACK (window_state_event), NULL);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);

	//tabs=mycalloc(nhost+1, GtkWidget*);
	pages=mycalloc(nhost+1, GtkWidget*);
	titles=mycalloc(nhost+1, GtkWidget*);
	pproc=mycalloc(nhost+1, proc_t*);
	nproc=mycalloc(nhost+1, int);
	cmdconnect=mycalloc(nhost+1, GtkWidget*);
	buffers=mycalloc(nhost+1, GtkTextBuffer*);
	hsock=mycalloc(nhost+1, int);
	for(int i=0; i<=nhost; i++){
		hsock[i]=-1;
	}
	usage_cpu=mycalloc(nhost, double);
	//usage_mem=mycalloc(nhost,double);
	usage_cpu2=mycalloc(nhost, double);
	//usage_mem2=mycalloc(nhost,double);
	prog_cpu=mycalloc(nhost, GtkWidget*);
	//prog_mem=mycalloc(nhost,GtkWidget *);

	pango_active=pango_attr_list_new();
	pango_down=pango_attr_list_new();
	pango_attr_list_insert(pango_down, pango_attr_foreground_new(0x88FF, 0x88FF, 0x88FF));
	pango_attr_list_insert(pango_active, pango_attr_foreground_new(0x0000, 0x0000, 0x0000));

	for(int ihost=0; ihost<=nhost; ihost++){//ihost==nhost is the include all tab
	//char tit[40];
	//snprintf(tit,40,"%s(0)",hosts[ihost]);
		titles[ihost]=gtk_label_new(NULL);
		gtk_label_set_attributes(GTK_LABEL(titles[ihost]), pango_down);
		GtkWidget* hbox0=gtk_hbox_new(FALSE, 0);
		if(ihost<nhost){
#if GTK_MAJOR_VERSION>=3
			prog_cpu[ihost]=monitor_new_progress(1, 4);
			//prog_mem[ihost]=monitor_new_progress(1,4);
#else
			prog_cpu[ihost]=monitor_new_progress(1, 16);
			//prog_mem[ihost]=monitor_new_progress(1,16);
#endif
			gtk_box_pack_start(GTK_BOX(hbox0), prog_cpu[ihost], FALSE, FALSE, 1);
			//gtk_box_pack_start(GTK_BOX(hbox0),prog_mem[ihost], FALSE, FALSE, 1);
			modify_bg(prog_cpu[ihost], 1);
			//modify_bg(prog_mem[ihost], 1);
		}
		gtk_box_pack_start(GTK_BOX(hbox0), titles[ihost], FALSE, TRUE, 0);
		gtk_widget_show_all(hbox0);
		GtkWidget* eventbox=gtk_event_box_new();
		gtk_container_add(GTK_CONTAINER(eventbox), hbox0);
		/*Put the box below an eventbox so that clicking on the progressbar
		  switches tab also.*/
		gtk_widget_show_all(eventbox);
		gtk_event_box_set_above_child(GTK_EVENT_BOX(eventbox), TRUE);
		gtk_event_box_set_visible_window(GTK_EVENT_BOX(eventbox), FALSE);
		pages[ihost]=gtk_vbox_new(FALSE, 0);
		if(ihost<nhost){
			cmdconnect[ihost]=gtk_button_new_with_label("Click to connect");
			g_signal_connect(cmdconnect[ihost], "clicked", G_CALLBACK(add_host_event), GINT_TO_POINTER(ihost));
			// button for reconnection
			GtkWidget* hbox=gtk_hbox_new(FALSE, 0);
			gtk_box_pack_start(GTK_BOX(hbox), cmdconnect[ihost], TRUE, FALSE, 0);
			gtk_box_pack_end(GTK_BOX(pages[ihost]), hbox, FALSE, FALSE, 0);
		}
		{//area for showing list of jobs
			GtkWidget* page=new_page(ihost);
			if(page){
				GtkWidget* scroll=gtk_scrolled_window_new(NULL, NULL);
				gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
				gtk_container_add(GTK_CONTAINER(scroll), page);//page is scrollable.
				gtk_box_pack_start(GTK_BOX(pages[ihost]), scroll, TRUE, TRUE, 0);
			}
		}
		{//text are to show details job information
			GtkWidget* seperator=gtk_hseparator_new();
			gtk_box_pack_start(GTK_BOX(pages[ihost]), seperator, FALSE, FALSE, 0);
			GtkWidget* view=gtk_text_view_new();
			buffers[ihost]=gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
			gtk_box_pack_start(GTK_BOX(pages[ihost]), view, FALSE, FALSE, 0);
			gtk_text_buffer_set_text(buffers[ihost], "", -1);
			gtk_text_view_set_editable(GTK_TEXT_VIEW(view), 0);
			gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
		}
		
		if(ihost<nhost){
			gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pages[ihost], eventbox);
		} else{
			gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), pages[ihost], eventbox, 0);
		}
		update_title(GINT_TO_POINTER(ihost));
		gtk_widget_show_all(pages[ihost]);
	}
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
	extern int sock_main[2];
	if(socketpair(AF_UNIX, SOCK_STREAM, 0, sock_main)){
		error("failed to create socketpair\n");
	}
	thread_new(listen_host, NULL);
	for(int ihost=0; ihost<nhost; ihost++){
		add_host_wrap(ihost);
	}
	gtk_widget_show_all(window);
#if MAC_INTEGRATION
	GtkosxApplication* theApp=g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
	gtkosx_application_set_dock_icon_pixbuf(theApp, icon_main);
	//g_signal_connect(theApp,"NSApplicationDidBecomeActive", G_CALLBACK(status_icon_on_click), GINT_TO_POINTER(1));//useless
	gtkosx_application_ready(theApp);
#endif
	gtk_main();
}
