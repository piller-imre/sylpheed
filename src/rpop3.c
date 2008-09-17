/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2008 Hiroyuki Yamamoto
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "defs.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtkwindow.h>
#include <gtk/gtkvbox.h>
#include <gtk/gtkhbox.h>
#include <gtk/gtklabel.h>
#include <gtk/gtkentry.h>
#include <gtk/gtkhbbox.h>
#include <gtk/gtkbutton.h>
#include <gtk/gtkstock.h>
#include <gtk/gtktreemodel.h>
#include <gtk/gtkliststore.h>
#include <gtk/gtktreeview.h>
#include <gtk/gtktreeselection.h>
#include <gtk/gtkcellrenderertext.h>
#include <string.h>

#include "rpop3.h"
#include "mainwindow.h"
#include "prefs_account.h"
#include "pop.h"
#include "procheader.h"
#include "procmsg.h"
#include "inc.h"
#include "utils.h"
#include "gtkutils.h"
#include "manage_window.h"
#include "alertpanel.h"

#define POP3_TOP	(N_POP3_STATE + 1000)
#define POP3_TOP_RECV	(N_POP3_STATE + 1001)
#define POP3_IDLE	(N_POP3_STATE + 1002)

enum
{
	COL_NUMBER,
	COL_SUBJECT,
	COL_FROM,
	COL_DATE,
	COL_SIZE,
	COL_MSGINFO,
	COL_DELETED,
	N_COLS
};

static struct RPop3Window {
	GtkWidget *window;

	GtkWidget *treeview;
	GtkListStore *store;

	GtkWidget *status_label;

	GtkWidget *open_btn;
	GtkWidget *delete_btn;
	GtkWidget *close_btn;

	Pop3Session *session;
	gboolean cancelled;
	gboolean finished;
	GArray *delete_array;
	gint delete_cur;
} rpop3_window;

static void rpop3_window_create	(PrefsAccount	*account);

gint pop3_greeting_recv          (Pop3Session *session,
                                  const gchar *msg);
gint pop3_getauth_user_send      (Pop3Session *session);
gint pop3_getauth_pass_send      (Pop3Session *session);
gint pop3_getauth_apop_send      (Pop3Session *session);
#if USE_SSL
gint pop3_stls_send              (Pop3Session *session);
gint pop3_stls_recv              (Pop3Session *session);
#endif
gint pop3_getrange_stat_send     (Pop3Session *session);
gint pop3_getrange_stat_recv     (Pop3Session *session,
                                  const gchar *msg);
gint pop3_getrange_last_send     (Pop3Session *session);
gint pop3_getrange_last_recv     (Pop3Session *session,
                                  const gchar *msg);
gint pop3_getrange_uidl_send     (Pop3Session *session);
gint pop3_getrange_uidl_recv     (Pop3Session *session,
                                  const gchar *data,
                                  guint        len);
gint pop3_getsize_list_send      (Pop3Session *session);
gint pop3_getsize_list_recv      (Pop3Session *session,
                                  const gchar *data,
                                  guint        len);
gint pop3_retr_send              (Pop3Session *session);
gint pop3_retr_recv              (Pop3Session *session,
                                  FILE        *fp,
                                  guint        len);
gint pop3_delete_send            (Pop3Session *session);
gint pop3_delete_recv            (Pop3Session *session);
gint pop3_logout_send            (Pop3Session *session);

void pop3_gen_send               (Pop3Session    *session,
                                  const gchar    *format, ...);

Pop3ErrorValue pop3_ok		(Pop3Session	*session,
				 const gchar	*msg);

static gint rpop3_start			(Session	*session);
static void rpop3_status_label_set	(const gchar	*fmt,
					 ...) G_GNUC_PRINTF(1, 2);
static void rpop3_clear_list		(void);

static gint rpop3_top_send	(Pop3Session	*session);
static gint rpop3_top_recv	(Pop3Session	*session,
				 FILE		*fp,
				 guint		 len);

static gint rpop3_session_recv_msg		(Session	*session,
						 const gchar	*msg);
static gint rpop3_session_recv_data_finished	(Session	*session,
						 guchar		*data,
						 guint		 len);
static gint rpop3_session_recv_data_as_file_finished
						(Session	*session,
						 FILE		*fp,
						 guint		 len);

static gint window_deleted	(GtkWidget	*widget,
				 GdkEventAny	*event,
				 gpointer	 data);
static gboolean key_pressed	(GtkWidget	*widget,
				 GdkEventKey	*event,
				 gpointer	 data);

static void rpop3_open		(GtkButton	*button,
				 gpointer	 data);
static void rpop3_delete	(GtkButton	*button,
				 gpointer	 data);
static void rpop3_close		(GtkButton	*button,
				 gpointer	 data);


gint rpop3_account(PrefsAccount *account)
{
	Session *session;
	gint ret;

	if (!account || account->protocol != A_POP3)
		return -1;
	if (inc_is_active())
		return -1;

	inc_lock();

	rpop3_window_create(account);

	session = pop3_session_new(account);
	rpop3_window.session = POP3_SESSION(session);
	rpop3_window.cancelled = FALSE;
	rpop3_window.finished = FALSE;

	/* override Pop3Session handlers */
	session->recv_msg = rpop3_session_recv_msg;
	session->send_data_finished = NULL;
	session->recv_data_finished = rpop3_session_recv_data_finished;
	session->recv_data_as_file_finished =
		rpop3_session_recv_data_as_file_finished;

	if (!POP3_SESSION(session)->pass) {
		gchar *pass;

		pass = input_query_password(account->recv_server,
					    account->userid);
		if (pass) {
			account->tmp_pass = g_strdup(pass);
			POP3_SESSION(session)->pass = pass;
		}
	}

	ret = rpop3_start(session);

	while (!rpop3_window.finished)
		gtk_main_iteration();

	session_destroy(session);
	rpop3_clear_list();
	gtk_widget_destroy(rpop3_window.window);
	memset(&rpop3_window, 0, sizeof(rpop3_window));

	inc_unlock();

	return ret;
}

static void rpop3_window_create(PrefsAccount *account)
{
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *scrwin;
	GtkWidget *treeview;
	GtkListStore *store;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeSelection *selection;
	GtkWidget *hbox;
	GtkWidget *status_label;
	GtkWidget *hbbox;
	GtkWidget *open_btn;
	GtkWidget *delete_btn;
	GtkWidget *close_btn;
	gchar buf[BUFFSIZE];

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_snprintf(buf, sizeof(buf), _("%s - Remote POP3 mailbox"),
		   account->account_name ? account->account_name : "");
	gtk_window_set_title(GTK_WINDOW(window), buf);
	gtk_widget_set_size_request(window, 640, -1);
	gtk_window_set_policy(GTK_WINDOW(window), FALSE, TRUE, TRUE);
	gtk_container_set_border_width(GTK_CONTAINER (window), 8);
	g_signal_connect(G_OBJECT(window), "delete_event",
			 G_CALLBACK(window_deleted), NULL);
	g_signal_connect(G_OBJECT(window), "key_press_event",
			 G_CALLBACK(key_pressed), NULL);
	MANAGE_WINDOW_SIGNALS_CONNECT(window);

	vbox = gtk_vbox_new(FALSE, 6);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	scrwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_box_pack_start(GTK_BOX(vbox), scrwin, TRUE, TRUE, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrwin),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scrwin, -1, 320);

	store = gtk_list_store_new(N_COLS, G_TYPE_INT, G_TYPE_STRING,
				   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				   G_TYPE_POINTER, G_TYPE_BOOLEAN);
	treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(treeview), TRUE);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

	gtk_container_add(GTK_CONTAINER(scrwin), treeview);

#define APPEND_COLUMN(label, col, width)                                \
{                                                                       \
        renderer = gtk_cell_renderer_text_new();                        \
        column = gtk_tree_view_column_new_with_attributes               \
                (label, renderer, "text", col,				\
		 "strikethrough", COL_DELETED, NULL);                   \
        gtk_tree_view_column_set_resizable(column, TRUE);               \
        if (width) {                                                    \
                gtk_tree_view_column_set_sizing                         \
                        (column, GTK_TREE_VIEW_COLUMN_FIXED);           \
                gtk_tree_view_column_set_fixed_width(column, width);    \
        }                                                               \
        gtk_tree_view_column_set_sort_column_id(column, col);           \
        gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), column);   \
}

        APPEND_COLUMN(_("No."), COL_NUMBER, 0);
        APPEND_COLUMN(_("Subject"), COL_SUBJECT, 200);
        APPEND_COLUMN(_("From"), COL_FROM, 160);
        APPEND_COLUMN(_("Date"), COL_DATE, 140);
        APPEND_COLUMN(_("Size"), COL_SIZE, 0);

	gtk_widget_show_all(scrwin);

	hbox = gtk_hbox_new(FALSE, 8);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	status_label = gtk_label_new("");
	gtk_box_pack_start(GTK_BOX(hbox), status_label, FALSE, FALSE, 0);

	hbbox = gtk_hbutton_box_new();
	gtk_button_box_set_layout(GTK_BUTTON_BOX(hbbox), GTK_BUTTONBOX_END);
	gtk_box_set_spacing(GTK_BOX(hbbox), 6);
	gtk_box_pack_end(GTK_BOX(vbox), hbbox, FALSE, FALSE, 0);

	open_btn = gtk_button_new_from_stock(GTK_STOCK_OPEN);
	gtk_box_pack_start(GTK_BOX(hbbox), open_btn, FALSE, FALSE, 0);
	gtk_widget_set_sensitive(open_btn, FALSE);

	delete_btn = gtk_button_new_from_stock(GTK_STOCK_DELETE);
	gtk_box_pack_start(GTK_BOX(hbbox), delete_btn, FALSE, FALSE, 0);
	gtk_widget_set_sensitive(delete_btn, FALSE);

	close_btn = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
	gtk_box_pack_start(GTK_BOX(hbbox), close_btn, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(open_btn), "clicked",
			 G_CALLBACK(rpop3_open), NULL);
	g_signal_connect(G_OBJECT(delete_btn), "clicked",
			 G_CALLBACK(rpop3_delete), NULL);
	g_signal_connect(G_OBJECT(close_btn), "clicked",
			 G_CALLBACK(rpop3_close), NULL);

	gtk_widget_show_all(window);

	rpop3_window.window = window;
	rpop3_window.treeview = treeview;
	rpop3_window.store = store;
	rpop3_window.status_label = status_label;
	rpop3_window.open_btn = open_btn;
	rpop3_window.delete_btn = delete_btn;
	rpop3_window.close_btn = close_btn;
}

static gint rpop3_start(Session *session)
{
	g_return_val_if_fail(session != NULL, -1);

	rpop3_status_label_set(_("Connecting to %s:%d"),
			       session->server, session->port);

	if (session_connect(session, session->server, session->port) < 0) {
		log_warning(_("Can't connect to POP3 server: %s:%d\n"),
			    session->server, session->port);
		return -1;
	}

	while (session_is_connected(session))
		gtk_main_iteration();

	return 0;
}

static void rpop3_status_label_set(const gchar *fmt, ...)
{
	va_list args;
	gchar buf[1024];

	va_start(args, fmt);
	g_vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	gtk_label_set_text(GTK_LABEL(rpop3_window.status_label), buf);
}

static gboolean clear_func(GtkTreeModel *model, GtkTreePath *path,
			   GtkTreeIter *iter, gpointer data)
{
	MsgInfo *msginfo;

	gtk_tree_model_get(model, iter, COL_MSGINFO, &msginfo, -1);
	procmsg_msginfo_free(msginfo);

	return FALSE;
}

static void rpop3_clear_list(void)
{
	
	gtk_tree_model_foreach(GTK_TREE_MODEL(rpop3_window.store), clear_func,
			       NULL);
	gtk_list_store_clear(rpop3_window.store);
}

static gint rpop3_top_send(Pop3Session *session)
{
	session->state = POP3_TOP;
	pop3_gen_send(session, "TOP %d 0", session->cur_msg);
	return PS_SUCCESS;
}

static gint rpop3_top_recv(Pop3Session *session, FILE *fp, guint len)
{
	MsgInfo *msginfo;
	MsgFlags flags = {0, 0};
	GtkTreeIter iter;
	const gchar *subject, *from, *date;
	gchar buf[1024];

	debug_print("rpop3_top_recv(): %d / %d (size %u)\n", session->cur_msg, session->count, len);

	msginfo = procheader_parse_stream(fp, flags, FALSE);

	subject = msginfo->subject ? msginfo->subject : _("(No Subject)");
	from = msginfo->from ? msginfo->from : _("(No From)");
	if (msginfo->date_t) {
		procheader_date_get_localtime(buf, sizeof(buf),
					      msginfo->date_t);
		date = buf;
	} else if (msginfo->date)
		date = msginfo->date;
	else
		date = _("(No Date)");

	gtk_list_store_append(rpop3_window.store, &iter);
	gtk_list_store_set(rpop3_window.store, &iter,
			   COL_NUMBER, session->cur_msg,
			   COL_SUBJECT, subject,
			   COL_FROM, from,
			   COL_DATE, date,
			   COL_SIZE, to_human_readable
				(session->msg[session->cur_msg].size),
			   COL_MSGINFO, msginfo,
			   COL_DELETED, FALSE,
			   -1);

	rpop3_status_label_set(_("Retrieving message headers (%d / %d)"),
			       session->cur_msg, session->count);

	return PS_SUCCESS;
}

static gint rpop3_delete_send(Pop3Session *session)
{
	g_return_val_if_fail(rpop3_window.delete_array != NULL, -1);
	g_return_val_if_fail
		(rpop3_window.delete_cur < rpop3_window.delete_array->len, -1);

	session->state = POP3_DELETE;
	session->cur_msg = g_array_index(rpop3_window.delete_array, gint,
					 rpop3_window.delete_cur);
	pop3_gen_send(session, "DELE %d", session->cur_msg);
	return PS_SUCCESS;
}

static gint rpop3_delete_recv(Pop3Session *session)
{
	session->msg[session->cur_msg].recv_time = RECV_TIME_DELETE;
	session->msg[session->cur_msg].deleted = TRUE;
	return PS_SUCCESS;
}

static gint rpop3_session_recv_msg(Session *session, const gchar *msg)
{
	Pop3Session *pop3_session = POP3_SESSION(session);
	gint val = PS_SUCCESS;
	const gchar *body;

	body = msg;
	if (pop3_session->state != POP3_GETRANGE_UIDL_RECV &&
	    pop3_session->state != POP3_GETSIZE_LIST_RECV) {
		val = pop3_ok(pop3_session, msg);
		if (val != PS_SUCCESS) {
			if (val != PS_NOTSUPPORTED) {
				pop3_session->state = POP3_ERROR;
				return -1;
			}
		}

		if (*body == '+' || *body == '-')
			body++;
		while (g_ascii_isalpha(*body))
			body++;
		while (g_ascii_isspace(*body))
			body++;
	}

	switch (pop3_session->state) {
	case POP3_READY:
	case POP3_GREETING:
		val = pop3_greeting_recv(pop3_session, body);
		rpop3_status_label_set(_("Authenticating..."));
#if USE_SSL
		if (pop3_session->ac_prefs->ssl_pop == SSL_STARTTLS)
			val = pop3_stls_send(pop3_session);
		else
#endif
		if (pop3_session->ac_prefs->use_apop_auth)
			val = pop3_getauth_apop_send(pop3_session);
		else
			val = pop3_getauth_user_send(pop3_session);
		break;
#if USE_SSL
        case POP3_STLS:
                if ((val = pop3_stls_recv(pop3_session)) != PS_SUCCESS)
                        return -1;
                if (pop3_session->ac_prefs->use_apop_auth)
                        val = pop3_getauth_apop_send(pop3_session);
                else
                        val = pop3_getauth_user_send(pop3_session);
                break;
#endif
	case POP3_GETAUTH_USER:
		val = pop3_getauth_pass_send(pop3_session);
		break;
	case POP3_GETAUTH_PASS:
	case POP3_GETAUTH_APOP:
		rpop3_status_label_set(_("Getting the number of messages..."));
		val = pop3_getrange_stat_send(pop3_session);
		break;
        case POP3_GETRANGE_STAT:
                if ((val = pop3_getrange_stat_recv(pop3_session, body)) < 0)
                        return -1;
                if (pop3_session->count > 0)
                        val = pop3_getrange_uidl_send(pop3_session);
                else {
			rpop3_status_label_set(_("No message"));
                        val = pop3_logout_send(pop3_session);
		}
                break;
        case POP3_GETRANGE_LAST:
                if (val == PS_NOTSUPPORTED)
                        pop3_session->error_val = PS_SUCCESS;
                else if ((val = pop3_getrange_last_recv
                                (pop3_session, body)) < 0)
                        return -1;
                if (pop3_session->cur_msg > 0)
                        val = pop3_getsize_list_send(pop3_session);
                else {
			rpop3_status_label_set(_("No message"));
                        val = pop3_logout_send(pop3_session);
		}
                break;
        case POP3_GETRANGE_UIDL:
                if (val == PS_NOTSUPPORTED) {
                        pop3_session->error_val = PS_SUCCESS;
                        val = pop3_getrange_last_send(pop3_session);
                } else {
                        pop3_session->state = POP3_GETRANGE_UIDL_RECV;
                        val = session_recv_data(session, 0, ".\r\n");
                }
                break;
        case POP3_GETSIZE_LIST:
                pop3_session->state = POP3_GETSIZE_LIST_RECV;
                val = session_recv_data(session, 0, ".\r\n");
                break;
	case POP3_TOP:
		pop3_session->state = POP3_TOP_RECV;
                val = session_recv_data_as_file(session, 0, ".\r\n");
		break;
        case POP3_RETR:
                pop3_session->state = POP3_RETR_RECV;
                val = session_recv_data_as_file(session, 0, ".\r\n");
                break;
        case POP3_DELETE:
                val = rpop3_delete_recv(pop3_session);
		if (val != PS_SUCCESS)
			break;
		if (rpop3_window.delete_cur + 1 < rpop3_window.delete_array->len) {
			rpop3_window.delete_cur++;
			val = rpop3_delete_send(pop3_session);
		} else {
			rpop3_status_label_set(_("Deleted %d messages"),
				 	       rpop3_window.delete_cur + 1);
			g_array_free(rpop3_window.delete_array, TRUE);
			rpop3_window.delete_array = NULL;
			rpop3_window.delete_cur = 0;
			pop3_session->state = POP3_IDLE;
		}
                break;
        case POP3_LOGOUT:
                pop3_session->state = POP3_DONE;
                session_disconnect(session);
                break;
	case POP3_ERROR:
	default:
		return -1;
	}

	if (val == PS_SUCCESS)
		return 0;
	else
		return -1;
}

static gint rpop3_session_recv_data_finished(Session *session, guchar *data,
					     guint len)
{
        Pop3Session *pop3_session = POP3_SESSION(session);
        Pop3ErrorValue val = PS_SUCCESS;

        switch (pop3_session->state) {
        case POP3_GETRANGE_UIDL_RECV:
                val = pop3_getrange_uidl_recv(pop3_session, (gchar *)data, len);
                if (val == PS_SUCCESS) {
			if (rpop3_window.cancelled)
				pop3_logout_send(rpop3_window.session);
			else
				pop3_getsize_list_send(pop3_session);
                } else
                        return -1;
                break;
        case POP3_GETSIZE_LIST_RECV:
                val = pop3_getsize_list_recv(pop3_session, (gchar *)data, len);
                if (val == PS_SUCCESS) {
			pop3_session->cur_msg = 1;
			if (rpop3_window.cancelled || pop3_session->count == 0)
				pop3_logout_send(rpop3_window.session);
			else
				rpop3_top_send(pop3_session);
		} else
                        return -1;
                break;
        case POP3_ERROR:
        default:
                return -1;
        }

        return 0;
}

static gint rpop3_session_recv_data_as_file_finished(Session *session,
						     FILE *fp, guint len)
{
        Pop3Session *pop3_session = POP3_SESSION(session);

        switch (pop3_session->state) {
	case POP3_RETR_RECV:
        	if (pop3_retr_recv(pop3_session, fp, len) < 0)
                	return -1;
		break;
	case POP3_TOP_RECV:
		if (rpop3_top_recv(pop3_session, fp, len) == PS_SUCCESS) {
			if (rpop3_window.cancelled)
				pop3_logout_send(rpop3_window.session);
			else if (pop3_session->cur_msg < pop3_session->count) {
				pop3_session->cur_msg++;
				rpop3_top_send(pop3_session);
			} else {
				rpop3_status_label_set
					(_("Retrieved %d message headers"),
					 pop3_session->count);
				gtk_widget_set_sensitive
					(rpop3_window.open_btn, TRUE);
				gtk_widget_set_sensitive
					(rpop3_window.delete_btn, TRUE);
				pop3_session->state = POP3_IDLE;
			}
		} else
			return -1;
		break;
	default:
		return -1;
	}

        return 0;
}

static gint window_deleted(GtkWidget *widget, GdkEventAny *event,
			   gpointer data)
{
	gtk_button_clicked(GTK_BUTTON(rpop3_window.close_btn));
	return TRUE;
}

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event,
			    gpointer data)
{
	return FALSE;
}

static void rpop3_open(GtkButton *button, gpointer data)
{
}

static void rpop3_delete(GtkButton *button, gpointer data)
{
	GtkTreeModel *model = GTK_TREE_MODEL(rpop3_window.store);
	GtkTreeSelection *selection;
	GtkTreeIter iter;
	GList *rows, *cur;
	gint num;
	gboolean deleted;
	GArray *array;
	AlertValue val;

	if (rpop3_window.session->state != POP3_IDLE)
		return;

	val = alertpanel(_("Delete messages"),
			 _("Really delete selected messages from server?\n"
			   "This operation cannot be reverted."),
			 GTK_STOCK_YES, GTK_STOCK_NO, NULL);
	if (val != G_ALERTDEFAULT)
		return;

	selection = gtk_tree_view_get_selection
		(GTK_TREE_VIEW(rpop3_window.treeview));

	rows = gtk_tree_selection_get_selected_rows(selection, NULL);

	array = g_array_sized_new(FALSE, FALSE, sizeof(gint),
				  g_list_length(rows));

	for (cur = rows; cur != NULL; cur = cur->next) {
		gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)cur->data);
		gtk_tree_model_get(model, &iter, COL_NUMBER, &num,
				   COL_DELETED, &deleted, -1);
		if (!deleted) {
			debug_print("rpop3_delete: marked %d to delete\n", num);
			g_array_append_val(array, num);
			gtk_list_store_set(GTK_LIST_STORE(model), &iter,
					   COL_DELETED, TRUE, -1);
		}
	}

	g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(rows);

	if (array->len > 0) {
		rpop3_window.delete_array = array;
		rpop3_window.delete_cur = 0;

		rpop3_delete_send(rpop3_window.session);
	} else
		g_array_free(array, TRUE);
}

static void rpop3_close(GtkButton *button, gpointer data)
{
	rpop3_window.finished = TRUE;

	if (rpop3_window.session->state == POP3_IDLE)
		pop3_logout_send(rpop3_window.session);
	else if (rpop3_window.session->state != POP3_DONE ||
		 rpop3_window.session->state != POP3_ERROR)
		rpop3_window.cancelled = TRUE;
}
