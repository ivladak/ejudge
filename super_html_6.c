/* -*- mode: c -*- */
/* $Id$ */

/* Copyright (C) 2011 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"
#include "version.h"
#include "ej_limits.h"

#include "super_html.h"
#include "super-serve.h"
#include "super_proto.h"
#include "mischtml.h"
#include "userlist_proto.h"
#include "userlist_clnt.h"
#include "userlist.h"
#include "misctext.h"
#include "errlog.h"
#include "xml_utils.h"
#include "ejudge_cfg.h"
#include "super_html_6.h"
#include "super_html_6_meta.h"
#include "meta_generic.h"
#include "charsets.h"
#include "csv.h"
#include "bitset.h"

#include "reuse_xalloc.h"

#include <stdarg.h>
#include <printf.h>
#include <limits.h>

#define ARMOR(s)  html_armor_buf(&ab, (s))
#define FAIL(c) do { retval = -(c); goto cleanup; } while (0)

#define FIRST_COOKIE(u) ((struct userlist_cookie*) (u)->cookies->first_down)
#define NEXT_COOKIE(c)  ((struct userlist_cookie*) (c)->b.right)
#define FIRST_CONTEST(u) ((struct userlist_contest*)(u)->contests->first_down)
#define NEXT_CONTEST(c)  ((struct userlist_contest*)(c)->b.right)

void
super_html_6_force_link()
{
}

unsigned char *
ss_url_unescaped(
        unsigned char *buf,
        size_t size,
        const struct super_http_request_info *phr,
        int action,
        int op,
        const char *format,
        ...)
{
  unsigned char fbuf[1024];
  unsigned char abuf[64];
  unsigned char obuf[64];
  const unsigned char *sep = "";
  va_list args;

  fbuf[0] = 0;
  if (format && *format) {
    va_start(args, format);
    vsnprintf(fbuf, sizeof(fbuf), format, args);
    va_end(args);
  }
  if (fbuf[0]) sep = "&";

  abuf[0] = 0;
  if (action > 0) snprintf(abuf, sizeof(abuf), "&action=%d", action);
  obuf[0] = 0;
  if (op > 0) snprintf(obuf, sizeof(obuf), "&op=%d", op);

  snprintf(buf, size, "%s?SID=%016llx%s%s%s%s", phr->self_url,
           phr->session_id, abuf, obuf, sep, fbuf);
  return buf;
}

void
ss_redirect(
        FILE *fout,
        struct super_http_request_info *phr,
        int new_op,
        const unsigned char *extra)
{
  unsigned char url[1024];

  if (extra && *extra) {
    ss_url_unescaped(url, sizeof(url), phr, SSERV_CMD_HTTP_REQUEST, new_op, "%s", extra);
  } else {
    ss_url_unescaped(url, sizeof(url), phr, SSERV_CMD_HTTP_REQUEST, new_op, 0);
  }

  fprintf(fout, "Content-Type: text/html; charset=%s\nCache-Control: no-cache\nPragma: no-cache\nLocation: %s\n\n", EJUDGE_CHARSET, url);
}

void
ss_redirect_2(
        FILE *fout,
        struct super_http_request_info *phr,
        int new_op,
        int contest_id,
        int group_id,
        int other_user_id,
        const unsigned char *marked_str)
{
  unsigned char url[1024];
  char *o_str = 0;
  size_t o_len = 0;
  FILE *o_out = 0;

  o_out = open_memstream(&o_str, &o_len);
  if (contest_id > 0) {
    fprintf(o_out, "&contest_id=%d", contest_id);
  }
  if (group_id > 0) {
    fprintf(o_out, "&group_id=%d", group_id);
  }
  if (other_user_id > 0) {
    fprintf(o_out, "&other_user_id=%d", other_user_id);
  }
  if (marked_str && *marked_str) {
    fprintf(o_out, "&marked=%s", marked_str);
  }
  fclose(o_out); o_out = 0;

  if (o_str && *o_str) {
    ss_url_unescaped(url, sizeof(url), phr, SSERV_CMD_HTTP_REQUEST, new_op, "%s", o_str);
  } else {
    ss_url_unescaped(url, sizeof(url), phr, SSERV_CMD_HTTP_REQUEST, new_op, 0);
  }

  xfree(o_str); o_str = 0; o_len = 0;

  fprintf(fout, "Content-Type: text/html; charset=%s\nCache-Control: no-cache\nPragma: no-cache\nLocation: %s\n\n", EJUDGE_CHARSET, url);
}

static unsigned char *
fix_string(const unsigned char *s)
{
  if (!s) return NULL;

  int len = strlen(s);
  if (len < 0) return NULL;

  while (len > 0 && (s[len - 1] <= ' ' || s[len - 1] == 127)) --len;
  if (len <= 0) return xstrdup("");

  int i = 0;
  while (i < len && (s[i] <= ' ' || s[i] == 127)) ++i;
  if (i >= len) return xstrdup("");

  unsigned char *out = (unsigned char *) xmalloc(len + 1);
  int j = 0;
  for (; i < len; ++i, ++j) {
    if (s[i] <= ' ' || s[i] == 127) {
      out[j] = ' ';
    } else {
      out[j] = s[i];
    }
  }
  out[j] = 0;

  return out;
}

static void
ss_select(
        FILE *fout,
        const unsigned char *param,
        const unsigned char **options,
        int value)
{
  int option_count = 0, i;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *s;

  if (!options) return;
  for (; options[option_count]; ++option_count);
  if (option_count <= 0) return;

  if (value < 0 || value >= option_count) value = 0;

  fprintf(fout, "<select name=\"%s\">", param);
  for (i = 0; i < option_count; ++i) {
    s = "";
    if (i == value) s = " selected=\"selected\"";
    fprintf(fout, "<option value=\"%d\"%s>%s</option>",
            i, s, ARMOR(options[i]));
  }
  fprintf(fout, "</select>");
  html_armor_free(&ab);
}

static int
get_global_caps(const struct super_http_request_info *phr, opcap_t *pcap)
{
  return opcaps_find(&phr->config->capabilities, phr->login, pcap);
}
static int
get_contest_caps(const struct super_http_request_info *phr, const struct contest_desc *cnts, opcap_t *pcap)
{
  return opcaps_find(&cnts->capabilities, phr->login, pcap);
}

static int
is_globally_privileged(const struct super_http_request_info *phr, const struct userlist_user *u)
{
  opcap_t caps = 0;
  if (u->is_privileged) return 1;
  if (opcaps_find(&phr->config->capabilities, u->login, &caps) >= 0) return 1;
  return 0;
}
static int
is_contest_privileged(
        const struct contest_desc *cnts,
        const struct userlist_user *u)
{
  opcap_t caps = 0;
  if (opcaps_find(&cnts->capabilities, u->login, &caps) >= 0) return 1;
  return 0;
}
static int
is_privileged(
        const struct super_http_request_info *phr,
        const struct contest_desc *cnts,
        const struct userlist_user *u)
{
  opcap_t caps = 0;
  if (u->is_privileged) return 1;
  if (opcaps_find(&phr->config->capabilities, u->login, &caps) >= 0) return 1;
  if (!cnts) return 0;
  if (opcaps_find(&cnts->capabilities, u->login, &caps) >= 0) return 1;
  return 0;
}

static int
ss_parse_params(
        struct super_http_request_info *phr,
        const struct meta_methods *mth,
        void *params)
{
  int field_id, field_type;
  void *field_ptr;
  const unsigned char *field_name;

  for (field_id = 1; field_id < mth->last_tag; ++field_id) {
    field_type = mth->get_type(field_id);
    field_ptr = mth->get_ptr_nc(params, field_id);
    field_name = mth->get_name(field_id);
    if (!field_ptr || !field_name) continue;
    switch (field_type) {
    case '0':                   /* ej_int_opt_0_t */
      ss_cgi_param_int_opt(phr, field_name, (int*) field_ptr, 0);
      break;
    case '1':                   /* ej_textbox_t */
      {
        const unsigned char *s = 0;
        if (ss_cgi_param(phr, field_name, &s) <= 0 || !s || !*s) {
          return -S_ERR_INV_VALUE;
        }
        unsigned char *s2 = fix_string(s);
        if (!s2 || !*s2) {
          xfree(s2);
          return -S_ERR_INV_VALUE;
        }
        *(unsigned char **) field_ptr = s2;
      }
      break;
    case '2':                   /* ej_textbox_opt_t */
      {
        const unsigned char *s = 0;
        if (ss_cgi_param(phr, field_name, &s) < 0) {
          return -S_ERR_INV_VALUE;
        }
        unsigned char *s2 = fix_string(s);
        if (!s2) s2 = xstrdup("");
        *(unsigned char **) field_ptr = s2;
      }
      break;
    case '3':                   /* ej_checkbox_t */
      {
        int *ip = (int*) field_ptr;
        ss_cgi_param_int_opt(phr, field_name, ip, 0);
        if (*ip != 1) *ip = 0;
      }
      break;
    case '4':                   /* ej_int_opt_1_t */
      ss_cgi_param_int_opt(phr, field_name, (int*) field_ptr, 1);
      break;
    case '5':                   /* ej_int_opt_m1_t */
      ss_cgi_param_int_opt(phr, field_name, (int*) field_ptr, -1);
      break;
    default:
      abort();
    }
  }

  return 0;
}

static int
userlist_user_count_contests(struct userlist_user *u)
{
  struct userlist_contest *c;
  int tot = 0;

  if (!u || !u->contests) return 0;
  for (c = FIRST_CONTEST(u); c; c = NEXT_CONTEST(c), tot++);
  return tot;
}
static int
userlist_user_count_cookies(struct userlist_user *u)
{
  struct userlist_cookie *cookie;
  int tot = 0;

  if (!u) return 0;
  if (!u->cookies) return 0;
  for (cookie = FIRST_COOKIE(u); cookie; cookie = NEXT_COOKIE(cookie), tot++);
  return tot;
}

static struct userlist_user *
get_user_info(
        struct super_http_request_info *phr,
        int user_id,
        int contest_id)
{
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;

  if (userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             user_id, contest_id, &xml_text) < 0 || !xml_text) {
    return NULL;
  }
  u = userlist_parse_user_str(xml_text);
  xfree(xml_text);
  return u;
}

static void
print_top_navigation_links(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr,
        int contest_id,
        int group_id,
        int other_user_id,
        const unsigned char *marked_str)
{
  unsigned char hbuf[1024];
  unsigned char contest_id_str[1024];
  unsigned char group_id_str[1024];
  unsigned char *marked_param = "";

  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }
  if (marked_str && *marked_str) {
    int len = strlen(marked_str);
    if (len < 100000) {
      marked_param = alloca(len + 32);
      sprintf(marked_param, "&amp;marked=%s", marked_str);
    }
  }

  fprintf(out_f, "<ul>");
  fprintf(out_f, "<li>%s%s</a></li>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, NULL),
          "Main page");
  fprintf(out_f, "<li>%s%s</a></li>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_BROWSE_PAGE, marked_param),
          "Browse users");
  fprintf(out_f, "<li>%s%s</a></li>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_GROUP_BROWSE_PAGE),
          "Browse groups");
  if (contest_id > 0) {
    fprintf(out_f, "<li>%s%s %d</a></li>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_BROWSE_PAGE,
                          contest_id_str, marked_param),
            "Browse users of contest", contest_id);
  }
  if (group_id > 0) {
    fprintf(out_f, "<li>%s%s %d</a></li>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_BROWSE_PAGE,
                          group_id_str, marked_param),
            "Browse users of group", group_id);
  }
  if (other_user_id > 0) {
    fprintf(out_f, "<li>%s%s</a></li>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DETAIL_PAGE,
                          other_user_id,
                          contest_id_str, group_id_str),
            "User details");
  }
  fprintf(out_f, "</ul>\n");
}

static const unsigned char * const reg_status_strs[] =
{
  "<font color=\"green\">OK</font>",
  "<font color=\"magenta\">Pending</font>",
  "<font color=\"red\">Rejected</font>",
  "<font color=\"red\"><b>Invalid status</b></font>",
};
static const unsigned char * const flag_op_legends[] =
{
  "Do nothing", "Clear", "Set", "Toggle", NULL,
};

int
super_serve_op_USER_BROWSE_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  unsigned char buf[1024];
  unsigned char hbuf[1024];
  unsigned char *xml_text = 0;
  const unsigned char *user_filter = 0;
  int user_offset = 0;
  int user_count = 20;
  const unsigned char *s;
  struct userlist_list *users = 0;
  int user_id, serial;
  const struct userlist_user *u;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *cl;
  int contest_id = 0, group_id = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  const struct contest_desc *cnts = 0;
  opcap_t gcaps = 0;
  opcap_t caps = 0;
  const struct userlist_contest *reg = 0;
  int min_user_id = INT_MAX;
  int max_user_id = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  s = 0;
  if (ss_cgi_param(phr, "marked", &s) > 0 && s) {
    if (bitset_url_decode(s, &marked) >= 0) {
      marked_str = bitset_url_encode(&marked);
    }
  }

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }

  if (get_global_caps(phr, &gcaps) >= 0 && opcaps_check(gcaps, OPCAP_LIST_USERS) >= 0) {
    // this user can view the full user list and the user list for any contest
  } else if (!cnts) {
    // user without global OPCAP_LIST_USERS capability cannot view the full user list
    FAIL(-S_ERR_PERM_DENIED);
  } else if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_LIST_USERS) < 0) {
    FAIL(-S_ERR_PERM_DENIED);
  }

  hbuf[0] = 0;
  if (contest_id > 0 && group_id > 0) {
    snprintf(hbuf, sizeof(hbuf), " for contest %d, group %d", contest_id, group_id);
  } else if (contest_id > 0) {
    snprintf(hbuf, sizeof(hbuf), " for contest %d", contest_id);
  } else if (group_id > 0) {
    snprintf(hbuf, sizeof(hbuf), " for group %d", group_id);
  }
  snprintf(buf, sizeof(buf), "serve-control: %s, browsing users%s",
           phr->html_name, hbuf);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function setAllCheckboxes(value)\n"
          "{\n"
          "  objs = document.forms[1].elements;\n"
          "  if (objs != null) {\n"
          "    for (var i = 0; i < objs.length; ++i) {\n"
          "      if (objs[i].type == \"checkbox\") {\n"
          "        objs[i].checked = value;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function toggleAllCheckboxes()\n"
          "{\n"
          "  objs = document.forms[1].elements;\n"
          "  if (objs != null) {\n"
          "    for (var i = 0; i < objs.length; ++i) {\n"
          "      if (objs[i].type == \"checkbox\") {\n"
          "        objs[i].checked = !objs[i].checked;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function setOperationVisibility(oper, value)\n"
          "{\n"
          "  obj1 = document.getElementById(\"Show\" + oper + \"Menu\");\n"
          "  obj2 = document.getElementById(\"Hide\" + oper + \"Menu\");\n"
          "  if (value) {\n"
          "    obj1.style.display = \"none\";\n"
          "    obj2.style.display = \"\";\n"
          "  } else {\n"
          "    obj1.style.display = \"\";\n"
          "    obj2.style.display = \"none\";\n"
          "  }\n"
          "}\n"
          "");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n", buf);

  if (cnts) {
    fprintf(out_f, "<h2>Contest %d: %s</h2>", cnts->id, ARMOR(cnts->name));
  } else {
    fprintf(out_f, "<br/>\n");
  }

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, marked_str);

  if (!phr->userlist_clnt) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No connection to the server!</pre>\n");
    goto do_footer;
  }

  if (phr->ss->user_filter_set) {
    user_filter = phr->ss->user_filter;
    user_offset = phr->ss->user_offset;
    user_count = phr->ss->user_count;
  }

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  if (marked_str && marked_str) {
    html_hidden(out_f, "marked", "%s", marked_str);
  }
  fprintf(out_f, "<table class=\"b0\">");
  s = user_filter;
  if (!s) s = "";
  fprintf(out_f, "<!--<tr><td class=\"b0\">Filter:</td><td class=\"b0\">%s</td></tr>-->",
          html_input_text(buf, sizeof(buf), "user_filter", 50, "%s", ARMOR(s)));
  hbuf[0] = 0;
  if (phr->ss->user_filter_set) {
    snprintf(hbuf, sizeof(hbuf), "%d", user_offset);
  }
  fprintf(out_f, "<tr><td class=\"b0\">Offset:</td><td class=\"b0\">%s</td></tr>",
          html_input_text(buf, sizeof(buf), "user_offset", 10, "%s", hbuf));
  hbuf[0] = 0;
  if (phr->ss->user_filter_set) {
    snprintf(hbuf, sizeof(hbuf), "%d", user_count);
  }
  fprintf(out_f, "<tr><td class=\"b0\">Count:</td><td class=\"b0\">%s</td></tr>",
          html_input_text(buf, sizeof(buf), "user_count", 10, "%s", hbuf));
  fprintf(out_f, "<tr><td class=\"b0\">&nbsp;</td><td class=\"b0\"><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td></tr>",
          SSERV_OP_USER_FILTER_CHANGE_ACTION, "Change");
  fprintf(out_f, "</table>");
  fprintf(out_f, "<table class=\"b0\"><tr>");
  fprintf(out_f, "<td class=\"b0\"><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>", SSERV_OP_USER_FILTER_FIRST_PAGE_ACTION, "&lt;&lt;");
  fprintf(out_f, "<td class=\"b0\"><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>", SSERV_OP_USER_FILTER_PREV_PAGE_ACTION, "&lt;");
  fprintf(out_f, "<td class=\"b0\"><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>", SSERV_OP_USER_FILTER_NEXT_PAGE_ACTION, "&gt;");
  fprintf(out_f, "<td class=\"b0\"><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>", SSERV_OP_USER_FILTER_LAST_PAGE_ACTION, "&gt;&gt;");
  fprintf(out_f, "</tr></table>\n");
  //fprintf(out_f, "</form>\n");

  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s><tr>", cl);
  fprintf(out_f, "<td%s><b>%s:</b></td>", cl, "Jump to contest");
  hbuf[0] = 0;
  if (contest_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", contest_id);
  }
  fprintf(out_f, "<td%s>%s</td>", cl,
          html_input_text(buf, sizeof(buf), "jump_contest_id", 10, "%s", hbuf));
  fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
          cl, SSERV_OP_USER_JUMP_CONTEST_ACTION, "Jump");
  fprintf(out_f, "</tr></table>\n");

  r = userlist_clnt_list_users_2(phr->userlist_clnt, ULS_LIST_ALL_USERS_2,
                                 contest_id, group_id, user_filter, user_offset, user_count,
                                 &xml_text);
  if (r < 0) {
    fprintf(out_f, "</form>\n");
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>Cannot get user list: %s</pre>\n",
            userlist_strerror(-r));
    goto do_footer;
  }
  users = userlist_parse_str(xml_text);
  if (!users) {
    fprintf(out_f, "</form>\n");
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>XML parse error</pre>\n");
    goto do_footer;
  }

  for (user_id = 1; user_id < users->user_map_size; ++user_id) {
    if (!(u = users->user_map[user_id])) continue;
    if (user_id >= max_user_id) max_user_id = user_id;
    if (user_id <= min_user_id) min_user_id = user_id;
  }

  //html_start_form(out_f, 1, phr->self_url, "");
  /*
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  */
  html_hidden(out_f, "min_user_id", "%d", min_user_id);
  html_hidden(out_f, "max_user_id", "%d", max_user_id);
  cl = " class=\"b1\"";
  fprintf(out_f, "<table%s>\n", cl);

  fprintf(out_f, "<tr>");
  fprintf(out_f, "<th%s>&nbsp;</th>", cl);
  fprintf(out_f, "<th%s>NN</th>", cl);
  fprintf(out_f, "<th%s>User Id</th>", cl);
  fprintf(out_f, "<th%s>User Login</th>", cl);
  fprintf(out_f, "<th%s>E-mail</th>", cl);
  fprintf(out_f, "<th%s>Name</th>", cl);
  //fprintf(out_f, "<th%s>Flags</th>", cl);
  if (contest_id > 0) {
    fprintf(out_f, "<th%s>Status</th>", cl);
    fprintf(out_f, "<th%s>Flags</th>", cl);
  }
  fprintf(out_f, "<th%s>Operations</th>", cl);
  fprintf(out_f, "</tr>\n");

  serial = user_offset - 1;
  for (user_id = 1; user_id < users->user_map_size; ++user_id) {
    if (!(u = users->user_map[user_id])) continue;
    reg = 0;
    if (contest_id > 0) {
      reg = userlist_get_user_contest(u, contest_id);
    }

    ++serial;
    fprintf(out_f, "<tr>\n");
    s = "";
    if (bitset_safe_get(&marked, user_id)) {
      s = " checked=\"checked\"";
    }
    fprintf(out_f, "<td class=\"b1\"><input type=\"checkbox\" name=\"user_%d\" value=\"1\"%s/></td>", user_id, s);
    fprintf(out_f, "<td class=\"b1\">%d</td>", serial);
    fprintf(out_f, "<td class=\"b1\">%d</td>", user_id);
    if (!u->login) {
      fprintf(out_f, "<td class=\"b1\"><i>NULL</i></td>");
    } else {
      fprintf(out_f, "<td class=\"b1\"><tt>%s</tt></td>", ARMOR(u->login));
    }
    if (!u->email) {
      fprintf(out_f, "<td class=\"b1\"><i>NULL</i></td>");
    } else {
      fprintf(out_f, "<td class=\"b1\"><tt>%s</tt></td>", ARMOR(u->email));
    }
    if (!u->cnts0 || !u->cnts0->name) {
      fprintf(out_f, "<td class=\"b1\"><i>NULL</i></td>");
    } else {
      fprintf(out_f, "<td class=\"b1\"><tt>%s</tt></td>", ARMOR(u->cnts0->name));
    }
    /*
    int flags_count = 0;
    fprintf(out_f, "<td class=\"b1\">");
    if (u->is_privileged) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "privileged");
      ++flags_count;
    }
    if (u->is_invisible) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "invisible");
      ++flags_count;
    }
    if (u->is_banned) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "banned");
      ++flags_count;
    }
    if (u->is_locked) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "locked");
      ++flags_count;
    }
    if (u->show_login) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "show_login");
      ++flags_count;
    }
    if (u->show_email) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "show_email");
      ++flags_count;
    }
    if (u->read_only) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "read_only");
      ++flags_count;
    }
    if (u->never_clean) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "never_clean");
      ++flags_count;
    }
    if (u->simple_registration) {
      if (flags_count > 0) fprintf(out_f, ", ");
      fprintf(out_f, "simple_reg");
      ++flags_count;
    }
    if (!flags_count) {
      fprintf(out_f, "&nbsp;");
    }
    fprintf(out_f, "</td>");
    */

    if (contest_id > 0) {
      if (reg) {
        r = reg->status;
        if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_LAST;
        fprintf(out_f, "<td%s>%s</td>", cl, reg_status_strs[r]);
        fprintf(out_f, "<td%s>", cl);
        r = 0;
        if ((reg->flags & USERLIST_UC_INVISIBLE)) {
          if (r++) fprintf(out_f, ", ");
          fprintf(out_f, "invisible");
        }
        if ((reg->flags & USERLIST_UC_BANNED)) {
          if (r++) fprintf(out_f, ", ");
          fprintf(out_f, "banned");
        }
        if ((reg->flags & USERLIST_UC_LOCKED)) {
          if (r++) fprintf(out_f, ", ");
          fprintf(out_f, "locked");
        }
        if ((reg->flags & USERLIST_UC_INCOMPLETE)) {
          if (r++) fprintf(out_f, ", ");
          fprintf(out_f, "incomplete");
        }
        if ((reg->flags & USERLIST_UC_DISQUALIFIED)) {
          if (r++) fprintf(out_f, ", ");
          fprintf(out_f, "disqualified");
        }
        fprintf(out_f, "</td>");
      } else {
        fprintf(out_f, "<td%s>&nbsp;</td><td%s>&nbsp;</td>", cl, cl);
      }
    }

    fprintf(out_f, "<td%s>", cl);
    fprintf(out_f, "%s%s</a>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DETAIL_PAGE,
                          user_id, contest_id_str, group_id_str),
            "[Details]");
    fprintf(out_f, "&nbsp;%s%s</a>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_PASSWORD_PAGE,
                          SSERV_OP_USER_BROWSE_PAGE,
                          user_id, contest_id_str, group_id_str),
            "[Reg. password]");
    if (contest_id > 0 && cnts && !cnts->disable_team_password) {
      fprintf(out_f, "&nbsp;%s%s</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CNTS_PASSWORD_PAGE,
                            SSERV_OP_USER_BROWSE_PAGE,
                            user_id, contest_id_str, group_id_str),
              "[Cnts. password]");
    }
    if (contest_id > 0) {
      fprintf(out_f, "&nbsp;%s[%s]</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_EDIT_REG_PAGE,
                            SSERV_OP_USER_BROWSE_PAGE,
                            user_id, contest_id, contest_id_str, group_id_str),
              "Change");
      fprintf(out_f, "&nbsp;%s[%s]</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_REG_PAGE,
                            SSERV_OP_USER_BROWSE_PAGE,
                            user_id, contest_id, contest_id_str, group_id_str),
              "Delete");
    }
    fprintf(out_f, "</td>");
    fprintf(out_f, "</tr>\n");
  }
  fprintf(out_f, "</table>\n");

  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s><tr>", cl);
  if (cnts) {
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_BROWSE_MARK_ALL_ACTION, "Mark all");
  }
  fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
          cl, SSERV_OP_USER_BROWSE_UNMARK_ALL_ACTION, "Unmark all");
  if (cnts) {
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_BROWSE_TOGGLE_ALL_ACTION, "Toggle all");
  }
  fprintf(out_f, "</tr></table>\n");

  fprintf(out_f, "<div id=\"ShowRegistrationMenu\">");
  fprintf(out_f, "<a onclick=\"setOperationVisibility('Registration', true)\">[%s]</a>\n",
          "Registration operations");
  fprintf(out_f, "</div>");
  fprintf(out_f, "<div style=\"display: none;\" id=\"HideRegistrationMenu\">");
  fprintf(out_f, "<a onclick=\"setOperationVisibility('Registration', false)\">[%s]</a><br/>\n",
          "Hide registration operations");
  fprintf(out_f, "<table%s>", cl);
  fprintf(out_f, "<tr><td%s colspan=\"2\"><b>Contest ID:</b> <input type=\"text\" name=\"other_contest_id\" /></td></tr>\n", cl);
  fprintf(out_f, "<tr><td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
          cl, SSERV_OP_USER_SEL_CREATE_REG_PAGE, "Register for another contest");
  if (cnts) {
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE, "Register for another contest and copy data");
  }
  fprintf(out_f, "</tr></table>\n");
  fprintf(out_f, "</div>");

  if (cnts) {
    fprintf(out_f, "<div id=\"ShowPasswordMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Password', true)\">[%s]</a>\n", "Password operations");
    fprintf(out_f, "</div>");
    fprintf(out_f, "<div style=\"display: none;\" id=\"HidePasswordMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Password', false)\">[%s]</a><br/>\n", "Hide password operations");
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>", cl);
    fprintf(out_f, "<tr><td%s><b>Registration passwords:</b></td>", cl);
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_SEL_VIEW_PASSWD_REDIRECT, "View");
    fprintf(out_f, "<td%s>&nbsp;</td>", cl);
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_SEL_RANDOM_PASSWD_PAGE, "Generate random");
    fprintf(out_f, "</tr>\n");
    if (!cnts->disable_team_password) {
      fprintf(out_f, "<tr><td%s><b>Contest passwords:</b></td>", cl);
      fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
              cl, SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_REDIRECT, "View");
      fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
              cl, SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE, "Clear");
      fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
              cl, SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE, "Generate random");
      fprintf(out_f, "</tr>\n");
    }
    fprintf(out_f, "</table>\n");
    fprintf(out_f, "</div>");

    fprintf(out_f, "<div id=\"ShowStatusMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Status', true)\">[%s]</a>\n", "Status operations");
    fprintf(out_f, "</div>");
    fprintf(out_f, "<div style=\"display: none;\" id=\"HideStatusMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Status', false)\">[%s]</a><br/>\n", "Hide status operations");
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>", cl);
    fprintf(out_f, "<tr><td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td></tr>",
            cl, SSERV_OP_USER_SEL_DELETE_REG_PAGE, "Delete registrations");
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
    ss_select(out_f, "status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, 0);
    fprintf(out_f, "</td>");
    fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td></tr>",
            cl, SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE, "Change status");
    fprintf(out_f, "</table>\n");
    fprintf(out_f, "</div>");

    fprintf(out_f, "<div id=\"ShowFlagsMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Flags', true)\">[%s]</a>\n", "Flags operations");
    fprintf(out_f, "</div>");
    fprintf(out_f, "<div style=\"display: none;\" id=\"HideFlagsMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('Flags', false)\">[%s]</a><br/>\n", "Hide flags operations");

    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>", cl);

    static const unsigned char * const flag_vars[] =
    {
      "invisible_op", "banned_op", "locked_op", "incomplete_op", "disqualified_op", NULL,
    };
    static const unsigned char * const flag_legends[] =
    {
      "Invisible", "Banned", "Locked", "Incomplete", "Disqualified", NULL,
    };

    for (int flag = 0; flag_vars[flag]; ++flag) {
      fprintf(out_f, "<tr><td%s><b>%s</b></td>", cl, flag_legends[flag]);
      for (int op = 0; flag_op_legends[op]; ++op) {
        s = "";
        if (!op) s = " checked=\"checked\"";
        fprintf(out_f, "<td%s><input type=\"radio\" name=\"%s\" value=\"%d\"%s /> %s</td>",
                cl, flag_vars[flag], op, s, flag_op_legends[op]);
      }
      fprintf(out_f, "</tr>\n");
    }

    fprintf(out_f, "<tr><td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td></tr>",
            cl, SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE, "Change flags");
    fprintf(out_f, "</table>\n");
    fprintf(out_f, "</div>");
  }

  fprintf(out_f, "</form>\n");

  if (opcaps_check(gcaps, OPCAP_CREATE_USER) >= 0) {
    fprintf(out_f, "<div id=\"ShowCreateUserMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('CreateUser', true)\">[%s]</a>\n",
            "User creation operations");
    fprintf(out_f, "</div>");
    fprintf(out_f, "<div style=\"display: none;\" id=\"HideCreateUserMenu\">");
    fprintf(out_f, "<a onclick=\"setOperationVisibility('CreateUser', false)\">[%s]</a><br/>\n",
            "Hide user creation operations");
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s><tr>", cl);
    fprintf(out_f, "<td%s>%s[%s]</a></td>", cl,
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CREATE_ONE_PAGE,
                          contest_id_str, group_id_str),
            "Create one new user");
    fprintf(out_f, "<td%s>%s[%s]</a></td>", cl,
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CREATE_MANY_PAGE,
                          contest_id_str, group_id_str),
            "Create MANY new users");
    fprintf(out_f, "<td%s>%s[%s]</a></td>", cl,
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CREATE_FROM_CSV_PAGE,
                          contest_id_str, group_id_str),
            "Create users from a CSV table");
    fprintf(out_f, "</tr></table>\n");
    fprintf(out_f, "</div>");
  }

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&users->b); users = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  bitset_free(&marked);
  xfree(marked_str);
  return retval;
}

static unsigned char *
collect_marked_set(
        struct super_http_request_info *phr,
        bitset_t *pms)
{
  const unsigned char *s = 0;

  if (ss_cgi_param(phr, "marked", &s) > 0 && s) {
    bitset_url_decode(s, pms);
  }

  int min_user_id = 0;
  int max_user_id = 0;
  ss_cgi_param_int_opt(phr, "min_user_id", &min_user_id, 0);
  ss_cgi_param_int_opt(phr, "max_user_id", &max_user_id, 0);
  if (min_user_id <= 0 || min_user_id > EJ_MAX_USER_ID
      || max_user_id <= 0 || max_user_id > EJ_MAX_USER_ID
      || min_user_id > max_user_id) return bitset_url_encode(pms);

  bitset_resize(pms, max_user_id + 1);
  for (int i = min_user_id; i <= max_user_id; ++i)
    bitset_off(pms, i);

  for (int i = 0; i < phr->param_num; ++i) {
    if (!strncmp(phr->param_names[i], "user_", 5)) {
      int user_id = 0, n = 0;
      if (sscanf(phr->param_names[i] + 5, "%d%n", &user_id, &n) == 1
          && !phr->param_names[i][n + 5]
          && user_id >= min_user_id
          && user_id <= max_user_id) {
        if (phr->param_sizes[i] == 1 && phr->params[i][0] == '1') {
          bitset_on(pms, user_id);
        }
      }
    }
  }
  return bitset_url_encode(pms);
}

int
super_serve_op_USER_FILTER_CHANGE_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  long long total_count = 0;
  int user_offset = 0;
  int user_count = 0;
  int value, r;
  int contest_id = 0, group_id = 0;
  const struct contest_desc *cnts = 0;
  opcap_t gcaps = 0;
  opcap_t caps = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;
  int notfirst = 0;
  FILE *extra_f = 0;
  char *extra_t = 0;
  size_t extra_z = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  extra_f = open_memstream(&extra_t, &extra_z);
  if (contest_id > 0) {
    fprintf(extra_f, "contest_id=%d", contest_id);
    notfirst = 1;
  }
  if (group_id > 0) {
    if (notfirst) putc('&', extra_f);
    notfirst = 1;
    fprintf(extra_f, "group_id=%d", group_id);
  }
  if (marked_str && *marked_str) {
    if (notfirst) putc('&', extra_f);
    notfirst = 1;
    fprintf(extra_f, "marked=%s", marked_str);
  }
  fclose(extra_f); extra_f = 0;

  if (get_global_caps(phr, &gcaps) >= 0 && opcaps_check(gcaps, OPCAP_LIST_USERS) >= 0) {
    // this user can view the full user list and the user list for any contest
  } else if (!cnts) {
    // user without global OPCAP_LIST_USERS capability cannot view the full user list
    FAIL(-S_ERR_PERM_DENIED);
  } else if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_LIST_USERS) < 0) {
    FAIL(-S_ERR_PERM_DENIED);
  }

  if (!phr->userlist_clnt) {
    goto cleanup;
  }
  if ((r = userlist_clnt_get_count(phr->userlist_clnt, ULS_GET_USER_COUNT,
                                   contest_id, group_id, 0, &total_count)) < 0) {
    err("set_user_filter: get_count failed: %d", -r);
    goto cleanup;
  }
  if (total_count <= 0) goto cleanup;
  if (phr->ss->user_filter_set) {
    user_offset = phr->ss->user_offset;
    user_count = phr->ss->user_count;
  }
  if (user_count <= 0) user_count = 20;
  if (user_count > 200) user_count = 200;

  switch (phr->opcode) {
  case SSERV_OP_USER_FILTER_CHANGE_ACTION:
    if (ss_cgi_param_int(phr, "user_offset", &value) >= 0) {
      user_offset = value;
    }
    if (ss_cgi_param_int(phr, "user_count", &value) >= 0) {
      user_count = value;
    }
    if (user_count <= 0) user_count = 20;
    if (user_count > 200) user_count = 200;
    break;

  case SSERV_OP_USER_FILTER_FIRST_PAGE_ACTION:
    user_offset = 0;
    break;
  case SSERV_OP_USER_FILTER_PREV_PAGE_ACTION:
    user_offset -= user_count;
    break;
  case SSERV_OP_USER_FILTER_NEXT_PAGE_ACTION:
    user_offset += user_count;
    break;
  case SSERV_OP_USER_FILTER_LAST_PAGE_ACTION:
    user_offset = total_count;
    break;
  }

  if (user_offset + user_count > total_count) {
    user_offset = total_count - user_count;
  }
  if (user_offset < 0) user_offset = 0;
  phr->ss->user_filter_set = 1;
  phr->ss->user_offset = user_offset;
  phr->ss->user_count = user_count;

cleanup:
  ss_redirect(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, extra_t);
  bitset_free(&marked);
  xfree(marked_str);
  if (extra_f) fclose(extra_f);
  xfree(extra_t);
  return retval;
}

int
super_serve_op_USER_JUMP_CONTEST_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, jump_contest_id = 0;
  const struct contest_desc *cnts = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "jump_contest_id", &jump_contest_id, 0);

  if (jump_contest_id < 0) jump_contest_id = 0;
  if (jump_contest_id > 0 && (contests_get(jump_contest_id, &cnts) < 0 || !cnts)) {
    cnts = 0;
    jump_contest_id = contest_id;
    if (jump_contest_id > 0 && (contests_get(jump_contest_id, &cnts) < 0 || !cnts)) {
      jump_contest_id = 0;
    }
  }

  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, jump_contest_id, group_id, 0, NULL);

  return retval;
}

int
super_serve_op_USER_BROWSE_MARK_ALL_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int contest_id = 0, group_id = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;
  const struct contest_desc *cnts = 0;
  int user_id = 0;
  struct userlist_list *users = 0;
  unsigned char *xml_text = 0;
  int notfirst = 0;
  FILE *extra_f = 0;
  char *extra_t = 0;
  size_t extra_z = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);

  if (contests_get(contest_id, &cnts) < 0 || !cnts) {
    contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (phr->opcode == SSERV_OP_USER_BROWSE_UNMARK_ALL_ACTION) {
    xfree(marked_str); marked_str = 0;
    goto cleanup;
  }

  // get the IDs of all users registered for contest
  r = userlist_clnt_list_all_users(phr->userlist_clnt, ULS_LIST_ALL_USERS, cnts->id, &xml_text);
  if (r < 0 || !xml_text) goto cleanup;
  users = userlist_parse_str(xml_text);
  if (!users) goto cleanup;
  xfree(xml_text); xml_text = 0;
  bitset_resize(&marked, users->user_map_size);

  if (phr->opcode == SSERV_OP_USER_BROWSE_MARK_ALL_ACTION) {
    for (user_id = 1; user_id < users->user_map_size; ++user_id) {
      if (users->user_map[user_id]) {
        bitset_on(&marked, user_id);
      }
    }
  } else if (phr->opcode == SSERV_OP_USER_BROWSE_TOGGLE_ALL_ACTION) {
    for (user_id = 1; user_id < users->user_map_size; ++user_id) {
      if (users->user_map[user_id]) {
        bitset_toggle(&marked, user_id);
      }
    }
  }

  xfree(marked_str);
  marked_str = bitset_url_encode(&marked);

cleanup:
  extra_f = open_memstream(&extra_t, &extra_z);
  if (contest_id > 0) {
    fprintf(extra_f, "contest_id=%d", contest_id);
    notfirst = 1;
  }
  if (group_id > 0) {
    if (notfirst) putc('&', extra_f);
    notfirst = 1;
    fprintf(extra_f, "group_id=%d", group_id);
  }
  if (marked_str && *marked_str) {
    if (notfirst) putc('&', extra_f);
    notfirst = 1;
    fprintf(extra_f, "marked=%s", marked_str);
  }
  fclose(extra_f); extra_f = 0;
  ss_redirect(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, extra_t);
  xfree(marked_str);
  bitset_free(&marked);
  if (extra_f) fclose(extra_f);
  xfree(extra_t);
  userlist_free(&users->b); users = 0;
  xfree(xml_text);
  return retval;
}

/*
handles: USER_SEL_RANDOM_PASSWD_PAGE
         USER_SEL_CLEAR_CNTS_PASSWD_PAGE
         USER_SEL_RANDOM_CNTS_PASSWD_PAGE
         USER_SEL_DELETE_REG_PAGE
         USER_SEL_CHANGE_REG_STATUS_PAGE
         USER_SEL_CHANGE_REG_FLAGS_PAGE
         USER_SEL_CREATE_REG_PAGE
         USER_SEL_CREATE_REG_AND_COPY_PAGE
 */
int
super_serve_op_USER_SEL_RANDOM_PASSWD_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  unsigned char buf[1024];
  unsigned char *marked_str = 0;
  bitset_t marked = BITSET_INITIALIZER;
  const struct contest_desc *cnts = 0;
  int user_id = 0, user_count = 0, serial;
  const unsigned char *cl = 0;
  unsigned char *xml_text = 0;
  struct userlist_list *users = 0;
  const struct userlist_user *u = 0;
  const struct userlist_contest *reg = 0;
  const struct userlist_user_info *ui = 0;
  const unsigned char *s = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  opcap_t gcaps = 0, caps = 0;
  int r;
  int need_privileged = 0, need_invisible = 0, need_banned = 0, need_locked = 0, need_disqualified = 0;
  int operation = 0;
  const unsigned char *button_label = 0;
  int status = USERLIST_REG_REJECTED;
  int invisible_op = 0, banned_op = 0, locked_op = 0, incomplete_op = 0, disqualified_op = 0;
  int is_set_changed = 0;
  const int *cnts_id_list = 0;
  int cnts_id_count = 0;
  int other_contest_id = 0;
  const struct contest_desc *other_cnts = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }

  /* additional parameters */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
    ss_cgi_param_int_opt(phr, "status", &status, -1);
    if (status < 0 || status >= USERLIST_REG_LAST) FAIL(S_ERR_INV_VALUE);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
    ss_cgi_param_int_opt(phr, "invisible_op", &invisible_op, 0);
    ss_cgi_param_int_opt(phr, "banned_op", &banned_op, 0);
    ss_cgi_param_int_opt(phr, "locked_op", &locked_op, 0);
    ss_cgi_param_int_opt(phr, "incomplete_op", &incomplete_op, 0);
    ss_cgi_param_int_opt(phr, "disqualified_op", &disqualified_op, 0);
    if (invisible_op < 0 || invisible_op > 3) invisible_op = 0;
    if (banned_op < 0 || banned_op > 3) banned_op = 0;
    if (locked_op < 0 || locked_op > 3) locked_op = 0;
    if (incomplete_op < 0 || incomplete_op > 3) incomplete_op = 0;
    if (disqualified_op < 0 || disqualified_op > 3) disqualified_op = 0;
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
    ss_cgi_param_int_opt(phr, "other_contest_id", &other_contest_id, 0);
    if (other_contest_id <= 0 || contests_get(other_contest_id, &other_cnts) < 0 || !other_cnts) {
      other_contest_id = 0;
    }
    cnts_id_count = contests_get_list(&cnts_id_list);
    break;
  }

  /* contest_id check and preliminary permission check */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_RANDOM_PASSWD_PAGE:
    if (get_global_caps(phr, &gcaps) < 0) FAIL(S_ERR_PERM_DENIED);
    if (opcaps_check(gcaps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE:
  case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_DELETE_REG_PAGE:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_DELETE_REG;
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_DELETE_REG;
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0 && opcaps_check(caps, OPCAP_DELETE_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_EDIT_REG;
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_EDIT_REG;
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0 && opcaps_check(caps, OPCAP_EDIT_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
    break;
  default:
    abort();
  }

  if (!phr->userlist_clnt) FAIL(S_ERR_DB_ERROR);
  r = userlist_clnt_list_users_2(phr->userlist_clnt, ULS_LIST_ALL_USERS_3,
                                 contest_id, group_id, marked_str, 0, 0,
                                 &xml_text);
  if (r < 0) FAIL(S_ERR_DB_ERROR);
  users = userlist_parse_str(xml_text);
  if (!users) FAIL(S_ERR_DB_ERROR);

  for (user_id = 1; user_id < marked.size; ++user_id) {
    if (bitset_get(&marked, user_id)) {
      if (user_id >= users->user_map_size || !(u = users->user_map[user_id])) {
        bitset_off(&marked, user_id);
        is_set_changed = 1;
        continue;
      }
      if (contest_id > 0 && !userlist_get_user_contest(u, contest_id)) {
        bitset_off(&marked, user_id);
        is_set_changed = 1;
        continue;
      }
      /* per-user check */
      switch (phr->opcode) {
      case SSERV_OP_USER_SEL_RANDOM_PASSWD_PAGE:
        if (is_privileged(phr, cnts, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else {
          if (opcaps_check(gcaps, OPCAP_EDIT_PASSWD) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE:
      case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_DELETE_REG_PAGE:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_DELETE_REG) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_DELETE_REG) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
      case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_REG) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_EDIT_REG) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
      case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
        break;
      default:
        abort();
      }
      if (!u) {
        bitset_off(&marked, user_id);
        is_set_changed = 1;
        continue;
      }
      ++user_count;
    }
  }
  if (user_count <= 0) {
    ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, NULL);
    goto cleanup;
  }

  if (is_set_changed) {
    xfree(marked_str);
    marked_str = bitset_url_encode(&marked);
  }

  /* page header generation */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_RANDOM_PASSWD_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, generate random registration passwords", phr->html_name);
    break;
  case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, clear contest passwords for contest %d",
             phr->html_name, contest_id);
    break;
  case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, generate random contest passwords for contest %d",
             phr->html_name, contest_id);
    break;
  case SSERV_OP_USER_SEL_DELETE_REG_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, delete registrations from contest %d",
             phr->html_name, contest_id);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, change registration statuses in contest %d",
             phr->html_name, contest_id);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
    if (!(invisible_op + banned_op + locked_op + incomplete_op + disqualified_op)) {
      ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, marked_str);
      goto cleanup;
    }
    snprintf(buf, sizeof(buf), "serve-control: %s, change registration flags in contest %d",
             phr->html_name, contest_id);
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, register users for another contest",
             phr->html_name);
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, register users for another contest and copy data",
             phr->html_name);
    break;
  default:
    abort();
  }

  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function updateCnts1()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj1.value;\n"
          "  var i;\n"
          "  for (i = 0; i < obj2.options.length; ++i) {\n"
          "    if (obj2.options[i].value == value) {\n"
          "      obj2.options.selectedIndex = i;\n"
          "      break;\n"
          "    }\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function updateCnts2()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj2.options[obj2.selectedIndex].value;\n"
          "  obj1.value = value;\n"
          "}\n");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  /* additional info */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE:
  case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE:
  case SSERV_OP_USER_SEL_DELETE_REG_PAGE:
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
    fprintf(out_f, "<h2>%s</h2>\n", ARMOR(cnts->name));
    break;
  }

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, marked_str);

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  if (marked_str && *marked_str) {
    html_hidden(out_f, "marked", "%s", marked_str);
  }

  /* additional info */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_RANDOM_PASSWD_PAGE:
    fprintf(out_f, "<p>The registration passwords are to be regenerated for the following %d users:</p>\n",
            user_count);
    operation = SSERV_OP_USER_SEL_RANDOM_PASSWD_ACTION;
    button_label = "Generate passwords!";
    break;
  case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_PAGE:
    fprintf(out_f, "<p>The contest passwords are to be cleared for the following %d users:</p>\n",
            user_count);
    operation = SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_ACTION;
    button_label = "Clear contest passwords!";
    break;
  case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_PAGE:
    fprintf(out_f, "<p>The contest passwords are to be regenerated for the following %d users:</p>\n",
            user_count);
    operation = SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_ACTION;
    button_label = "Generate contest passwords!";
    break;
  case SSERV_OP_USER_SEL_DELETE_REG_PAGE:
    fprintf(out_f, "<p>The registrations are to be deleted for the following %d users:</p>\n", user_count);
    operation = SSERV_OP_USER_SEL_DELETE_REG_ACTION;
    button_label = "Delete registrations!";
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_PAGE:
    html_hidden(out_f, "status", "%d", status);
    fprintf(out_f, "<p>The registration status is to be changed to %s for the following %d users:</p>\n",
            reg_status_strs[status], user_count);
    operation = SSERV_OP_USER_SEL_CHANGE_REG_STATUS_ACTION;
    button_label = "Change!";
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE:
    html_hidden(out_f, "invisible_op", "%d", invisible_op);
    html_hidden(out_f, "banned_op", "%d", banned_op);
    html_hidden(out_f, "locked_op", "%d", locked_op);
    html_hidden(out_f, "incomplete_op", "%d", incomplete_op);
    html_hidden(out_f, "disqualified_op", "%d", disqualified_op);
    fprintf(out_f, "<p>The registration flags are to be changed for the following %d users as follows:</p>\n",
            user_count);
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>", cl);
    if (invisible_op > 0) {
      fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
              cl, "Invisible", cl, flag_op_legends[invisible_op]);
    }
    if (banned_op > 0) {
      fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
              cl, "Banned", cl, flag_op_legends[banned_op]);
    }
    if (locked_op > 0) {
      fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
              cl, "Locked", cl, flag_op_legends[locked_op]);
    }
    if (incomplete_op > 0) {
      fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
              cl, "Incomplete", cl, flag_op_legends[incomplete_op]);
    }
    if (disqualified_op > 0) {
      fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
              cl, "Disqualified", cl, flag_op_legends[disqualified_op]);
    }
    fprintf(out_f, "</table>\n");
    operation = SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION;
    button_label = "Change!";
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>\n", cl);
    buf[0] = 0;
    if (other_contest_id > 0) snprintf(buf, sizeof(buf), "%d", other_contest_id);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input id=\"cnts1\" onchange=\"updateCnts1()\" type=\"text\" name=\"other_contest_id_1\" size=\"20\" value=\"%s\"/></td></tr>\n",
            cl, "Contest ID", cl, buf);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Contest name", cl);
    fprintf(out_f, "<select id=\"cnts2\" onchange=\"updateCnts2()\" name=\"other_contest_id_2\"><option value=\"0\"></option>");
    for (int i = 0; i < cnts_id_count; ++i) {
      int other_contest_id_2 = cnts_id_list[i];
      if (other_contest_id_2 <= 0) continue;
      if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
      if (cnts->closed) continue;
      s = "";
      if (other_contest_id_2 == other_contest_id) s = " selected=\"selected\"";
      fprintf(out_f, "<option value=\"%d\"%s>%s</option>", other_contest_id_2, s, ARMOR(cnts->name));
    }
    fprintf(out_f, "</select>");
    fprintf(out_f, "</td></tr>\n");
    fprintf(out_f, "</table>\n");

    switch (phr->opcode) {
    case SSERV_OP_USER_SEL_CREATE_REG_PAGE:
      operation = SSERV_OP_USER_SEL_CREATE_REG_ACTION;
      button_label = "Register!";
      break;
    case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_PAGE:
      operation = SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_ACTION;
      button_label = "Register and copy!";
      break;
    }
    break;
  default:
    abort();
  }

  cl = " class=\"b1\"";
  fprintf(out_f, "<table%s>", cl);
  fprintf(out_f, "<tr><th%s>%s</th><th%s>%s</th><th%s>%s</th><th%s>%s</th><th%s>%s</th><th%s>%s</th></tr>\n",
          cl, "NN", cl, "User ID", cl, "Login", cl, "Name", cl, "Status", cl, "Flags");
  for (user_id = 1, serial = 0; user_id < marked.size; ++user_id) {
    if (bitset_get(&marked, user_id)) {
      u = users->user_map[user_id];
      ui = u->cnts0;
      reg = 0;
      if (contest_id > 0) {
        reg = userlist_get_user_contest(u, contest_id);
      }
      fprintf(out_f, "<tr><td%s>%d</td>", cl, ++serial);
      fprintf(out_f, "<td%s>%d</td>", cl, user_id);
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(u->login));
      s = u->login;
      if (ui && ui->name && *ui->name) s = ui->name;
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(s));
      if (cnts && reg) {
        r = reg->status;
        if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_LAST;
        fprintf(out_f, "<td%s>%s</td>", cl, reg_status_strs[r]);
      } else {
        fprintf(out_f, "<td%s>&nbsp;</td>", cl);
      }
      fprintf(out_f, "<td%s>", cl);
      s = "";
      if (is_privileged(phr, cnts, u)) {
        need_privileged = 1;
        fprintf(out_f, "%s%s", s, "privileged");
        s = ", ";
      }
      if (cnts && reg) {
        if ((reg->flags & USERLIST_UC_INVISIBLE)) {
          need_invisible = 1;
          fprintf(out_f, "%s%s", s, "invisible");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_BANNED)) {
          need_banned = 1;
          fprintf(out_f, "%s%s", s, "banned");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_LOCKED)) {
          need_locked = 1;
          fprintf(out_f, "%s%s", s, "locked");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_DISQUALIFIED)) {
          need_disqualified = 1;
          fprintf(out_f, "%s%s", s, "disqualified");
          s = ", ";
        }
      }
      if (!*s) fprintf(out_f, "&nbsp;");
      fprintf(out_f, "</td>");
      fprintf(out_f, "</tr>\n");
    }
  }
  fprintf(out_f, "</table>\n");

  if (phr->opcode != SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_PAGE) {
    cl = " class=\"b0\"";
    fprintf(out_f, "<table%s>", cl);
    if (need_privileged) {
      fprintf(out_f, "<tr><td%s><input type=\"checkbox\" name=\"%s\" value=\"1\" /></td><td%s>%s</td></tr>\n",
              cl, "include_privileged", cl, "Peform the operation even for PRIVILEGED users");
    }
    if (need_invisible) {
      fprintf(out_f, "<tr><td%s><input type=\"checkbox\" name=\"%s\" value=\"1\" /></td><td%s>%s</td></tr>\n",
              cl, "include_invisible", cl, "Peform the operation even for INVISIBLE users");
    }
    if (need_banned) {
      fprintf(out_f, "<tr><td%s><input type=\"checkbox\" name=\"%s\" value=\"1\" /></td><td%s>%s</td></tr>\n",
              cl, "include_banned", cl, "Peform the operation even for BANNED users");
    }
    if (need_locked) {
      fprintf(out_f, "<tr><td%s><input type=\"checkbox\" name=\"%s\" value=\"1\" /></td><td%s>%s</td></tr>\n",
              cl, "include_locked", cl, "Peform the operation even for LOCKED users");
    }
    if (need_disqualified) {
      fprintf(out_f, "<tr><td%s><input type=\"checkbox\" name=\"%s\" value=\"1\" /></td><td%s>%s</td></tr>\n",
              cl, "include_disqualified", cl, "Peform the operation even for DISQUALIFIED users");
    }
    fprintf(out_f, "</table>");
  }

  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s><tr>", cl);
  fprintf(out_f, "<tr><td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td>",
            cl, SSERV_OP_USER_SEL_CANCEL_ACTION, "Cancel");
  fprintf(out_f, "<td%s><input type=\"submit\" name=\"op_%d\" value=\"%s\" /></td></tr>",
            cl, operation, button_label);
  fprintf(out_f, "</tr></table>\n");
    
  fprintf(out_f, "</form>\n");

  ss_write_html_footer(out_f);

cleanup:
  html_armor_free(&ab);
  userlist_free(&users->b); users = 0;
  bitset_free(&marked);
  xfree(marked_str);
  xfree(xml_text);
  return retval;
}

int
super_serve_op_USER_SEL_RANDOM_PASSWD_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int contest_id = 0, group_id = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;
  const struct contest_desc *cnts = 0, *other_cnts = 0;
  int status = -1;
  int invisible_op = 0, banned_op = 0, locked_op = 0, incomplete_op = 0, disqualified_op = 0;
  int clear_mask = 0, set_mask = 0, toggle_mask = 0;
  int other_contest_id = 0;
  opcap_t gcaps = 0, caps = 0, rcaps = 0;
  unsigned char *xml_text = 0;
  struct userlist_list *users = 0;
  int user_id = 0, user_count = 0;
  const struct userlist_user *u = 0;
  const struct userlist_contest *reg = 0;
  int include_privileged = 0, include_invisible = 0, include_banned = 0, include_locked = 0, include_disqualified = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  ss_cgi_param_int_opt(phr, "include_privileged", &include_privileged, 0);
  if (include_privileged != 1) include_privileged = 0;
  ss_cgi_param_int_opt(phr, "include_invisible", &include_invisible, 0);
  if (include_invisible != 1) include_invisible = 0;
  ss_cgi_param_int_opt(phr, "include_banned", &include_banned, 0);
  if (include_banned != 1) include_banned = 0;
  ss_cgi_param_int_opt(phr, "include_locked", &include_locked, 0);
  if (include_locked != 1) include_locked = 0;
  ss_cgi_param_int_opt(phr, "include_disqualified", &include_disqualified, 0);
  if (include_disqualified != 1) include_disqualified = 0;

  /* additional parameters */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_ACTION:
    ss_cgi_param_int_opt(phr, "status", &status, -1);
    if (status < 0 || status >= USERLIST_REG_LAST) FAIL(S_ERR_INV_VALUE);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION:
    ss_cgi_param_int_opt(phr, "invisible_op", &invisible_op, 0);
    ss_cgi_param_int_opt(phr, "banned_op", &banned_op, 0);
    ss_cgi_param_int_opt(phr, "locked_op", &locked_op, 0);
    ss_cgi_param_int_opt(phr, "incomplete_op", &incomplete_op, 0);
    ss_cgi_param_int_opt(phr, "disqualified_op", &disqualified_op, 0);
    if (invisible_op < 0 || invisible_op > 3) invisible_op = 0;
    if (banned_op < 0 || banned_op > 3) banned_op = 0;
    if (locked_op < 0 || locked_op > 3) locked_op = 0;
    if (incomplete_op < 0 || incomplete_op > 3) incomplete_op = 0;
    if (disqualified_op < 0 || disqualified_op > 3) disqualified_op = 0;
    if (invisible_op == 1) {
      clear_mask |= USERLIST_UC_INVISIBLE;
    } else if (invisible_op == 2) {
      set_mask |= USERLIST_UC_INVISIBLE;
    } else if (invisible_op == 3) {
      toggle_mask |= USERLIST_UC_INVISIBLE;
    }
    if (banned_op == 1) {
      clear_mask |= USERLIST_UC_BANNED;
    } else if (banned_op == 2) {
      set_mask |= USERLIST_UC_BANNED;
    } else if (banned_op == 3) {
      toggle_mask |= USERLIST_UC_BANNED;
    }
    if (locked_op == 1) {
      clear_mask |= USERLIST_UC_LOCKED;
    } else if (locked_op == 2) {
      set_mask |= USERLIST_UC_LOCKED;
    } else if (locked_op == 3) {
      toggle_mask |= USERLIST_UC_LOCKED;
    }
    if (incomplete_op == 1) {
      clear_mask |= USERLIST_UC_INCOMPLETE;
    } else if (incomplete_op == 2) {
      set_mask |= USERLIST_UC_INCOMPLETE;
    } else if (incomplete_op == 3) {
      toggle_mask |= USERLIST_UC_INCOMPLETE;
    }
    if (disqualified_op == 1) {
      clear_mask |= USERLIST_UC_DISQUALIFIED;
    } else if (disqualified_op == 2) {
      set_mask |= USERLIST_UC_DISQUALIFIED;
    } else if (disqualified_op == 3) {
      toggle_mask |= USERLIST_UC_DISQUALIFIED;
    }
    if (!(clear_mask + set_mask + toggle_mask)) goto done;
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_ACTION:
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_ACTION:
    ss_cgi_param_int_opt(phr, "other_contest_id_1", &other_contest_id, 0);
    if (other_contest_id <= 0 || contests_get(other_contest_id, &other_cnts) < 0 || !other_cnts) {
      other_contest_id = 0;
    }
    if (!other_cnts) FAIL(S_ERR_INV_CONTEST);
    break;
  }

  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_RANDOM_PASSWD_ACTION:
    if (get_global_caps(phr, &gcaps) < 0) FAIL(S_ERR_PERM_DENIED);
    if (opcaps_check(gcaps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_ACTION:
  case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_ACTION:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_DELETE_REG_ACTION:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_DELETE_REG;
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_DELETE_REG;
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0 && opcaps_check(caps, OPCAP_DELETE_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_ACTION:
  case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_EDIT_REG;
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_EDIT_REG;
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0 && opcaps_check(caps, OPCAP_EDIT_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_ACTION:
    get_global_caps(phr, &gcaps);
    get_contest_caps(phr, other_cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_CREATE_REG) < 0 && opcaps_check(caps, OPCAP_CREATE_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_ACTION:
    if  (!cnts) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    get_contest_caps(phr, other_cnts, &caps);
    caps |= gcaps;
    if (opcaps_check(caps, OPCAP_PRIV_CREATE_REG) < 0 && opcaps_check(caps, OPCAP_CREATE_REG) < 0)
      FAIL(S_ERR_PERM_DENIED);
    get_contest_caps(phr, cnts, &rcaps);
    rcaps |= gcaps;
    if (opcaps_check(rcaps, OPCAP_GET_USER) < 0)
      FAIL(S_ERR_PERM_DENIED);
    break;
  default:
    abort();
  }

  if (!phr->userlist_clnt) FAIL(S_ERR_DB_ERROR);
  r = userlist_clnt_list_users_2(phr->userlist_clnt, ULS_LIST_ALL_USERS_3,
                                 contest_id, group_id, marked_str, 0, 0,
                                 &xml_text);
  if (r < 0) FAIL(S_ERR_DB_ERROR);
  users = userlist_parse_str(xml_text);
  if (!users) FAIL(S_ERR_DB_ERROR);

  for (user_id = 1; user_id < marked.size; ++user_id) {
    if (bitset_get(&marked, user_id)) {
      if (user_id >= users->user_map_size || !(u = users->user_map[user_id])) {
        bitset_off(&marked, user_id);
        continue;
      }
      if (contest_id > 0 && !userlist_get_user_contest(u, contest_id)) {
        bitset_off(&marked, user_id);
        continue;
      }
      if (!include_privileged && is_privileged(phr, cnts, u)) {
        bitset_off(&marked, user_id);
        continue;
      }
      if (cnts && phr->opcode != SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION
          && (reg = userlist_get_user_contest(u, contest_id))) {
        if (((reg->flags & USERLIST_UC_INVISIBLE) && !include_invisible)
            || ((reg->flags & USERLIST_UC_BANNED) && !include_banned)
            || ((reg->flags & USERLIST_UC_LOCKED) && !include_locked)
            || ((reg->flags & USERLIST_UC_DISQUALIFIED) && !include_disqualified)) {
          bitset_off(&marked, user_id);
          continue;
        }
      }
      switch (phr->opcode) {
      case SSERV_OP_USER_SEL_RANDOM_PASSWD_ACTION:
        if (is_privileged(phr, cnts, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else {
          if (opcaps_check(gcaps, OPCAP_EDIT_PASSWD) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_ACTION:
      case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_ACTION:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_DELETE_REG_ACTION:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_DELETE_REG) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_DELETE_REG) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_ACTION:
      case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_REG) < 0) u = 0;
        } else if (is_contest_privileged(cnts, u)) {
          if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_EDIT_REG) < 0) u = 0;
        }
        break;
      case SSERV_OP_USER_SEL_CREATE_REG_ACTION:
      case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_ACTION:
        if (is_globally_privileged(phr, u)) {
          if (opcaps_check(gcaps, OPCAP_PRIV_CREATE_REG) < 0) u = 0;
        } else {
          if (opcaps_check(caps, OPCAP_CREATE_USER) < 0) u = 0;
        }
        break;
      default:
        abort();
      }
      if (!u) {
        bitset_off(&marked, user_id);
        continue;
      }
      ++user_count;
    }
  }
  if (user_count <= 0) goto done;

  /* do the requested operation */
  for (user_id = 1; user_id < marked.size; ++user_id) {
    if (!bitset_get(&marked, user_id)) continue;

    r = 0;
    switch (phr->opcode) {
    case SSERV_OP_USER_SEL_RANDOM_PASSWD_ACTION:
      r = userlist_clnt_register_contest(phr->userlist_clnt, ULS_RANDOM_PASSWD, user_id,
                                         contest_id, 0, 0);
      break;
    case SSERV_OP_USER_SEL_CLEAR_CNTS_PASSWD_ACTION:
      r = userlist_clnt_delete_field(phr->userlist_clnt, ULS_DELETE_FIELD,
                                     user_id, contest_id, 0, USERLIST_NC_TEAM_PASSWD);
      break;
    case SSERV_OP_USER_SEL_RANDOM_CNTS_PASSWD_ACTION:
      r = userlist_clnt_register_contest(phr->userlist_clnt, ULS_RANDOM_TEAM_PASSWD, user_id,
                                         contest_id, 0, 0);
      break;
    case SSERV_OP_USER_SEL_DELETE_REG_ACTION:
      r = userlist_clnt_change_registration(phr->userlist_clnt, user_id, contest_id, -2, 0, 0);
      break;
    case SSERV_OP_USER_SEL_CHANGE_REG_STATUS_ACTION:
      r = userlist_clnt_change_registration(phr->userlist_clnt, user_id, contest_id, status, 0, 0);
      break;
    case SSERV_OP_USER_SEL_CHANGE_REG_FLAGS_ACTION:
      if (clear_mask > 0 && r >= 0) {
        r = userlist_clnt_change_registration(phr->userlist_clnt, user_id, contest_id, -1, 2, clear_mask);
      }
      if (set_mask > 0 && r >= 0) {
        r = userlist_clnt_change_registration(phr->userlist_clnt, user_id, contest_id, -1, 1, set_mask);
      }
      if (toggle_mask > 0 && r >= 0) {
        r = userlist_clnt_change_registration(phr->userlist_clnt, user_id, contest_id, -1, 3, toggle_mask);
      }
      break;
    case SSERV_OP_USER_SEL_CREATE_REG_ACTION:
      r = userlist_clnt_register_contest(phr->userlist_clnt, ULS_PRIV_REGISTER_CONTEST,
                                         user_id, other_contest_id, 0, 0);
      break;
    case SSERV_OP_USER_SEL_CREATE_REG_AND_COPY_ACTION:
      r = userlist_clnt_register_contest(phr->userlist_clnt, ULS_PRIV_REGISTER_CONTEST,
                                         user_id, other_contest_id, 0, 0);
      if (r >= 0) {
        r = userlist_clnt_copy_user_info(phr->userlist_clnt, user_id, contest_id, other_contest_id);
      }
      break;
    default:
      abort();
    }
    if (r < 0) FAIL(S_ERR_DB_ERROR);
  }

done:
  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, NULL);

cleanup:
  userlist_free(&users->b); users = 0;
  bitset_free(&marked);
  xfree(marked_str);
  xfree(xml_text);
  return retval;
}

int
super_serve_op_USER_SEL_CANCEL_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int contest_id = 0, group_id = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);
  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, marked_str);

  xfree(marked_str);
  bitset_free(&marked);
  return 0;
}

struct user_row_info
{
  int field_id;
  unsigned char *field_desc;
};

static char const * const member_string[] =
{
  "Contestant",
  "Reserve",
  "Coach",
  "Advisor",
  "Guest"
};
static char const * const member_string_pl[] =
{
  "Contestants",
  "Reserves",
  "Coaches",
  "Advisors",
  "Guests"
};

static void
string_row(
        FILE *out_f,
        const unsigned char *tr_class,
        int is_hidden,
        const unsigned char *td_class,
        const unsigned char *legend,
        const unsigned char *param_suffix,
        const unsigned char *str)
{
  unsigned char trcl[256];
  unsigned char tdcl[256];
  unsigned char param_name[256];
  unsigned char buf[1024];
  unsigned char onchange[1024];
  const unsigned char *checked = "";
  const unsigned char *display = "";
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  trcl[0] = 0;
  if (tr_class) {
    snprintf(trcl, sizeof(trcl), " class=\"%s\"", tr_class);
  }
  tdcl[0] = 0;
  if (td_class) {
    snprintf(tdcl, sizeof(tdcl), " class=\"%s\"", td_class);
  }
  if (!str) {
    checked = " checked=\"checked\"";
    str = "";
  }
  snprintf(param_name, sizeof(param_name), "field_%s", param_suffix);
  if (is_hidden) {
    display = " style=\"display: none;\"";
  }

  fprintf(out_f, "<tr%s%s>", trcl, display);
  fprintf(out_f, "<td%s><b>%s:</b></td>", tdcl, legend);
  fprintf(out_f, "<td%s><input type=\"checkbox\" onchange=\"checkNull('%s')\" name=\"field_null_%s\" value=\"1\"%s /></td>",
          tdcl, param_suffix, param_suffix, checked);
  snprintf(onchange, sizeof(onchange), "uncheckNull('%s')", param_suffix);
  fprintf(out_f, "<td%s>%s</td>", tdcl,
          html_input_text_js(buf, sizeof(buf), param_name, 50, onchange, "%s", ARMOR(str)));
  fprintf(out_f, "<td%s>&nbsp;</td>", tdcl);
  fprintf(out_f, "</tr>\n");
  html_armor_free(&ab);
}

static const struct user_row_info user_flag_rows[] =
{
  { USERLIST_NN_IS_PRIVILEGED, "Globally privileged" },
  { USERLIST_NN_IS_INVISIBLE, "Globally invisible" },
  { USERLIST_NN_IS_BANNED, "Globally banned" },
  { USERLIST_NN_IS_LOCKED, "Globally locked" },
  { USERLIST_NN_SHOW_LOGIN, "Show login to everybody" },
  { USERLIST_NN_SHOW_EMAIL, "Show email to everybody" },
  { USERLIST_NN_READ_ONLY, "Globally read-only" },
  { USERLIST_NN_NEVER_CLEAN, "Do not auto-clean" },
  { USERLIST_NN_SIMPLE_REGISTRATION, "Simple registration" },
  { 0, 0 },
};

static const struct user_row_info user_timestamp_rows[] =
{
  { USERLIST_NN_REGISTRATION_TIME, "Registration time" },
  { USERLIST_NN_LAST_LOGIN_TIME, "Last login time" },
  { USERLIST_NN_LAST_CHANGE_TIME, "Last change time" },
  { USERLIST_NN_LAST_PWDCHANGE_TIME, "Last password change time" },
  { 0, 0 },
};

static const struct user_row_info user_info_rows[] =
{
  { USERLIST_NC_INST, "Institution name" },
  { USERLIST_NC_INST_EN, "Inst. name (En)" },
  { USERLIST_NC_INSTSHORT, "Short inst. name" },
  { USERLIST_NC_INSTSHORT_EN, "Short inst. name (En)" },
  { USERLIST_NC_INSTNUM, "Institution number" },
  { USERLIST_NC_FAC, "Faculty name" },
  { USERLIST_NC_FAC_EN, "Faculty name (En)" },
  { USERLIST_NC_FACSHORT, "Short faculty name" },
  { USERLIST_NC_FACSHORT_EN, "Short faculty name (En)" },
  { USERLIST_NC_HOMEPAGE, "Web home page" },
  { USERLIST_NC_CITY, "City" },
  { USERLIST_NC_CITY_EN, "City (En)" },
  { USERLIST_NC_COUNTRY, "Country" },
  { USERLIST_NC_COUNTRY_EN, "Country (En)" },
  { USERLIST_NC_REGION, "Region" },
  { USERLIST_NC_AREA, "Region (En)" },
  { USERLIST_NC_ZIP, "Zip code" },
  { USERLIST_NC_STREET, "Street address" },
  { USERLIST_NC_LOCATION, "Computer location" },
  { USERLIST_NC_SPELLING, "Name spelling" },
  { USERLIST_NC_PRINTER_NAME, "Printer name" },
  { USERLIST_NC_EXAM_ID, "Examination Id" },
  { USERLIST_NC_EXAM_CYPHER, "Examination cypher" },
  { USERLIST_NC_LANGUAGES, "Programming languages" },
  { USERLIST_NC_PHONE, "Contact phone" },
  { USERLIST_NC_FIELD0, "Additional field 0" },
  { USERLIST_NC_FIELD1, "Additional field 1" },
  { USERLIST_NC_FIELD2, "Additional field 2" },
  { USERLIST_NC_FIELD3, "Additional field 3" },
  { USERLIST_NC_FIELD4, "Additional field 4" },
  { USERLIST_NC_FIELD5, "Additional field 5" },
  { USERLIST_NC_FIELD6, "Additional field 6" },
  { USERLIST_NC_FIELD7, "Additional field 7" },
  { USERLIST_NC_FIELD8, "Additional field 8" },
  { USERLIST_NC_FIELD9, "Additional field 9" },

  { 0, 0 },
};

static const struct user_row_info user_info_stat_rows[] =
{
  { USERLIST_NC_CREATE_TIME, "Create time" },
  { USERLIST_NC_LAST_LOGIN_TIME, "Last login time" },
  { USERLIST_NC_LAST_CHANGE_TIME, "Last change time" },
  { USERLIST_NC_LAST_PWDCHANGE_TIME, "Last password change time" },

  { 0, 0 },
};

static const struct user_row_info member_rows[] =
{
  { USERLIST_NM_FIRSTNAME, "First name" },
  { USERLIST_NM_FIRSTNAME_EN, "First name (En)" },
  { USERLIST_NM_MIDDLENAME, "Middle name" },
  { USERLIST_NM_MIDDLENAME_EN, "Middle name (En)" },
  { USERLIST_NM_SURNAME, "Surname" },
  { USERLIST_NM_SURNAME_EN, "Surname (En)" },
  { USERLIST_NM_GROUP, "Academic group" },
  { USERLIST_NM_GROUP_EN, "Academic group (En)" },
  { USERLIST_NM_EMAIL, "Email" },
  { USERLIST_NM_HOMEPAGE, "Web home page" },
  { USERLIST_NM_OCCUPATION, "Occupation" },
  { USERLIST_NM_OCCUPATION_EN, "Occupation (En)" },
  { USERLIST_NM_DISCIPLINE, "Discipline" },
  { USERLIST_NM_INST, "Institution name" },
  { USERLIST_NM_INST_EN, "Institution name (En)" },
  { USERLIST_NM_INSTSHORT, "Short inst. name" },
  { USERLIST_NM_INSTSHORT_EN, "Short inst. name (En)" },
  { USERLIST_NM_FAC, "Faculty name" },
  { USERLIST_NM_FAC_EN, "Faculty name (En)" },
  { USERLIST_NM_FACSHORT, "Short faculty name" },
  { USERLIST_NM_FACSHORT_EN, "Short faculty name (En)" },
  { USERLIST_NM_PHONE, "Phone" },

  { 0, 0 },
};

static const struct user_row_info member_date_rows[] =
{
  { USERLIST_NM_BIRTH_DATE, "Date of birth" },
  { USERLIST_NM_ENTRY_DATE, "Date of entry" },
  { USERLIST_NM_GRADUATION_DATE, "Graduation date" },

  { 0, 0 },
};

static const struct user_row_info member_time_rows[] =
{
  { USERLIST_NM_CREATE_TIME, "Create time" },
  { USERLIST_NM_LAST_CHANGE_TIME, "Last change time" },

  { 0, 0 },
};

int
super_serve_op_USER_DETAIL_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r, row, role, pers, reg_count, cookie_count;
  int other_user_id = 0, contest_id = 0, group_id = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  unsigned char buf[1024];
  unsigned char buf2[1024];
  unsigned char hbuf[1024];
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  struct userlist_user_info *ui = 0;
  const unsigned char *cl, *s, *s2;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  struct userlist_member *m;
  const struct contest_desc *cnts = 0;
  struct userlist_contest *reg;
  struct userlist_cookie *cookie;
  opcap_t gcaps = 0;
  opcap_t caps = 0;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }

  if (get_global_caps(phr, &gcaps) >= 0 && opcaps_check(gcaps, OPCAP_GET_USER) >= 0) {
    // this user can view the full user list and the user list for any contest
  } else if (!cnts) {
    // user without global OPCAP_LIST_USERS capability cannot view the full user list
    FAIL(-S_ERR_PERM_DENIED);
  } else if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_GET_USER) < 0) {
    FAIL(-S_ERR_PERM_DENIED);
  }

  snprintf(buf, sizeof(buf), "serve-control: %s, viewing user %d",
           phr->html_name, other_user_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function toggleRowsVisibility(value, rows1, rows2)\n"
          "{\n"
          "  var vis1 = \"\";\n"
          "  var vis2 = \"\";\n"
          "  if (value == true) {\n"
          "    vis1 = \"none\";\n"
          "  } else {\n"
          "    vis2 = \"none\";\n"
          "  }\n"
          "  if (rows1 != null) {\n"
          "    for (var row in rows1) {\n"
          "      var obj = document.getElementById(rows1[row]);\n"
          "      if (obj != null) {\n"
          "        obj.style.display = vis1;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "  if (rows2 != null) {\n"
          "    for (var row in rows2) {\n"
          "      var obj = document.getElementById(rows2[row]);\n"
          "      if (obj != null) {\n"
          "        obj.style.display = vis2;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function toggleRowsVisibility2(value, tid, rowclass1, rowclass2)\n"
          "{\n"
          "  var vis1 = \"\";\n"
          "  var vis2 = \"\";\n"
          "  if (value == true) {\n"
          "    vis1 = \"none\";\n"
          "  } else {\n"
          "    vis2 = \"none\";\n"
          "  }\n"
          "  var tobj = document.getElementById(tid);\n"
          "  if (tobj == null) {\n"
          "    return;\n"
          "  }\n"
          "  var trows = tobj.rows;\n"
          "  if (trows != null) {\n"
          "    for (var row in trows) {\n"
          "      if (trows[row].className == rowclass1) {\n"
          "        trows[row].style.display = vis1;\n"
          "      } else if (trows[row].className == rowclass2) {\n"
          "        trows[row].style.display = vis2;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function toggleStatVisibility(value)\n"
          "{\n"
          "  toggleRowsVisibility2(value, \"UserData\", \"StatRow1\", \"StatRow2\");\n"
          "}\n"
          "function toggleFlagVisibility(value)\n"
          "{\n"
          "  toggleRowsVisibility2(value, \"UserData\", \"FlagRow1\", \"FlagRow2\");\n"
          "}\n"
          "function toggleUserInfoVisibility(value)\n"
          "{\n"
          "  toggleRowsVisibility2(value, \"UserData\", \"UserInfoRow1\", \"UserInfoRow2\");\n"
          "}\n"
          "function toggleMemberInfoVisibility(value)\n"
          "{\n"
          "  toggleRowsVisibility2(value, \"UserData\", \"MemberInfoRow1\", \"MemberInfoRow2\");\n"
          "}\n"
          "function showContestRegs()\n"
          "{\n"
          "  document.getElementById(\"ContestRegsShowLink\").style.display = \"none\";\n"
          "  document.getElementById(\"ContestRegsTable\").style.display = \"\";\n"
          "}\n"
          "function hideContestRegs()\n"
          "{\n"
          "  document.getElementById(\"ContestRegsShowLink\").style.display = \"\";\n"
          "  document.getElementById(\"ContestRegsTable\").style.display = \"none\";\n"
          "}\n"
          "function showCookies()\n"
          "{\n"
          "  document.getElementById(\"CookiesShowLink\").style.display = \"none\";\n"
          "  document.getElementById(\"CookiesTable\").style.display = \"\";\n"
          "}\n"
          "function hideCookies()\n"
          "{\n"
          "  document.getElementById(\"CookiesShowLink\").style.display = \"\";\n"
          "  document.getElementById(\"CookiesTable\").style.display = \"none\";\n"
          "}\n"
          "function checkNull(field_id)\n"
          "{\n"
          "  var form_obj = document.getElementById(\"UserForm\");\n"
          "  var checkbox_obj = form_obj[\"field_null_\" + field_id];\n"
          "  var text_obj = form_obj[\"field_\" + field_id];\n"
          "  if (checkbox_obj != null && checkbox_obj.checked) {\n"
          "    if (text_obj) text_obj.value = \"\";\n"
          "  }\n"
          "}\n"
          "function uncheckNull(field_id)\n"
          "{\n"
          "  var form_obj = document.getElementById(\"UserForm\");\n"
          "  var checkbox_obj = form_obj[\"field_null_\" + field_id];\n"
          "  var text_obj = form_obj[\"field_\" + field_id];\n"
          "  if (text_obj != null && (text_obj.value != null && text_obj.value != \"\")) {\n"
          "    if (checkbox_obj != null) checkbox_obj.checked = false;\n"
          "  }\n"
          "}\n"
          "");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!(u = get_user_info(phr, other_user_id, contest_id))) FAIL(S_ERR_DB_ERROR);
  ui = u->cnts0;

  html_start_form_id(out_f, 1, phr->self_url, "UserForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  cl = " class=\"b1\"";
  fprintf(out_f, "<table%s id=\"UserData\">\n", cl);
  fprintf(out_f, "<tr><td%s colspan=\"4\" align=\"center\">", cl);
  fprintf(out_f, "%s%s</a>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id,
                        phr->self_url, NULL,
                        "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CANCEL_AND_PREV_ACTION,
                        other_user_id, contest_id_str, group_id_str),
          "Prev user");
  fprintf(out_f, "&nbsp;%s%s</a>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id,
                        phr->self_url, NULL,
                        "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CANCEL_AND_NEXT_ACTION,
                        other_user_id, contest_id_str, group_id_str),
          "Next user");
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr><th%s width=\"250px\">&nbsp;</th><th%s><b>NULL?</b></th><th%s>&nbsp;</th><th%s>&nbsp;</th></tr>\n", cl, cl, cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>%d</td><td%s>&nbsp;</td></tr>\n",
          cl, "User ID", cl, cl, other_user_id, cl);
  s = u->login;
  if (!s) s = "";
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User login", cl, cl, 
          html_input_text(buf, sizeof(hbuf), "other_login", 50, "%s", ARMOR(s)), cl);
  s = u->email;
  if (!s) s = "";
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User e-mail", cl, cl, 
          html_input_text(buf, sizeof(buf), "email", 50, "%s", ARMOR(s)), cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
          cl, "Password", cl, cl);
  if (!u->passwd) {
    fprintf(out_f, "<i>NULL</i>");
  } else if (u->passwd_method == USERLIST_PWD_PLAIN) {
    fprintf(out_f, "<tt>%s</tt>", ARMOR(u->passwd));
  } else if (u->passwd_method == USERLIST_PWD_SHA1) {
    fprintf(out_f, "<i>Hashed with SHA1</i>");
  } else {
    fprintf(out_f, "<i>Unsupported method</i>");
  }
  fprintf(out_f, "</td><td%s>%s%s</a></td></tr>", cl,
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;next_op=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_PASSWORD_PAGE,
                        other_user_id, SSERV_OP_USER_DETAIL_PAGE, contest_id_str, group_id_str),
          "[Change]");
  fprintf(out_f, "<tr class=\"StatRow1\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleStatVisibility(true)\">[%s]</a></td></tr>\n",
          cl, "Show user statistics");
  fprintf(out_f, "<tr class=\"StatRow2\" style=\"display: none;\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleStatVisibility(false)\">[%s]</a></td></tr>\n", cl, "Hide user statistics");

  for (row = 0; user_timestamp_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr class=\"StatRow2\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
            cl, user_timestamp_rows[row].field_desc, cl, cl);
    time_t *pt = (time_t*) userlist_get_user_field_ptr(u, user_timestamp_rows[row].field_id);
    if (pt && *pt > 0) {
      fprintf(out_f, "%s</td><td%s>%s%s</a></td></tr>\n",
              xml_unparse_date(*pt), cl,
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;field_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CLEAR_FIELD_ACTION,
                            other_user_id, user_timestamp_rows[row].field_id,
                            contest_id_str, group_id_str),
              "[Reset]");
    } else if (pt) {
      fprintf(out_f, "<i>Not set</i></td><td%s>&nbsp;</td></tr>\n", cl);
    } else {
      fprintf(out_f, "<i>Invalid field</i></td><td%s>&nbsp;</td></tr>\n", cl);
    }
  }

  fprintf(out_f, "<tr class=\"FlagRow1\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleFlagVisibility(true)\">[%s]</a></td></tr>\n",
          cl, "Show user flags");
  fprintf(out_f, "<tr class=\"FlagRow2\" style=\"display: none;\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleFlagVisibility(false)\">[%s]</a></td></tr>\n", cl, "Hide user flags");

  for (row = 0; user_flag_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr class=\"FlagRow2\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
            cl, user_flag_rows[row].field_desc, cl, cl);
    int *pi = (int*) userlist_get_user_field_ptr(u, user_flag_rows[row].field_id);
    if (pi) {
      s = "";
      if (*pi > 0) {
        s = " checked=\"checked\"";
      }
      fprintf(out_f, "<input type=\"checkbox\" name=\"field_%d\" value=\"1\"%s />",
              user_flag_rows[row].field_id, s);
    } else {
      fprintf(out_f, "<i>Invalid field</i>");
    }
    fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
  }

  fprintf(out_f, "<tr><td%s align=\"center\" colspan=\"4\"><b>%s</b></td></tr>\n",
          cl, "Generic contest-specific fields");
  s = "";
  if (ui && ui->cnts_read_only > 0) s = " checked=\"checked\"";
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s><input type=\"checkbox\" name=\"field_%d\" value=\"1\"%s /></td><td%s>&nbsp;</td></tr>\n",
          cl, "User data is read-only", cl, cl, USERLIST_NC_CNTS_READ_ONLY, s, cl);
  s = "";
  s2 = 0;
  if (ui) s2 = ui->name;
  if (!s2) {
    s = " checked=\"checked\"";
    s2 = "";
  }
  snprintf(hbuf, sizeof(hbuf), "field_%d", USERLIST_NC_NAME);
  snprintf(buf2, sizeof(buf2), "uncheckNull('%d')", USERLIST_NC_NAME);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"checkNull('%d')\" name=\"field_null_%d\" value=\"1\"%s /></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User name", cl, USERLIST_NC_NAME, USERLIST_NC_NAME, s, cl, 
          html_input_text_js(buf, sizeof(buf), hbuf, 50, buf2, "%s", ARMOR(s2)), cl);
  if (contest_id > 0 && cnts && !cnts->disable_team_password) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
            cl, "Contest password", cl, cl);
    if (!ui || !ui->team_passwd) {
      fprintf(out_f, "<i>NULL</i>");
    } else if (ui->team_passwd_method == USERLIST_PWD_PLAIN) {
      fprintf(out_f, "<tt>%s</tt>", ARMOR(ui->team_passwd));
    } else if (ui->team_passwd_method == USERLIST_PWD_SHA1) {
      fprintf(out_f, "<i>Hashed with SHA1</i>");
    } else {
      fprintf(out_f, "<i>Unsupported method</i>");
    }
    fprintf(out_f, "</td><td%s>%s%s</a></td></tr>", cl,
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;contest_id=%d%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CNTS_PASSWORD_PAGE,
                          other_user_id, contest_id, group_id_str),
            "[Change]");
  }

  fprintf(out_f, "<tr class=\"UserInfoRow1\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleUserInfoVisibility(true)\">[%s]</a></td></tr>\n",
          cl, "Show more user info fields");
  fprintf(out_f, "<tr class=\"UserInfoRow2\" style=\"display: none;\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleUserInfoVisibility(false)\">[%s]</a></td></tr>\n", cl, "Hide user info fields");

  for (row = 0; user_info_rows[row].field_id > 0; ++row) {
    s = 0;
    if (user_info_rows[row].field_id == USERLIST_NC_INSTNUM) {
      if (ui && ui->instnum > 0) {
        snprintf(buf2, sizeof(buf2), "%d", ui->instnum);
        s = buf2;
      }
    } else if (ui) {
      unsigned char **ps = (unsigned char**) userlist_get_user_info_field_ptr(ui, user_info_rows[row].field_id);
      if (!ps) continue;
      s = *ps;
    }
    snprintf(hbuf, sizeof(hbuf), "%d", user_info_rows[row].field_id);
    string_row(out_f, "UserInfoRow2", 1, "b1", user_info_rows[row].field_desc, hbuf, s);
  }

  if (ui) {
    for (row = 0; user_info_stat_rows[row].field_id > 0; ++row) {
      fprintf(out_f, "<tr class=\"UserInfoRow2\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
              cl, user_info_stat_rows[row].field_desc, cl, cl);
      time_t *pt = (time_t*) userlist_get_user_info_field_ptr(ui, user_info_stat_rows[row].field_id);
      if (pt && *pt > 0) {
        fprintf(out_f, "%s</td><td%s>%s%s</a></td></tr>\n",
                xml_unparse_date(*pt), cl,
                html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                              NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;field_id=%d%s%s",
                              SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CLEAR_FIELD_ACTION,
                              other_user_id, user_info_stat_rows[row].field_id,
                              contest_id_str, group_id_str),
                "[Reset]");
      } else if (pt) {
        fprintf(out_f, "<i>Not set</i></td><td%s>&nbsp;</td></tr>\n", cl);
      } else {
        fprintf(out_f, "<i>Invalid field</i></td><td%s>&nbsp;</td></tr>\n", cl);
      }
    }
  }

  fprintf(out_f, "<tr class=\"MemberInfoRow1\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleMemberInfoVisibility(true)\">[%s]</a></td></tr>\n",
          cl, "Show members");
  fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td colspan=\"4\"%s align=\"center\"><a onclick=\"toggleMemberInfoVisibility(false)\">[%s]</a></td></tr>\n", cl, "Hide members");

  if (ui && ui->members) {
    for (role = 0; role < CONTEST_LAST_MEMBER; ++role) {
      int role_cnt = userlist_members_count(ui->members, role);
      if (role_cnt <= 0) continue;
      fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td colspan=\"4\"%s align=\"center\"><b>%s (%d)</b></td></tr>\n", cl, member_string_pl[role], role_cnt);
      for (pers = 0; pers < role_cnt; ++pers) {
        if (!(m = (struct userlist_member*) userlist_members_get_nth(ui->members, role, pers)))
          continue;

        fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td colspan=\"3\"%s align=\"center\"><b>%s %d (%d)</b></td><td%s>%s[%s]</a></tr>\n", cl, member_string[role], pers + 1, m->serial, cl,
                html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                              NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;serial=%d%s%s",
                              SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_MEMBER_PAGE,
                              other_user_id, m->serial, contest_id_str, group_id_str),
                "Delete");

        fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td%s><b>%s</b></td><td%s>&nbsp;</td><td%s>%d</td><td%s>&nbsp;</td></tr>\n",
                cl, "Member serial Id", cl, cl, m->serial, cl);

        snprintf(hbuf, sizeof(hbuf), "field_%d_%d", m->serial, USERLIST_NM_STATUS);
        fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td%s><b>%s</b></td><td%s>&nbsp;</td><td%s>",
                cl, "Status", cl, cl);
        ss_select(out_f, hbuf, (const unsigned char* []) { "Undefined", "School student", "Student", "Magistrant", "PhD student", "School teacher", "Professor", "Scientist", "Other", NULL }, m->status);
        fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
        snprintf(hbuf, sizeof(hbuf), "field_%d_%d", USERLIST_NM_GENDER, m->serial);
        fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td%s><b>%s</b></td><td%s>&nbsp;</td><td%s>",
                cl, "Status", cl, cl);
        ss_select(out_f, hbuf, (const unsigned char* []) { "Undefined", "Male", "Female", NULL }, m->gender);
        fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);

        s = 0;
        if (m->grade > 0) {
          snprintf(buf2, sizeof(buf2), "%d", m->grade);
          s = buf2;
        }
        snprintf(hbuf, sizeof(hbuf), "%d_%d", USERLIST_NM_GRADE, m->serial);
        string_row(out_f, "MemberInfoRow2", 1, "b1", "Grade", hbuf, s);

        for (row = 0; member_rows[row].field_id > 0; ++row) {
          unsigned char **ps = (unsigned char**) userlist_get_member_field_ptr(m, member_rows[row].field_id);
          if (!ps) continue;
          s = *ps;
          snprintf(hbuf, sizeof(hbuf), "%d_%d", member_rows[row].field_id, m->serial);
          string_row(out_f, "MemberInfoRow2", 1, "b1", member_rows[row].field_desc, hbuf, s);
        }

        for (row = 0; member_date_rows[row].field_id > 0; ++row) {
          time_t *pt = (time_t*) userlist_get_member_field_ptr(m, member_date_rows[row].field_id);
          if (!pt) continue;
          s = 0;
          if (*pt > 0) {
            userlist_get_member_field_str(buf2, sizeof(buf2), m, member_date_rows[row].field_id, 0, 0);
            s = buf2;
          }
          snprintf(hbuf, sizeof(hbuf), "%d_%d", member_date_rows[row].field_id, m->serial);
          string_row(out_f, "MemberInfoRow2", 1, "b1", member_date_rows[row].field_desc, hbuf, s);
        }

        for (row = 0; member_time_rows[row].field_id > 0; ++row) {
          fprintf(out_f, "<tr class=\"MemberInfoRow2\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
                  cl, member_time_rows[row].field_desc, cl, cl);
          time_t *pt = (time_t*) userlist_get_member_field_ptr(m, member_time_rows[row].field_id);
          if (pt && *pt > 0) {
            fprintf(out_f, "%s</td><td%s>%s%s</a></td></tr>\n",
                    xml_unparse_date(*pt), cl,
                    html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                                  NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;member_id=%d&amp;field_id=%d%s%s",
                                  SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CLEAR_FIELD_ACTION,
                                  other_user_id, m->serial, member_time_rows[row].field_id,
                                  contest_id_str, group_id_str),
                    "[Reset]");
          } else if (pt) {
            fprintf(out_f, "<i>Not set</i></td><td%s>&nbsp;</td></tr>\n", cl);
          } else {
            fprintf(out_f, "<i>Invalid field</i></td><td%s>&nbsp;</td></tr>\n", cl);
          }
        }
      }
    }
  }

  fprintf(out_f, "<tr><td%s colspan=\"4\" align=\"center\">", cl);
  fprintf(out_f, "<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_SAVE_AND_PREV_ACTION, "Save and goto PREV user");
  fprintf(out_f, "&nbsp;<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_SAVE_ACTION, "Save and goto user list");
  fprintf(out_f, "&nbsp;<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_SAVE_AND_NEXT_ACTION, "Save and goto NEXT user");
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr><td%s colspan=\"4\" align=\"center\">", cl);
  fprintf(out_f, "<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_CANCEL_AND_PREV_ACTION, "Cancel and goto PREV user");
  fprintf(out_f, "&nbsp;<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_CANCEL_ACTION, "Cancel and goto user list");
  fprintf(out_f, "&nbsp;<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_CANCEL_AND_NEXT_ACTION, "Cancel and goto NEXT user");
  fprintf(out_f, "</td></tr>\n");

  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  fprintf(out_f, "Create new member: ");
  ss_select(out_f, "role", (const unsigned char* []) { "", "Contestant", "Reserve", "Coach", "Advisor", "Guest", NULL }, 0);
  fprintf(out_f, "<input type=\"submit\" name=\"op_%d\" value=\"%s\" />",
          SSERV_OP_USER_CREATE_MEMBER_ACTION, "Create member");
  fprintf(out_f, "</form>\n");

  reg_count = userlist_user_count_contests(u);
  if (reg_count > 0) {
    fprintf(out_f, "<h2>%s</h2>\n", "Contest registrations");

    fprintf(out_f, "<div id=\"ContestRegsShowLink\"><p><a onclick=\"showContestRegs()\">%s</a></p></div>\n",
            "Show Contest Registrations");
    fprintf(out_f, "<div id=\"ContestRegsTable\" style=\"display: none;\"><p><a onclick=\"hideContestRegs()\">%s</a></p>\n",
            "Hide Contest Registrations");
    fprintf(out_f, "<table%s>\n", cl);
    fprintf(out_f, "<tr><th%s align=\"center\"><b>Contest Id</b></th><th%s align=\"center\"><b>Contest name</b></th>"
            "<th%s align=\"center\"><b>Status</b></th><th%s align=\"center\"><b>Flags</b></th>"
            "<th%s align=\"center\"><b>Create date</b></th><th%s align=\"center\"><b>Last change date</b></th>"
            "<th%s align=\"center\"><b>Actions</b></th></tr>\n",
            cl, cl, cl, cl, cl, cl, cl);
    for (reg = FIRST_CONTEST(u); reg; reg = NEXT_CONTEST(reg)) {
      if (contests_get(reg->id, &cnts) < 0 || !cnts) continue;
      fprintf(out_f, "<tr>");
      fprintf(out_f, "<td%s>%d</td>", cl, reg->id);
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(cnts->name));
      r = reg->status;
      if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_LAST;
      fprintf(out_f, "<td%s>%s</td>", cl, reg_status_strs[r]);
      fprintf(out_f, "<td%s>", cl);
      r = 0;
      if ((reg->flags & USERLIST_UC_INVISIBLE)) {
        if (r++) fprintf(out_f, ", ");
        fprintf(out_f, "invisible");
      }
      if ((reg->flags & USERLIST_UC_BANNED)) {
        if (r++) fprintf(out_f, ", ");
        fprintf(out_f, "banned");
      }
      if ((reg->flags & USERLIST_UC_LOCKED)) {
        if (r++) fprintf(out_f, ", ");
        fprintf(out_f, "locked");
      }
      if ((reg->flags & USERLIST_UC_INCOMPLETE)) {
        if (r++) fprintf(out_f, ", ");
        fprintf(out_f, "incomplete");
      }
      if ((reg->flags & USERLIST_UC_DISQUALIFIED)) {
        if (r++) fprintf(out_f, ", ");
        fprintf(out_f, "disqualified");
      }
      fprintf(out_f, "</td>");
      if (reg->create_time > 0) {
        fprintf(out_f, "<td%s>%s</td>", cl, xml_unparse_date(reg->create_time));
      } else {
        fprintf(out_f, "<td%s><i>Not set</i></td>", cl);
      }
      if (reg->last_change_time > 0) {
        fprintf(out_f, "<td%s>%s</td>", cl, xml_unparse_date(reg->last_change_time));
      } else {
        fprintf(out_f, "<td%s><i>Not set</i></td>", cl);
      }
      fprintf(out_f, "<td%s>", cl);
      fprintf(out_f, "%s[%s]</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;contest_id=%d",
                                  SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DETAIL_PAGE,
                                  other_user_id, reg->id),
              "User details");
      fprintf(out_f, "&nbsp;%s[%s]</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_EDIT_REG_PAGE,
                            SSERV_OP_USER_DETAIL_PAGE,
                            other_user_id, reg->id, contest_id_str, group_id_str),
              "Change");
      fprintf(out_f, "&nbsp;%s[%s]</a>",
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;next_op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_REG_PAGE,
                            SSERV_OP_USER_DETAIL_PAGE,
                            other_user_id, reg->id, contest_id_str, group_id_str),
              "Delete");

      fprintf(out_f, "</td>");
      fprintf(out_f, "</tr>\n");
    }
    fprintf(out_f, "</table>\n");
    fprintf(out_f, "<p>%s[%s]</a></p>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CREATE_REG_PAGE,
                          other_user_id),
            "Create a registration");
    fprintf(out_f, "</div>\n");
  }

  cookie_count = userlist_user_count_cookies(u);
  if (cookie_count > 0) {
    fprintf(out_f, "<h2>%s</h2>\n", "Sessions");

    fprintf(out_f, "<div id=\"CookiesShowLink\"><p><a onclick=\"showCookies()\">%s</a></p></div>\n",
            "Show Cookies");
    fprintf(out_f, "<div id=\"CookiesTable\" style=\"display: none;\"><p><a onclick=\"hideCookies()\">%s</a></p>\n",
            "Hide Cookies");
    fprintf(out_f, "<table%s>\n", cl);

    fprintf(out_f, "<tr>");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "IP address");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "SSL?");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Session ID");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Expiry time");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Contest ID");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Locale ID");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Privilege Level");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Role");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Recovery?");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Team?");
    fprintf(out_f, "<td%s align=\"center\"><b>%s</b></td>", cl, "Actions");
    fprintf(out_f, "</tr>\n");

    for (cookie=FIRST_COOKIE(u);cookie;cookie=NEXT_COOKIE(cookie)) {
      fprintf(out_f, "<tr>");
      fprintf(out_f, "<td%s>%s</td>", cl, xml_unparse_ip(cookie->ip));
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->ssl);
      fprintf(out_f, "<td%s>%016llx</td>", cl, cookie->cookie);
      fprintf(out_f, "<td%s>%s</td>", cl, xml_unparse_date(cookie->expire));
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->contest_id);
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->locale_id);
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->priv_level);
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->role);
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->recovery);
      fprintf(out_f, "<td%s>%d</td>", cl, cookie->team_login);
      fprintf(out_f, "<td%s>%s[%s]</a></td>", cl,
              html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                            NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;other_SID=%016llx%s%s",
                            SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_SESSION_ACTION,
                            other_user_id, cookie->cookie, contest_id_str, group_id_str),
              "Delete");
      fprintf(out_f, "</tr>");
    }

    fprintf(out_f, "</table>\n");
    fprintf(out_f, "<p>%s[%s]</a></p>",
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_ALL_SESSIONS_ACTION,
                          other_user_id, contest_id_str, group_id_str),
            "Delete all sessions");
    fprintf(out_f, "</div>\n");
  }

  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

static void
print_user_info(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr,
        const struct contest_desc *cnts,
        const struct userlist_user *u,
        const struct userlist_user_info *ui,
        const struct userlist_member *m,
        int role,
        int num)
{
  unsigned char *cl = " class=\"b1\"";
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *s;
  int row = 0;
  unsigned char buf[1024];

  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s width=\"250px\" colspan=\"2\" align=\"center\"><b>%s %d</b></td></tr>\n", cl,
          "User information for user", u->id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "User ID", cl, u->id);
  s = u->login;
  if (!s) s = "";
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User login", cl, ARMOR(s));
  s = u->email;
  if (!s) s = "";
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User e-mail", cl, ARMOR(s));

  /*
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
          cl, "Password", cl, cl);
  if (!u->passwd) {
    fprintf(out_f, "<i>NULL</i>");
  } else if (u->passwd_method == USERLIST_PWD_PLAIN) {
    fprintf(out_f, "<tt>%s</tt>", ARMOR(u->passwd));
  } else if (u->passwd_method == USERLIST_PWD_SHA1) {
    fprintf(out_f, "<i>Hashed with SHA1</i>");
  } else {
    fprintf(out_f, "<i>Unsupported method</i>");
  }
  fprintf(out_f, "</td><td%s>%s%s</a></td></tr>", cl,
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;next_op=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_PASSWORD_PAGE,
                        other_user_id, SSERV_OP_USER_DETAIL_PAGE, contest_id_str, group_id_str),
          "[Change]");
  */

  fprintf(out_f, "<tr><td colspan=\"2\"%s align=\"center\"><b>%s</b></td></tr>\n",
          cl, "User statistics");

  for (row = 0; user_timestamp_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>",
            cl, user_timestamp_rows[row].field_desc, cl);
    time_t *pt = (time_t*) userlist_get_user_field_ptr(u, user_timestamp_rows[row].field_id);
    if (pt && *pt > 0) {
      fprintf(out_f, "%s</td></tr>\n", xml_unparse_date(*pt));
    } else if (pt) {
      fprintf(out_f, "<i>Not set</i></td></tr>\n");
    }
  }

  for (row = 0; user_flag_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>",
            cl, user_flag_rows[row].field_desc, cl);
    int *pi = (int*) userlist_get_user_field_ptr(u, user_flag_rows[row].field_id);
    if (pi) {
      fprintf(out_f, "%s", (*pi)?"YES":"NO");
    } else {
      fprintf(out_f, "<i>Invalid field</i>");
    }
    fprintf(out_f, "</td></tr>\n");
  }

  if (!ui || !cnts) goto cleanup;

  fprintf(out_f, "<tr><td%s align=\"center\" colspan=\"2\"><b>%s %d</b></td></tr>\n",
          cl, "Contest-specific fields for contest", cnts->id);

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User data is read-only", cl, ui->cnts_read_only?"YES":"NO");
  s = 0;
  if (ui) s = ui->name;
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User name", cl, ARMOR(s));

  /*
  if (contest_id > 0 && cnts && !cnts->disable_team_password) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>&nbsp;</td><td%s>",
            cl, "Contest password", cl, cl);
    if (!ui || !ui->team_passwd) {
      fprintf(out_f, "<i>NULL</i>");
    } else if (ui->team_passwd_method == USERLIST_PWD_PLAIN) {
      fprintf(out_f, "<tt>%s</tt>", ARMOR(ui->team_passwd));
    } else if (ui->team_passwd_method == USERLIST_PWD_SHA1) {
      fprintf(out_f, "<i>Hashed with SHA1</i>");
    } else {
      fprintf(out_f, "<i>Unsupported method</i>");
    }
    fprintf(out_f, "</td><td%s>%s%s</a></td></tr>", cl,
            html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                          NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;contest_id=%d%s",
                          SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_CNTS_PASSWORD_PAGE,
                          other_user_id, contest_id, group_id_str),
            "[Change]");
  }
  */

  for (row = 0; user_info_rows[row].field_id > 0; ++row) {
    userlist_get_user_info_field_str(buf, sizeof(buf), ui, user_info_rows[row].field_id, 1);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
            cl, user_info_rows[row].field_desc, cl, ARMOR(buf));
  }

  for (row = 0; user_info_stat_rows[row].field_id > 0; ++row) {
    userlist_get_user_info_field_str(buf, sizeof(buf), ui, user_info_stat_rows[row].field_id, 1);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
            cl, user_info_stat_rows[row].field_desc, cl, ARMOR(buf));
  }
  
  if (!m) goto cleanup;

  fprintf(out_f, "<tr><td%s align=\"center\" colspan=\"2\"><b>%s %s::%d (%d)</b></td></tr>\n",
          cl, "Member", member_string[role], num + 1, m->serial);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "Member serial Id", cl, m->serial);
  userlist_get_member_field_str(buf, sizeof(buf), m, USERLIST_NM_STATUS, 1, 0);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "Status", cl, ARMOR(buf));
  userlist_get_member_field_str(buf, sizeof(buf), m, USERLIST_NM_GENDER, 1, 0);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "Gender", cl, ARMOR(buf));
  userlist_get_member_field_str(buf, sizeof(buf), m, USERLIST_NM_GRADE, 1, 0);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "Grade", cl, ARMOR(buf));

  for (row = 0; member_rows[row].field_id > 0; ++row) {
    userlist_get_member_field_str(buf, sizeof(buf), m, member_rows[row].field_id, 1, 0);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
            cl, member_rows[row].field_desc, cl, ARMOR(buf));
  }

  for (row = 0; member_date_rows[row].field_id > 0; ++row) {
    userlist_get_member_field_str(buf, sizeof(buf), m, member_date_rows[row].field_id, 1, 0);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
            cl, member_date_rows[row].field_desc, cl, ARMOR(buf));
  }

  for (row = 0; member_time_rows[row].field_id > 0; ++row) {
    userlist_get_member_field_str(buf, sizeof(buf), m, member_time_rows[row].field_id, 1, 0);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
            cl, member_time_rows[row].field_desc, cl, ARMOR(buf));
  }

  fprintf(out_f, "</table>\n");

cleanup:
  html_armor_free(&ab);
}

int
super_serve_op_USER_PASSWORD_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  unsigned char buf[1024];
  int other_user_id = -1, contest_id = -1, group_id = -1, next_op = -1;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const unsigned char *cl = 0;
  const unsigned char *s = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (!(u = get_user_info(phr, other_user_id, 0))) FAIL(S_ERR_DB_ERROR);

  opcap_t caps = 0;
  if (get_global_caps(phr, &caps) < 0) FAIL(S_ERR_PERM_DENIED);
  int cap = OPCAP_EDIT_PASSWD;
  if (is_globally_privileged(phr, u) || is_contest_privileged(cnts, u))
    cap = OPCAP_PRIV_EDIT_PASSWD;
  if (opcaps_check(caps, cap) < 0) FAIL(S_ERR_PERM_DENIED);

  snprintf(buf, sizeof(buf), "serve-control: %s, change registration password for user %d",
           phr->html_name, other_user_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function randomChar()\n"
          "{\n"
          "  var str = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\";\n"
          "  var ind = Math.floor(Math.random() * str.length);\n"
          "  if (ind < 0 || ind >= str.length) ind = 0;\n"
          "  return str.charAt(ind);\n"
          "}\n"
          "function randomString(length)\n"
          "{\n"
          "  var res = \"\";\n"
          "  for (var i = 0; i < length; ++i) {\n"
          "    res += randomChar();\n"
          "  }\n"
          "  return res;\n"
          "}\n"
          "function generateRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"PasswordForm\");\n"
          "  form_obj.reg_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"PasswordForm\");\n"
          "  form_obj.reg_password1.value = form_obj.reg_random.value;\n"
          "  form_obj.reg_password2.value = form_obj.reg_random.value;\n"
          "}\n"
          "");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!phr->userlist_clnt) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No connection to the server!</pre>\n");
    goto do_footer;
  }

  s = 0;
  if (u && u->cnts0) s = u->cnts0->name;
  if (!s) s = "";

  html_start_form_id(out_f, 1, phr->self_url, "PasswordForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  if (next_op > 0) {
    html_hidden(out_f, "next_op", "%d", next_op);
  }
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CHANGE_PASSWORD_ACTION);
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td><td%s>&nbsp;</td></tr>\n",
          cl, "User ID", cl, other_user_id, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User login", cl, ARMOR(u->login), cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User name", cl, ARMOR(s), cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s colspan=\"2\">",
          cl, "Current password", cl);
  if (!u->passwd) {
    fprintf(out_f, "<i>NULL</i>");
  } else if (u->passwd_method == USERLIST_PWD_PLAIN) {
    fprintf(out_f, "<tt>%s</tt>", ARMOR(u->passwd));
  } else if (u->passwd_method == USERLIST_PWD_SHA1) {
    fprintf(out_f, "Sha1 hash: <i>%s</i>", ARMOR(u->passwd));
  }
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"reg_password1\" size=\"20\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "New password", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"reg_password2\" size=\"20\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Confirm new password", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"reg_random\" size=\"40\" /></td><td%s><a onclick=\"generateRandomRegPassword()\">[%s]</a>&nbsp;<a onclick=\"copyRandomRegPassword()\">[%s]</a></td></tr>\n",
          cl, "Random password", cl, cl, "Generate", "Copy");
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"usesha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);
  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"%s\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, cl, "Change password", cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CNTS_PASSWORD_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  unsigned char buf[1024];
  int other_user_id = -1, contest_id = -1, group_id = -1;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const unsigned char *cl = 0;
  const unsigned char *s = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  if (ss_cgi_param_int(phr, "contest_id", &contest_id) < 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) {
      FAIL(S_ERR_INV_CONTEST);
    }
  }
  if (group_id < 0) group_id = 0;

  if (!(u = get_user_info(phr, other_user_id, contest_id))) FAIL(S_ERR_DB_ERROR);

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0;
  get_global_caps(phr, &gcaps);
  opcap_t caps = 0;
  get_contest_caps(phr, cnts, &caps);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(gcaps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0)
      FAIL(S_ERR_PERM_DENIED);
  }

  snprintf(buf, sizeof(buf), "serve-control: %s, change contest password for user %d in contest %d",
           phr->html_name, other_user_id, contest_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function randomChar()\n"
          "{\n"
          "  var str = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\";\n"
          "  var ind = Math.floor(Math.random() * str.length);\n"
          "  if (ind < 0 || ind >= str.length) ind = 0;\n"
          "  return str.charAt(ind);\n"
          "}\n"
          "function randomString(length)\n"
          "{\n"
          "  var res = \"\";\n"
          "  for (var i = 0; i < length; ++i) {\n"
          "    res += randomChar();\n"
          "  }\n"
          "  return res;\n"
          "}\n"
          "function generateRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"PasswordForm\");\n"
          "  form_obj.cnts_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"PasswordForm\");\n"
          "  form_obj.cnts_password1.value = form_obj.cnts_random.value;\n"
          "  form_obj.cnts_password2.value = form_obj.cnts_random.value;\n"
          "}\n"
          "");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!phr->userlist_clnt) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No connection to the server!</pre>\n");
    goto do_footer;
  }

  s = 0;
  if (u && u->cnts0) s = u->cnts0->name;
  if (!s) s = "";

  html_start_form_id(out_f, 1, phr->self_url, "PasswordForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CHANGE_CNTS_PASSWORD_ACTION);
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td><td%s>&nbsp;</td></tr>\n",
          cl, "User ID", cl, other_user_id, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User login", cl, ARMOR(u->login), cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
          cl, "User name", cl, ARMOR(s), cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest ID", cl, contest_id, cl);
  if (cnts) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td><td%s>&nbsp;</td></tr>\n",
            cl, "Contest name", cl, ARMOR(cnts->name), cl);
  }
  if (cnts && cnts->disable_team_password > 0) {
    fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
            cl, "Contest password is disabled");
  } else {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s colspan=\"2\">",
            cl, "Current password", cl);
    if (!u->passwd) {
      fprintf(out_f, "<i>NULL</i>");
    } else if (u->passwd_method == USERLIST_PWD_PLAIN) {
      fprintf(out_f, "<tt>%s</tt>", ARMOR(u->passwd));
    } else if (u->passwd_method == USERLIST_PWD_SHA1) {
      fprintf(out_f, "Sha1 hash: <i>%s</i>", ARMOR(u->passwd));
    }
    fprintf(out_f, "</td></tr>\n");
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"useregpasswd\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, "Copy from reg. password", cl, cl);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"settonull\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, "Set to NULL", cl, cl);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"cnts_password1\" size=\"20\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, "New password", cl, cl);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"cnts_password2\" size=\"20\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, "Confirm new password", cl, cl);
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"cnts_random\" size=\"40\" /></td><td%s><a onclick=\"generateRandomCntsPassword()\">[%s]</a>&nbsp;<a onclick=\"copyRandomCntsPassword()\">[%s]</a></td></tr>\n",
            cl, "Random password", cl, cl, "Generate", "Copy");
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"usesha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, "Use SHA1", cl, cl);
    fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"%s\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, cl, "Change password", cl);
  }
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CREATE_REG_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int other_user_id = 0, contest_id = 0, group_id = 0;
  const struct contest_desc *cnts = 0;
  unsigned char buf[1024];
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const unsigned char *cl = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const int *cnts_id_list = 0;
  int cnts_id_count, i, other_contest_id_2;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);

  snprintf(buf, sizeof(buf), "serve-control: %s, create a contest registration for user %d",
           phr->html_name, other_user_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function updateCnts1()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj1.value;\n"
          "  var i;\n"
          "  for (i = 0; i < obj2.options.length; ++i) {\n"
          "    if (obj2.options[i].value == value) {\n"
          "      obj2.options.selectedIndex = i;\n"
          "      break;\n"
          "    }\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function updateCnts2()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj2.options[obj2.selectedIndex].value;\n"
          "  obj1.value = value;\n"
          "}\n");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!(u = get_user_info(phr, other_user_id, contest_id)))
    FAIL(S_ERR_DB_ERROR);

  cnts_id_count = contests_get_list(&cnts_id_list);
  if (cnts_id_count <= 0 || !cnts_id_list) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No contests available</pre>\n");
    goto do_footer;
  }

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CREATE_REG_ACTION);
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "User ID", cl, other_user_id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User login", cl, ARMOR(u->login));
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input id=\"cnts1\" onchange=\"updateCnts1()\" type=\"text\" name=\"other_contest_id_1\" size=\"20\"/></td></tr>\n",
          cl, "Contest ID", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Contest name", cl);
  fprintf(out_f, "<select id=\"cnts2\" onchange=\"updateCnts2()\" name=\"other_contest_id_2\"><option value=\"0\"></option>");
  for (i = 0; i < cnts_id_count; ++i) {
    other_contest_id_2 = cnts_id_list[i];
    if (other_contest_id_2 <= 0) continue;
    if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
    if (cnts->closed) continue;
    fprintf(out_f, "<option value=\"%d\">%s</option>", other_contest_id_2, ARMOR(cnts->name));
  }
  fprintf(out_f, "</select>");
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
  ss_select(out_f, "status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, 1);
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Invisible?", cl, "is_invisible");
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Banned?", cl, "is_banned");
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Locked?", cl, "is_locked");
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Incomplete?", cl, "is_incomplete");
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Disqualified?", cl, "is_disqualified");
  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"Create registration\" /></td></tr>\n", cl, cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_EDIT_REG_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int other_user_id = 0, other_contest_id = 0, contest_id = 0, group_id = 0, next_op = 0;
  const struct contest_desc *cnts = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  unsigned char next_op_str[128];
  unsigned char buf[1024];
  unsigned char hbuf[1024];
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const struct userlist_contest *reg;
  const unsigned char *cl = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int reg_count = 0;
  const unsigned char *checked = " checked=\"checked\"";
  const unsigned char *s = 0;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  if (ss_cgi_param_int(phr, "other_contest_id", &other_contest_id) < 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }
  if (other_contest_id <= 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  if (contests_get(other_contest_id, &cnts) < 0 || !cnts) {
    FAIL(S_ERR_INV_CONTEST);
  }
  if (next_op != SSERV_OP_USER_DETAIL_PAGE && next_op != SSERV_OP_USER_BROWSE_PAGE) next_op = 0;
  next_op_str[0] = 0;
  if (next_op > 0) {
    snprintf(next_op_str, sizeof(next_op_str), "&amp;next_op=%d", next_op);
  }

  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_EDIT_REG;
  if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_EDIT_REG;
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  if (opcaps_check(caps, OPCAP_EDIT_REG) < 0 && opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0)
    FAIL(S_ERR_PERM_DENIED);

  snprintf(buf, sizeof(buf), "serve-control: %s, edit the contest registration for user %d, contest %d",
           phr->html_name, other_user_id, other_contest_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);
  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!phr->userlist_clnt) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No connection to the server!</pre>\n");
    goto do_footer;
  }

  if (!(u = get_user_info(phr, other_user_id, 0)))
    FAIL(S_ERR_DB_ERROR);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if ((reg_count = userlist_user_count_contests(u)) <= 0) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No contest registrations</pre>\n");
    goto do_footer;
  }
  for (reg = FIRST_CONTEST(u); reg; reg = NEXT_CONTEST(reg)) {
    if (reg->id == other_contest_id) break;
  }
  if (!reg) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>User is not registered for this contest</pre>\n");
    goto do_footer;
  }

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  html_hidden(out_f, "other_contest_id", "%d", other_contest_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  if (next_op > 0) {
    html_hidden(out_f, "next_op", "%d", next_op);
  }
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_EDIT_REG_ACTION);
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "User ID", cl, other_user_id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User login", cl, ARMOR(u->login));
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "Contest ID", cl, other_contest_id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "Contest name", cl, ARMOR(cnts->name));
  r = reg->status;
  if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_PENDING;
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
  ss_select(out_f, "status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, r);
  fprintf(out_f, "</td></tr>\n");
  s = "";
  if ((reg->flags & USERLIST_UC_INVISIBLE)) s = checked;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\"%s /></td></tr>\n",
          cl, "Invisible?", cl, "is_invisible", s);
  s = "";
  if ((reg->flags & USERLIST_UC_BANNED)) s = checked;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\"%s /></td></tr>\n",
          cl, "Banned?", cl, "is_banned", s);
  s = "";
  if ((reg->flags & USERLIST_UC_LOCKED)) s = checked;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\"%s /></td></tr>\n",
          cl, "Locked?", cl, "is_locked", s);
  s = "";
  if ((reg->flags & USERLIST_UC_INCOMPLETE)) s = checked;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\"%s /></td></tr>\n",
          cl, "Incomplete?", cl, "is_incomplete", s);
  s = "";
  if ((reg->flags & USERLIST_UC_DISQUALIFIED)) s = checked;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\"%s /></td></tr>\n",
          cl, "Disqualified?", cl, "is_disqualified", s);
  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"Save changes\" /></td></tr>\n", cl, cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  fprintf(out_f, "<p>%s[%s]</a></p>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_REG_PAGE,
                        other_user_id, other_contest_id, contest_id_str, group_id_str, next_op_str),
          "Delete");

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_DELETE_REG_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int other_user_id = 0, other_contest_id = 0, contest_id = 0, group_id = 0, next_op = 0;
  const struct contest_desc *cnts = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  unsigned char next_op_str[128];
  unsigned char buf[1024];
  unsigned char hbuf[1024];
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const struct userlist_contest *reg;
  const unsigned char *cl = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int reg_count = 0;
  const unsigned char *no = " no";
  const unsigned char *yes = " <b>YES</b>";
  const unsigned char *s = 0;

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  if (ss_cgi_param_int(phr, "other_contest_id", &other_contest_id) < 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }
  if (other_contest_id <= 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  if (contests_get(other_contest_id, &cnts) < 0 || !cnts) {
    FAIL(S_ERR_INV_CONTEST);
  }
  if (next_op != SSERV_OP_USER_BROWSE_PAGE && next_op != SSERV_OP_USER_DETAIL_PAGE) next_op = 0;
  next_op_str[0] = 0;
  if (next_op > 0) {
    snprintf(next_op_str, sizeof(next_op_str), "&amp;next_op=%d", next_op);
  }

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_DELETE_REG;
  if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_DELETE_REG;
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0 && opcaps_check(caps, OPCAP_DELETE_REG) < 0)
    FAIL(S_ERR_PERM_DENIED);

  snprintf(buf, sizeof(buf), "serve-control: %s, delete the contest registration for user %d, contest %d",
           phr->html_name, other_user_id, other_contest_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);
  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  if (!(u = get_user_info(phr, other_user_id, 0))) FAIL(S_ERR_DB_ERROR);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if ((reg_count = userlist_user_count_contests(u)) <= 0) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>No contest registrations</pre>\n");
    goto do_footer;
  }
  for (reg = FIRST_CONTEST(u); reg; reg = NEXT_CONTEST(reg)) {
    if (reg->id == other_contest_id) break;
  }
  if (!reg) {
    fprintf(out_f, "<hr/><h2>Error</h2>\n");
    fprintf(out_f, "<pre>User is not registered for this contest</pre>\n");
    goto do_footer;
  }

  html_start_form(out_f, 1, phr->self_url, "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "other_user_id", "%d", other_user_id);
  html_hidden(out_f, "other_contest_id", "%d", other_contest_id);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  if (next_op > 0) {
    html_hidden(out_f, "next_op", "%d", next_op);
  }
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_EDIT_REG_ACTION);
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "User ID", cl, other_user_id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "User login", cl, ARMOR(u->login));
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%d</td></tr>\n",
          cl, "Contest ID", cl, other_contest_id);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>\n",
          cl, "Contest name", cl, ARMOR(cnts->name));

  r = reg->status;
  if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_LAST;
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>%s</td></tr>",
          cl, "Status", cl, reg_status_strs[r]);
  s = no;
  if ((reg->flags & USERLIST_UC_INVISIBLE)) s = yes;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s>%s</td></tr>\n",
          cl, "Invisible?", cl, s);
  s = no;
  if ((reg->flags & USERLIST_UC_BANNED)) s = yes;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s>%s</td></tr>\n",
          cl, "Banned?", cl, s);
  s = no;
  if ((reg->flags & USERLIST_UC_LOCKED)) s = yes;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s>%s</td></tr>\n",
          cl, "Locked?", cl, s);
  s = no;
  if ((reg->flags & USERLIST_UC_INCOMPLETE)) s = yes;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s>%s</td></tr>\n",
          cl, "Incomplete?", cl, s);
  s = no;
  if ((reg->flags & USERLIST_UC_DISQUALIFIED)) s = yes;
  fprintf(out_f, "<tr><td%s><b>%s</td></td><td%s>%s</td></tr>\n",
          cl, "Disqualified?", cl, s);


  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"Confirm delete!\" /></td></tr>\n", cl, cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  fprintf(out_f, "<p>%s[%s]</a></p>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;other_contest_id=%d%s%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_EDIT_REG_PAGE,
                        other_user_id, other_contest_id, contest_id_str, group_id_str, next_op_str),
          "Edit");

do_footer:
  ss_write_html_footer(out_f);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CREATE_ONE_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, row, i;
  int contest_id = 0, group_id = 0, other_contest_id_2 = 0;
  const struct contest_desc *cnts = 0;
  unsigned char buf[1024], hbuf[1024];
  const unsigned char *cl = 0;
  const int *cnts_id_list = 0;
  int cnts_id_count = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *s;
  opcap_t caps = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }

  cnts_id_count = contests_get_list(&cnts_id_list);
  if (cnts_id_count <= 0 || !cnts_id_list) {
    cnts_id_count = 0;
    cnts_id_list = 0;
  }

  snprintf(buf, sizeof(buf), "serve-control: %s, create a new user",
           phr->html_name);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function changeEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.other_email.value != null && form_obj.other_email.value != \"\") {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"\";\n"
          "    changeSendEmail(form_obj);\n"
          "  } else {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"none\";\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function changeSendEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.send_email.checked) {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"\";\n"
          "  } else {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function randomChar()\n"
          "{\n"
          "  var str = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\";\n"
          "  var ind = Math.floor(Math.random() * str.length);\n"
          "  if (ind < 0 || ind >= str.length) ind = 0;\n"
          "  return str.charAt(ind);\n"
          "}\n"
          "function randomString(length)\n"
          "{\n"
          "  var res = \"\";\n"
          "  for (var i = 0; i < length; ++i) {\n"
          "    res += randomChar();\n"
          "  }\n"
          "  return res;\n"
          "}\n"
          "function generateRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_password1.value = form_obj.reg_random.value;\n"
          "  form_obj.reg_password2.value = form_obj.reg_random.value;\n"
          "}\n"
          "function generateRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_password1.value = form_obj.cnts_random.value;\n"
          "  form_obj.cnts_password2.value = form_obj.cnts_random.value;\n"
          "}\n"
          "function copyRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = form_obj.reg_random.value;\n"
          "  form_obj.cnts_password1.value = form_obj.reg_password1.value;\n"
          "  form_obj.cnts_password2.value = form_obj.reg_password2.value;\n"
          "  form_obj.cnts_sha1.checked = form_obj.reg_sha1.checked;\n"
          "}\n");
  fprintf(out_f,
          "function toggleRowsVisibility2(value, tid, rowclass1, rowclass2)\n"
          "{\n"
          "  var vis1 = \"\";\n"
          "  var vis2 = \"\";\n"
          "  if (value == true) {\n"
          "    vis1 = \"none\";\n"
          "  } else {\n"
          "    vis2 = \"none\";\n"
          "  }\n"
          "  var tobj = document.getElementById(tid);\n"
          "  if (tobj == null) {\n"
          "    return;\n"
          "  }\n"
          "  var trows = tobj.rows;\n"
          "  if (trows != null) {\n"
          "    for (var row in trows) {\n"
          "      if (trows[row].className == rowclass1) {\n"
          "        trows[row].style.display = vis1;\n"
          "      } else if (trows[row].className == rowclass2) {\n"
          "        trows[row].style.display = vis2;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function changeCntsRegCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"CntsRegRow0\", \"CntsRegRow\");\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n"
          "function changeGroupCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"GroupRow0\", \"GroupRow\");\n"
          "}\n"
          "");
  fprintf(out_f,
          "function updateCnts1()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj1.value;\n"
          "  var i;\n"
          "  for (i = 0; i < obj2.options.length; ++i) {\n"
          "    if (obj2.options[i].value == value) {\n"
          "      obj2.options.selectedIndex = i;\n"
          "      break;\n"
          "    }\n"
          "  }\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f,
          "function updateCnts2()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj2.options[obj2.selectedIndex].value;\n"
          "  obj1.value = value;\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f, "var cnts_passwd_enabled = { ");
  row = 0;
  for (i = 0; i < cnts_id_count; ++i) {
    other_contest_id_2 = cnts_id_list[i];
    if (other_contest_id_2 <= 0) continue;
    if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
    if (!cnts->disable_team_password) {
      if (row) fprintf(out_f, ", ");
      ++row;
      fprintf(out_f, "%d : true", other_contest_id_2);
    }
  }
  fprintf(out_f, "};\n");
  // CntsRegRowUseRegPasswd cnts_use_reg_passwd
  // CntsRegRowSetToNull    cnts_rull_passwd
  // CntsRegRowPasswd1      cnts_password1
  // CntsRegRowPasswd2      cnts_password2
  // CntsRegRowPasswdRandom cnts_random
  // CntsRegRowPasswdSha1   cnts_sha1
  fprintf(out_f,
          "function updateCntsPasswdVisibility()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  if (!form_obj.reg_cnts_create.checked || !cnts_passwd_enabled[form_obj.other_contest_id_1.value]) {\n"
          "    form_obj.cnts_use_reg_passwd.checked = false;\n"
          "    form_obj.cnts_null_passwd.checked = false;\n"
          "    form_obj.cnts_password1.value = \"\";\n"
          "    form_obj.cnts_password2.value = \"\";\n"
          "    form_obj.cnts_random.value = \"\";\n"
          "    form_obj.cnts_sha1.checked = false;\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswd1\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswd2\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswdRandom\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "  } else {\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"\";\n"
          "    if (form_obj.cnts_use_reg_passwd.checked) {\n"
          "      form_obj.cnts_null_passwd.checked = false;\n"
          "      form_obj.cnts_password1.value = \"\";\n"
          "      form_obj.cnts_password2.value = \"\";\n"
          "      form_obj.cnts_random.value = \"\";\n"
          "      form_obj.cnts_sha1.checked = false;\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswd1\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswd2\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswdRandom\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "    } else {\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"\";\n"
          "      if (form_obj.cnts_null_passwd.checked) {\n"
          "        form_obj.cnts_password1.value = \"\";\n"
          "        form_obj.cnts_password2.value = \"\";\n"
          "        form_obj.cnts_random.value = \"\";\n"
          "        form_obj.cnts_sha1.checked = false;\n"
          "        document.getElementById(\"CntsRegRowPasswd1\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswd2\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswdRandom\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "      } else {\n"
          "        document.getElementById(\"CntsRegRowPasswd1\").style.display = \"\";\n"
          "        document.getElementById(\"CntsRegRowPasswd2\").style.display = \"\";\n"
          "        document.getElementById(\"CntsRegRowPasswdRandom\").style.display = \"\";\n"
          "        document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"\";\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, NULL);

  html_start_form_id(out_f, 1, phr->self_url, "CreateForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CREATE_ONE_ACTION);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s id=\"CreateUserTable\">\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"text\" size=\"40\" name=\"other_login\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Login", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"text\" size=\"40\" onchange=\"changeEmail(this.form)\" name=\"other_email\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "E-mail", cl, cl);
  fprintf(out_f, "<tr id=\"SendEmailRow\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeSendEmail(this.form)\" name=\"send_email\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Send registration e-mail", cl, cl);
  fprintf(out_f, "<tr id=\"ConfirmEmailRow\" style=\"display: none;\"><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"confirm_email\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Confirm e-mail by user", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"password\" name=\"reg_password1\" size=\"40\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Registration password", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"password\" name=\"reg_password2\" size=\"40\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Confirm password", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"reg_random\" size=\"40\" /></td><td%s><a onclick=\"generateRandomRegPassword()\">[%s]</a>&nbsp;<a onclick=\"copyRandomRegPassword()\">[%s]</a></td></tr>\n",
          cl, "Random password", cl, cl, "Generate", "Copy");
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"reg_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  for (row = 0; user_flag_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"field_%d\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, user_flag_rows[row].field_desc, cl, user_flag_rows[row].field_id, cl);
  }

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Contest registration");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeCntsRegCreate(this)\" name=\"reg_cnts_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Create a contest registration", cl, cl);

  hbuf[0] = 0;
  if (contest_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", contest_id);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input id=\"cnts1\" onchange=\"updateCnts1()\" type=\"text\" name=\"other_contest_id_1\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest ID", cl, hbuf, cl);
  if (cnts_id_count > 0) {
    fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Contest name", cl);
    fprintf(out_f, "<select id=\"cnts2\" onchange=\"updateCnts2()\" name=\"other_contest_id_2\"><option value=\"0\"></option>");
    for (i = 0; i < cnts_id_count; ++i) {
      other_contest_id_2 = cnts_id_list[i];
      if (other_contest_id_2 <= 0) continue;
      if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
      if (cnts->closed) continue;
      s = "";
      if (contest_id > 0 && cnts->id == contest_id) {
        s = " selected=\"selected\"";
      }
      fprintf(out_f, "<option value=\"%d\"%s>%s</option>", other_contest_id_2, s, ARMOR(cnts->name));
    }
    fprintf(out_f, "</select>");
    fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
  ss_select(out_f, "cnts_status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, 1);
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Invisible?", cl, "is_invisible");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Banned?", cl, "is_banned");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Locked?", cl, "is_locked");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Incomplete?", cl, "is_incomplete");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Disqualified?", cl, "is_disqualified");

  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowUseRegPasswd\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" onchange=\"updateCntsPasswdVisibility()\" name=\"%s\" /></td></tr>\n",
          cl, "Use reg. password?", cl, "cnts_use_reg_passwd");
  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowSetToNull\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" onchange=\"updateCntsPasswdVisibility()\" name=\"%s\" /></td></tr>\n",
          cl, "Set to null?", cl, "cnts_null_passwd");
  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowPasswd1\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"cnts_password1\" size=\"40\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest password", cl, cl);
  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowPasswd2\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"password\" name=\"cnts_password2\" size=\"40\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Confirm password", cl, cl);
  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowPasswdRandom\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"cnts_random\" size=\"40\" /></td><td%s><a onclick=\"generateRandomCntsPassword()\">[%s]</a>&nbsp;<a onclick=\"copyRandomCntsPassword()\">[%s]</a>&nbsp;<a onclick=\"copyRegPassword()\">[%s]</a></td></tr>\n",
          cl, "Random password", cl, cl, "Generate", "Copy", "Copy reg. password");
  fprintf(out_f, "<tr class=\"CntsRegRow\" id=\"CntsRegRowPasswdSha1\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" size=\"40\" name=\"cnts_name\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "User name", cl, cl);

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Group membership");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeGroupCreate(this)\" name=\"group_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Add user to a group", cl, cl);
  hbuf[0] = 0;
  if (group_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", group_id);
  }
  fprintf(out_f, "<tr class=\"GroupRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"other_group_id\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Group ID", cl, hbuf, cl);

  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"%s\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, cl, "Create a user", cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  ss_write_html_footer(out_f);

cleanup:
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CREATE_MANY_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, row, i;
  int contest_id = 0, group_id = 0, other_contest_id_2 = 0;
  const struct contest_desc *cnts = 0;
  unsigned char buf[1024], hbuf[1024];
  const unsigned char *cl = 0;
  const int *cnts_id_list = 0;
  int cnts_id_count = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *s;
  opcap_t caps = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }

  cnts_id_count = contests_get_list(&cnts_id_list);
  if (cnts_id_count <= 0 || !cnts_id_list) {
    cnts_id_count = 0;
    cnts_id_list = 0;
  }

  snprintf(buf, sizeof(buf), "serve-control: %s, create many new users",
           phr->html_name);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\" src=\"%ssprintf.js\" ></script>\n",
          CONF_STYLE_PREFIX);
  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function changeEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.other_email.value != null && form_obj.other_email.value != \"\") {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"\";\n"
          "    changeSendEmail(form_obj);\n"
          "  } else {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"none\";\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function changeSendEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.send_email.checked) {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"\";\n"
          "  } else {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function randomChar()\n"
          "{\n"
          "  var str = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\";\n"
          "  var ind = Math.floor(Math.random() * str.length);\n"
          "  if (ind < 0 || ind >= str.length) ind = 0;\n"
          "  return str.charAt(ind);\n"
          "}\n"
          "function randomString(length)\n"
          "{\n"
          "  var res = \"\";\n"
          "  for (var i = 0; i < length; ++i) {\n"
          "    res += randomChar();\n"
          "  }\n"
          "  return res;\n"
          "}\n"
          "function generateRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_password1.value = form_obj.reg_random.value;\n"
          "  form_obj.reg_password2.value = form_obj.reg_random.value;\n"
          "}\n"
          "function generateRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_password1.value = form_obj.cnts_random.value;\n"
          "  form_obj.cnts_password2.value = form_obj.cnts_random.value;\n"
          "}\n"
          "function copyRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = form_obj.reg_random.value;\n"
          "  form_obj.cnts_password1.value = form_obj.reg_password1.value;\n"
          "  form_obj.cnts_password2.value = form_obj.reg_password2.value;\n"
          "  form_obj.cnts_sha1.checked = form_obj.reg_sha1.checked;\n"
          "}\n");
  fprintf(out_f,
          "function toggleRowsVisibility2(value, tid, rowclass1, rowclass2)\n"
          "{\n"
          "  var vis1 = \"\";\n"
          "  var vis2 = \"\";\n"
          "  if (value == true) {\n"
          "    vis1 = \"none\";\n"
          "  } else {\n"
          "    vis2 = \"none\";\n"
          "  }\n"
          "  var tobj = document.getElementById(tid);\n"
          "  if (tobj == null) {\n"
          "    return;\n"
          "  }\n"
          "  var trows = tobj.rows;\n"
          "  if (trows != null) {\n"
          "    for (var row in trows) {\n"
          "      if (trows[row].className == rowclass1) {\n"
          "        trows[row].style.display = vis1;\n"
          "      } else if (trows[row].className == rowclass2) {\n"
          "        trows[row].style.display = vis2;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function changeCntsRegCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"CntsRegRow0\", \"CntsRegRow\");\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n"
          "function changeGroupCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"GroupRow0\", \"GroupRow\");\n"
          "}\n"
          "");
  fprintf(out_f,
          "function updateCnts1()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj1.value;\n"
          "  var i;\n"
          "  for (i = 0; i < obj2.options.length; ++i) {\n"
          "    if (obj2.options[i].value == value) {\n"
          "      obj2.options.selectedIndex = i;\n"
          "      break;\n"
          "    }\n"
          "  }\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f,
          "function updateCnts2()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj2.options[obj2.selectedIndex].value;\n"
          "  obj1.value = value;\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f, "var cnts_passwd_enabled = { ");
  row = 0;
  for (i = 0; i < cnts_id_count; ++i) {
    other_contest_id_2 = cnts_id_list[i];
    if (other_contest_id_2 <= 0) continue;
    if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
    if (!cnts->disable_team_password) {
      if (row) fprintf(out_f, ", ");
      ++row;
      fprintf(out_f, "%d : true", other_contest_id_2);
    }
  }
  fprintf(out_f, "};\n");
  // CntsRegRowUseRegPasswd   cnts_use_reg_passwd
  // CntsRegRowSetToNull      cnts_null_passwd
  // CntsRegRowUseRandom      cnts_random_passwd
  // CntsRegRowPasswdTemplate cnts_password_template
  // CntsRegRowPasswdSha1     cnts_sha1
  fprintf(out_f,
          "function updateCntsPasswdVisibility()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  if (!form_obj.reg_cnts_create.checked || !cnts_passwd_enabled[form_obj.other_contest_id_1.value]) {\n"
          "    form_obj.cnts_use_reg_passwd.checked = false;\n"
          "    form_obj.cnts_null_passwd.checked = false;\n"
          "    form_obj.cnts_random_passwd.checked = false;\n"
          "    form_obj.cnts_password_template.value = \"\";\n"
          "    form_obj.cnts_sha1.checked = false;\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswdTemplate\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "  } else {\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"\";\n"
          "    if (form_obj.cnts_use_reg_passwd.checked) {\n"
          "      form_obj.cnts_null_passwd.checked = false;\n"
          "      form_obj.cnts_random_passwd.checked = false;\n"
          "      form_obj.cnts_password_template.value = \"\";\n"
          "      form_obj.cnts_sha1.checked = false;\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswdTemplate\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "    } else {\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"\";\n"
          "      if (form_obj.cnts_null_passwd.checked) {\n"
          "        form_obj.cnts_random_passwd.checked = false;\n"
          "        form_obj.cnts_password_template.value = \"\";\n"
          "        form_obj.cnts_sha1.checked = false;\n"
          "        document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswdTemplate\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "      } else {\n"
          "        document.getElementById(\"CntsRegRowUseRandom\").style.display = \"\";\n"
          "        if (form_obj.cnts_random_passwd.checked) {\n"
          "          form_obj.cnts_password_template.value = \"\";\n"
          "          form_obj.cnts_sha1.checked = false;\n"
          "          document.getElementById(\"CntsRegRowPasswdTemplate\").style.display = \"none\";\n"
          "          document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "        } else {\n"
          "          document.getElementById(\"CntsRegRowPasswdTemplate\").style.display = \"\";\n"
          "          document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"\";\n"
          "        }\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function formatLogins()\n"
          "{\n"
          "  var form_obj = document.getElementById(\"CreateForm\");\n"
          "  var div_obj = document.getElementById(\"LoginsCreated\");\n"
          "  if (div_obj.childNodes.length == 1) {\n"
          "    div_obj.removeChild(div_obj.childNodes[0]);\n"
          "  }\n"
          "  var str = \"\";\n"
          "  var first = parseInt(form_obj.first_serial.value);\n"
          "  var last = parseInt(form_obj.last_serial.value);\n"
          "  var format = form_obj.login_template.value;\n"
          "  if (first != null && first != NaN && last != null && last != NaN && first >= 0 && last >= 0 && first <= last && last - first + 1 <= 10000 && format != null && format.length > 0) {\n"
          "    if (last - first + 1 <= 5) {\n"
          "      for (var i = first; i <= last; ++i) {\n"
          "        str += \" \" + sprintf(format, i);\n"
          "      }\n"
          "    } else {\n"
          "      str += sprintf(format, first);\n"
          "      str += \" \" + sprintf(format, first + 1);\n"
          "      str += \" \" + sprintf(format, first + 2);\n"
          "      str += \" ...\";\n"
          "      str += \" \" + sprintf(format, last - 1);\n"
          "      str += \" \" + sprintf(format, last);\n"
          "    }\n"
          "  }\n"
          "  var node = document.createTextNode(str);\n"
          "  div_obj.appendChild(node);\n"
          "}\n"
          "");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, NULL);

  html_start_form_id(out_f, 1, phr->self_url, "CreateForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CREATE_MANY_ACTION);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s id=\"CreateUserTable\">\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"text\" size=\"40\" name=\"first_serial\" onchange=\"formatLogins()\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "First serial number", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"text\" size=\"40\" name=\"last_serial\" onchange=\"formatLogins()\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Last serial number", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s*:</b></td><td%s><input type=\"text\" size=\"40\" name=\"login_template\" onchange=\"formatLogins()\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Login template", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s colspan=\"2\"><div id=\"LoginsCreated\" style=\"display: inline;\"></div></td></tr>\n",
          cl, "Logins to be created", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeRandomRegPassword()\" name=\"reg_random\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use random password", cl, cl);
  fprintf(out_f, "<tr id=\"RegPasswordTemplateRow\"><td%s><b>%s:</b></td><td%s><input type=\"text\" size=\"40\" name=\"reg_password_template\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Password template", cl, cl);
  fprintf(out_f, "<tr id=\"RegPasswordSha1Row\"><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"reg_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  for (row = 0; user_flag_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"field_%d\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, user_flag_rows[row].field_desc, cl, user_flag_rows[row].field_id, cl);
  }

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Contest registration");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeCntsRegCreate(this)\" name=\"reg_cnts_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Create a contest registration", cl, cl);

  hbuf[0] = 0;
  if (contest_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", contest_id);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input id=\"cnts1\" onchange=\"updateCnts1()\" type=\"text\" name=\"other_contest_id_1\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest ID", cl, hbuf, cl);
  if (cnts_id_count > 0) {
    fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Contest name", cl);
    fprintf(out_f, "<select id=\"cnts2\" onchange=\"updateCnts2()\" name=\"other_contest_id_2\"><option value=\"0\"></option>");
    for (i = 0; i < cnts_id_count; ++i) {
      other_contest_id_2 = cnts_id_list[i];
      if (other_contest_id_2 <= 0) continue;
      if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
      if (cnts->closed) continue;
      s = "";
      if (contest_id > 0 && cnts->id == contest_id) {
        s = " selected=\"selected\"";
      }
      fprintf(out_f, "<option value=\"%d\"%s>%s</option>", other_contest_id_2, s, ARMOR(cnts->name));
    }
    fprintf(out_f, "</select>");
    fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
  ss_select(out_f, "cnts_status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, 1);
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Invisible?", cl, "is_invisible");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Banned?", cl, "is_banned");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Locked?", cl, "is_locked");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Incomplete?", cl, "is_incomplete");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Disqualified?", cl, "is_disqualified");

  fprintf(out_f, "<tr id=\"CntsRegRowUseRegPasswd\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_use_reg_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use registration password", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowSetToNull\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_null_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Set to null", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowUseRandom\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_random_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Random contest password", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowPasswdTemplate\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"cnts_password_template\" size=\"40\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest password template", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowPasswdSha1\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" size=\"40\" name=\"cnts_name_template\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "User name template", cl, cl);

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Group membership");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeGroupCreate(this)\" name=\"group_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Add user to a group", cl, cl);
  hbuf[0] = 0;
  if (group_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", group_id);
  }
  fprintf(out_f, "<tr class=\"GroupRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"other_group_id\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Group ID", cl, hbuf, cl);

  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"%s\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, cl, "Create many users", cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  ss_write_html_footer(out_f);

cleanup:
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CREATE_FROM_CSV_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, row, i;
  int contest_id = 0, group_id = 0, other_contest_id_2 = 0;
  const struct contest_desc *cnts = 0;
  unsigned char buf[1024], hbuf[1024];
  const unsigned char *cl = 0;
  const int *cnts_id_list = 0;
  int cnts_id_count = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  const unsigned char *s;
  opcap_t caps = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);

  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }

  cnts_id_count = contests_get_list(&cnts_id_list);
  if (cnts_id_count <= 0 || !cnts_id_list) {
    cnts_id_count = 0;
    cnts_id_list = 0;
  }

  snprintf(buf, sizeof(buf), "serve-control: %s, create users from a CSV file",
           phr->html_name);
  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<script language=\"javascript\">\n");
  fprintf(out_f,
          "function changeEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.other_email.value != null && form_obj.other_email.value != \"\") {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"\";\n"
          "    changeSendEmail(form_obj);\n"
          "  } else {\n"
          "    document.getElementById(\"SendEmailRow\").style.display = \"none\";\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function changeSendEmail(form_obj)\n"
          "{\n"
          "  if (form_obj.send_email.checked) {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"\";\n"
          "  } else {\n"
          "    document.getElementById(\"ConfirmEmailRow\").style.display = \"none\";\n"
          "  }\n"
          "}\n");
  fprintf(out_f,
          "function randomChar()\n"
          "{\n"
          "  var str = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\";\n"
          "  var ind = Math.floor(Math.random() * str.length);\n"
          "  if (ind < 0 || ind >= str.length) ind = 0;\n"
          "  return str.charAt(ind);\n"
          "}\n"
          "function randomString(length)\n"
          "{\n"
          "  var res = \"\";\n"
          "  for (var i = 0; i < length; ++i) {\n"
          "    res += randomChar();\n"
          "  }\n"
          "  return res;\n"
          "}\n"
          "function generateRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.reg_password1.value = form_obj.reg_random.value;\n"
          "  form_obj.reg_password2.value = form_obj.reg_random.value;\n"
          "}\n"
          "function generateRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = randomString(16);\n"
          "}\n"
          "function copyRandomCntsPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_password1.value = form_obj.cnts_random.value;\n"
          "  form_obj.cnts_password2.value = form_obj.cnts_random.value;\n"
          "}\n"
          "function copyRegPassword()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  form_obj.cnts_random.value = form_obj.reg_random.value;\n"
          "  form_obj.cnts_password1.value = form_obj.reg_password1.value;\n"
          "  form_obj.cnts_password2.value = form_obj.reg_password2.value;\n"
          "  form_obj.cnts_sha1.checked = form_obj.reg_sha1.checked;\n"
          "}\n");
  fprintf(out_f,
          "function toggleRowsVisibility2(value, tid, rowclass1, rowclass2)\n"
          "{\n"
          "  var vis1 = \"\";\n"
          "  var vis2 = \"\";\n"
          "  if (value == true) {\n"
          "    vis1 = \"none\";\n"
          "  } else {\n"
          "    vis2 = \"none\";\n"
          "  }\n"
          "  var tobj = document.getElementById(tid);\n"
          "  if (tobj == null) {\n"
          "    return;\n"
          "  }\n"
          "  var trows = tobj.rows;\n"
          "  if (trows != null) {\n"
          "    for (var row in trows) {\n"
          "      if (trows[row].className == rowclass1) {\n"
          "        trows[row].style.display = vis1;\n"
          "      } else if (trows[row].className == rowclass2) {\n"
          "        trows[row].style.display = vis2;\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n"
          "function changeCntsRegCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"CntsRegRow0\", \"CntsRegRow\");\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n"
          "function changeGroupCreate(obj)\n"
          "{\n"
          "  toggleRowsVisibility2(obj.checked, \"CreateUserTable\", \"GroupRow0\", \"GroupRow\");\n"
          "}\n"
          "");
  fprintf(out_f,
          "function updateCnts1()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj1.value;\n"
          "  var i;\n"
          "  for (i = 0; i < obj2.options.length; ++i) {\n"
          "    if (obj2.options[i].value == value) {\n"
          "      obj2.options.selectedIndex = i;\n"
          "      break;\n"
          "    }\n"
          "  }\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f,
          "function updateCnts2()\n"
          "{\n"
          "  var obj1 = document.getElementById(\"cnts1\");\n"
          "  var obj2 = document.getElementById(\"cnts2\");\n"
          "  var value = obj2.options[obj2.selectedIndex].value;\n"
          "  obj1.value = value;\n"
          "  updateCntsPasswdVisibility();\n"
          "}\n");
  fprintf(out_f, "var cnts_passwd_enabled = { ");
  row = 0;
  for (i = 0; i < cnts_id_count; ++i) {
    other_contest_id_2 = cnts_id_list[i];
    if (other_contest_id_2 <= 0) continue;
    if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
    if (!cnts->disable_team_password) {
      if (row) fprintf(out_f, ", ");
      ++row;
      fprintf(out_f, "%d : true", other_contest_id_2);
    }
  }
  fprintf(out_f, "};\n");
  // CntsRegRowUseRegPasswd   cnts_use_reg_passwd
  // CntsRegRowSetToNull      cnts_null_passwd
  // CntsRegRowUseRandom      cnts_random_passwd
  // CntsRegRowPasswdSha1     cnts_sha1
  fprintf(out_f,
          "function updateCntsPasswdVisibility()\n"
          "{\n"
          "  form_obj = document.getElementById(\"CreateForm\");\n"
          "  if (!form_obj.reg_cnts_create.checked || !cnts_passwd_enabled[form_obj.other_contest_id_1.value]) {\n"
          "    form_obj.cnts_use_reg_passwd.checked = false;\n"
          "    form_obj.cnts_null_passwd.checked = false;\n"
          "    form_obj.cnts_random_passwd.checked = false;\n"
          "    form_obj.cnts_sha1.checked = false;\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "    document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "  } else {\n"
          "    document.getElementById(\"CntsRegRowUseRegPasswd\").style.display = \"\";\n"
          "    if (form_obj.cnts_use_reg_passwd.checked) {\n"
          "      form_obj.cnts_null_passwd.checked = false;\n"
          "      form_obj.cnts_random_passwd.checked = false;\n"
          "      form_obj.cnts_sha1.checked = false;\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "      document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "    } else {\n"
          "      document.getElementById(\"CntsRegRowSetToNull\").style.display = \"\";\n"
          "      if (form_obj.cnts_null_passwd.checked) {\n"
          "        form_obj.cnts_random_passwd.checked = false;\n"
          "        form_obj.cnts_sha1.checked = false;\n"
          "        document.getElementById(\"CntsRegRowUseRandom\").style.display = \"none\";\n"
          "        document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "      } else {\n"
          "        document.getElementById(\"CntsRegRowUseRandom\").style.display = \"\";\n"
          "        if (form_obj.cnts_random_passwd.checked) {\n"
          "          form_obj.cnts_password_template.value = \"\";\n"
          "          form_obj.cnts_sha1.checked = false;\n"
          "          document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"none\";\n"
          "        } else {\n"
          "          document.getElementById(\"CntsRegRowPasswdSha1\").style.display = \"\";\n"
          "        }\n"
          "      }\n"
          "    }\n"
          "  }\n"
          "}\n");
  fprintf(out_f, "</script>\n");

  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, NULL);

  html_start_form_id(out_f, 2, phr->self_url, "CreateForm", "");
  html_hidden(out_f, "SID", "%016llx", phr->session_id);
  html_hidden(out_f, "action", "%d", SSERV_CMD_HTTP_REQUEST);
  html_hidden(out_f, "op", "%d", SSERV_OP_USER_CREATE_ONE_ACTION);
  if (contest_id > 0) {
    html_hidden(out_f, "contest_id", "%d", contest_id);
  }
  if (group_id > 0) {
    html_hidden(out_f, "group_id", "%d", group_id);
  }
  cl = " class=\"b0\"";
  fprintf(out_f, "<table%s id=\"CreateUserTable\">\n", cl);
  fprintf(out_f, "<tr id=\"SendEmailRow\"><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeSendEmail(this.form)\" name=\"send_email\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Send registration e-mail", cl, cl);
  fprintf(out_f, "<tr id=\"ConfirmEmailRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"confirm_email\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Confirm e-mail by user", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"reg_random\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use random password", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"reg_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  for (row = 0; user_flag_rows[row].field_id > 0; ++row) {
    fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"field_%d\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
            cl, user_flag_rows[row].field_desc, cl, user_flag_rows[row].field_id, cl);
  }

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Contest registration");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeCntsRegCreate(this)\" name=\"reg_cnts_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Create a contest registration", cl, cl);

  hbuf[0] = 0;
  if (contest_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", contest_id);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input id=\"cnts1\" onchange=\"updateCnts1()\" type=\"text\" name=\"other_contest_id_1\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Contest ID", cl, hbuf, cl);
  if (cnts_id_count > 0) {
    fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Contest name", cl);
    fprintf(out_f, "<select id=\"cnts2\" onchange=\"updateCnts2()\" name=\"other_contest_id_2\"><option value=\"0\"></option>");
    for (i = 0; i < cnts_id_count; ++i) {
      other_contest_id_2 = cnts_id_list[i];
      if (other_contest_id_2 <= 0) continue;
      if (contests_get(other_contest_id_2, &cnts) < 0 || !cnts) continue;
      if (cnts->closed) continue;
      s = "";
      if (contest_id > 0 && cnts->id == contest_id) {
        s = " selected=\"selected\"";
      }
      fprintf(out_f, "<option value=\"%d\"%s>%s</option>", other_contest_id_2, s, ARMOR(cnts->name));
    }
    fprintf(out_f, "</select>");
    fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
  }
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s>", cl, "Status", cl);
  ss_select(out_f, "cnts_status", (const unsigned char* []) { "OK", "Pending", "Rejected", NULL }, 1);
  fprintf(out_f, "</td></tr>\n");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Invisible?", cl, "is_invisible");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Banned?", cl, "is_banned");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Locked?", cl, "is_locked");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Incomplete?", cl, "is_incomplete");
  fprintf(out_f, "<tr class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s</td></td><td%s><input type=\"checkbox\" value=\"1\" name=\"%s\" /></td></tr>\n",
          cl, "Disqualified?", cl, "is_disqualified");

  fprintf(out_f, "<tr id=\"CntsRegRowUseRegPasswd\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_use_reg_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use registration password", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowSetToNull\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_null_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Set to null", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowUseRandom\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_random_passwd\" onchange=\"updateCntsPasswdVisibility()\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Random contest password", cl, cl);
  fprintf(out_f, "<tr id=\"CntsRegRowPasswdSha1\" class=\"CntsRegRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" name=\"cnts_sha1\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Use SHA1", cl, cl);

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "Group membership");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"checkbox\" onchange=\"changeGroupCreate(this)\" name=\"group_create\" value=\"1\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "Add user to a group", cl, cl);
  hbuf[0] = 0;
  if (group_id > 0) {
    snprintf(hbuf, sizeof(hbuf), "%d", group_id);
  }
  fprintf(out_f, "<tr class=\"GroupRow\" style=\"display: none;\" ><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"other_group_id\" size=\"20\" value=\"%s\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Group ID", cl, hbuf, cl);

  fprintf(out_f, "<tr><td%s colspan=\"3\" align=\"center\"><b>%s</b></td></tr>\n",
          cl, "File");

  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"text\" name=\"separator\" size=\"20\" value=\";\"/></td><td%s>&nbsp;</td></tr>\n",
          cl, "Field separator", cl, cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s>", cl, "Charset", cl);
  charset_html_select(out_f, NULL, NULL);
  fprintf(out_f, "</td><td%s>&nbsp;</td></tr>\n", cl);
  fprintf(out_f, "<tr><td%s><b>%s:</b></td><td%s><input type=\"file\" name=\"csv_file\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, "CSV File", cl, cl);

  fprintf(out_f, "<tr><td%s>&nbsp;</td><td%s><input type=\"submit\" name=\"submit\" value=\"%s\" /></td><td%s>&nbsp;</td></tr>\n",
          cl, cl, "Create users", cl);
  fprintf(out_f, "</table>\n");
  fprintf(out_f, "</form>\n");

  ss_write_html_footer(out_f);

cleanup:
  html_armor_free(&ab);
  return retval;
}

int
super_serve_op_USER_CHANGE_PASSWORD_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int contest_id = 0, group_id = 0, other_user_id = 0, next_op = 0, usesha1 = 0;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  opcap_t caps = 0;
  const unsigned char *s = 0;
  unsigned char *reg_password1 = 0;
  unsigned char *reg_password2 = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);
  if (contest_id != 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }

  s = 0;
  if (ss_cgi_param(phr, "reg_password1", &s) <= 0 || !s) FAIL(S_ERR_UNSPEC_PASSWD1);
  reg_password1 = fix_string(s);
  if (!reg_password1 || !*reg_password1) FAIL(S_ERR_UNSPEC_PASSWD1);
  if (strlen(reg_password1) > 1024) FAIL(S_ERR_INV_PASSWD1);
  s = 0;
  if (ss_cgi_param(phr, "reg_password2", &s) <= 0 || !s) FAIL(S_ERR_UNSPEC_PASSWD2);
  reg_password2 = fix_string(s);
  if (!reg_password2 || !*reg_password2) FAIL(S_ERR_UNSPEC_PASSWD2);
  if (strlen(reg_password2) > 1024) FAIL(S_ERR_INV_PASSWD2);
  if (strcmp(reg_password1, reg_password2) != 0) FAIL(S_ERR_PASSWDS_DIFFER);

  ss_cgi_param_int_opt(phr, "usesha1", &usesha1, 0);
  if (usesha1 != 1) usesha1 = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  if (get_global_caps(phr, &caps) < 0) FAIL(S_ERR_PERM_DENIED);
  if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) FAIL(S_ERR_INV_USER_ID);
  if (!phr->userlist_clnt) FAIL(S_ERR_NO_CONNECTION);
  r = userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, 0, &xml_text);
  if (r < 0) {
    if (r == -ULS_ERR_BAD_UID) FAIL(S_ERR_INV_USER_ID);
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);
  if (is_globally_privileged(phr, u) && opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0)
    FAIL(S_ERR_PERM_DENIED);
  else if (opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0)
    FAIL(S_ERR_PERM_DENIED);

  r = ULS_PRIV_SET_REG_PASSWD_PLAIN;
  if (usesha1) r = ULS_PRIV_SET_REG_PASSWD_SHA1;

  r = userlist_clnt_set_passwd(phr->userlist_clnt, r, other_user_id, 0, "", reg_password1);
  if (r < 0) FAIL(S_ERR_DB_ERROR);

  if (next_op == SSERV_OP_USER_DETAIL_PAGE) {
    ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, contest_id, group_id, other_user_id, NULL);
  } else {
    ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, NULL);
  }

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  xfree(reg_password1); reg_password1 = 0;
  xfree(reg_password2); reg_password2 = 0;
  return retval;
}

int
super_serve_op_USER_CHANGE_CNTS_PASSWORD_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  int contest_id = 0, group_id = 0, other_user_id = 0;
  int next_op = 0;
  const struct contest_desc *cnts = 0;
  int useregpasswd = 0, settonull = 0, usesha1 = 0;
  unsigned char *cnts_password1 = 0;
  unsigned char *cnts_password2 = 0;
  const unsigned char *s = 0;
  opcap_t gcaps = 0, ccaps = 0, fcaps = 0;
  struct userlist_user *u = 0;
  unsigned char *xml_text = 0;

  if (ss_cgi_param_int(phr, "contest_id", &contest_id) <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contest_id <= 0 || contests_get(contest_id, &cnts) < 0 || !cnts)
    FAIL(S_ERR_INV_CONTEST);

  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);

  ss_cgi_param_int_opt(phr, "useregpasswd", &useregpasswd, 0);
  if (useregpasswd != 1) useregpasswd = 0;
  ss_cgi_param_int_opt(phr, "settonull", &settonull, 0);
  if (settonull != 1) settonull = 0;
  ss_cgi_param_int_opt(phr, "usesha1", &usesha1, 0);
  if (usesha1 != 1) usesha1 = 0;

  if (!useregpasswd && !settonull) {
    s = 0;
    if (ss_cgi_param(phr, "cnts_password1", &s) <= 0 || !s) FAIL(S_ERR_UNSPEC_PASSWD1);
    cnts_password1 = fix_string(s);
    if (!cnts_password1 || !*cnts_password1) FAIL(S_ERR_UNSPEC_PASSWD1);
    if (strlen(cnts_password1) > 1024) FAIL(S_ERR_INV_PASSWD1);
    s = 0;
    if (ss_cgi_param(phr, "cnts_password2", &s) <= 0 || !s) FAIL(S_ERR_UNSPEC_PASSWD2);
    cnts_password2 = fix_string(s);
    if (!cnts_password2 || !*cnts_password2) FAIL(S_ERR_UNSPEC_PASSWD2);
    if (strlen(cnts_password2) > 1024) FAIL(S_ERR_INV_PASSWD2);
    if (strcmp(cnts_password1, cnts_password2) != 0) FAIL(S_ERR_PASSWDS_DIFFER);
  }

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  get_global_caps(phr, &gcaps);
  get_contest_caps(phr, cnts, &ccaps);
  fcaps = gcaps | ccaps;
  if (opcaps_check(fcaps, OPCAP_EDIT_PASSWD) < 0 && opcaps_check(fcaps, OPCAP_PRIV_EDIT_PASSWD) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0) FAIL(S_ERR_INV_USER_ID);
  if (!phr->userlist_clnt) FAIL(S_ERR_NO_CONNECTION);
  r = userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, contest_id, &xml_text);
  if (r < 0) {
    if (r == -ULS_ERR_BAD_UID) FAIL(S_ERR_INV_USER_ID);
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(fcaps, OPCAP_PRIV_EDIT_PASSWD) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(fcaps, OPCAP_EDIT_PASSWD) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if (settonull) {
    r = userlist_clnt_delete_field(phr->userlist_clnt, ULS_DELETE_FIELD,
                                   other_user_id, contest_id, 0,
                                   USERLIST_NC_TEAM_PASSWD);
  } else if (useregpasswd) {
    r = userlist_clnt_register_contest(phr->userlist_clnt, ULS_COPY_TO_TEAM, other_user_id,
                                       contest_id, 0, 0);
  } else {
    r = ULS_PRIV_SET_CNTS_PASSWD_PLAIN;
    if (usesha1) r = ULS_PRIV_SET_CNTS_PASSWD_SHA1;

    r = userlist_clnt_set_passwd(phr->userlist_clnt, r, other_user_id, 0, "", cnts_password1);
  }
  if (r < 0) FAIL(S_ERR_DB_ERROR);

  if (next_op == SSERV_OP_USER_DETAIL_PAGE) {
    ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, contest_id, group_id, other_user_id, NULL);
  } else {
    ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, contest_id, group_id, 0, NULL);
  }

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  xfree(cnts_password1); cnts_password1 = 0;
  xfree(cnts_password2); cnts_password2 = 0;
  return retval;
}

int
super_serve_op_USER_CREATE_ONE_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  opcap_t caps = 0;
  const struct contest_desc *cnts = 0;
  int other_user_id = 0;
  unsigned char *xml_text = 0;

  struct ss_op_param_USER_CREATE_ONE_ACTION params;
  memset(&params, 0, sizeof(params));
  retval = ss_parse_params(phr, &meta_ss_op_param_USER_CREATE_ONE_ACTION_methods, &params);
  if (retval < 0) goto cleanup;

  if (params.contest_id > 0) {
    cnts = 0;
    if (contests_get(params.contest_id, &cnts) < 0 || !cnts) {
      params.contest_id = 0;
    }
  }
  cnts = 0;
  if (params.reg_cnts_create) {
    if (contests_get(params.other_contest_id_1, &cnts) < 0 || !cnts) {
      FAIL(S_ERR_INV_CONTEST);
    }
  } else {
    params.other_contest_id_1 = 0;
  }

  if (params.group_create) {
    r = userlist_clnt_list_all_users(phr->userlist_clnt, ULS_LIST_GROUP_USERS,
                                     params.other_group_id, &xml_text);
    if (r < 0) FAIL(S_ERR_INV_GROUP_ID);
  } else {
    params.other_group_id = 0;
  }

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }
  if (cnts) {
    if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_REG) < 0) {
      FAIL(S_ERR_PERM_DENIED);
    }
  }

  if (!params.other_login || !*params.other_login) FAIL(S_ERR_UNSPEC_LOGIN);
  if (!params.reg_password1 || !*params.reg_password1) FAIL(S_ERR_UNSPEC_PASSWD1);
  if (!params.reg_password2 || !*params.reg_password2) FAIL(S_ERR_UNSPEC_PASSWD2);
  if (strcmp(params.reg_password1, params.reg_password2) != 0) FAIL(S_ERR_PASSWDS_DIFFER);
  if (params.cnts_status < 0 || params.cnts_status >= USERLIST_REG_LAST) params.cnts_status = USERLIST_REG_PENDING;
  if (cnts && !cnts->disable_team_password && !params.cnts_use_reg_passwd && !params.cnts_null_passwd) {
    if (!params.cnts_password1 || !*params.cnts_password1) FAIL(S_ERR_UNSPEC_PASSWD1);
    if (!params.cnts_password2 || !*params.cnts_password2) FAIL(S_ERR_UNSPEC_PASSWD2);
    if (strcmp(params.cnts_password1, params.cnts_password2) != 0) FAIL(S_ERR_PASSWDS_DIFFER);
  }
  if (params.other_email && *params.other_email && !is_valid_email_address(params.other_email))
    FAIL(S_ERR_INV_EMAIL);

  struct userlist_pk_create_user_2 up;
  memset(&up, 0, sizeof(up));
  up.send_email_flag = params.send_email;
  up.confirm_email_flag = params.confirm_email;
  up.use_sha1_flag = params.reg_sha1;
  up.is_privileged_flag = params.field_1;
  up.is_invisible_flag = params.field_2;
  up.is_banned_flag = params.field_3;
  up.is_locked_flag = params.field_4;
  up.show_login_flag = params.field_5;
  up.show_email_flag = params.field_6;
  up.read_only_flag = params.field_7;
  up.never_clean_flag = params.field_8;
  up.simple_registration_flag = params.field_9;
  up.contest_id = params.other_contest_id_1;
  up.cnts_status = params.cnts_status;
  up.cnts_is_invisible_flag = params.is_invisible;
  up.cnts_is_banned_flag = params.is_banned;
  up.cnts_is_locked_flag = params.is_locked;
  up.cnts_is_incomplete_flag = params.is_incomplete;
  up.cnts_is_disqualified_flag = params.is_disqualified;
  up.cnts_use_reg_passwd_flag = params.cnts_use_reg_passwd;
  up.cnts_set_null_passwd_flag = params.cnts_null_passwd;
  up.cnts_use_sha1_flag = params.cnts_sha1;
  up.group_id = params.other_group_id;

  r = userlist_clnt_create_user_2(phr->userlist_clnt, ULS_CREATE_USER_2, &up,
                                  params.other_login, params.other_email,
                                  params.reg_password1, params.cnts_password1,
                                  params.cnts_name, &other_user_id);
  if (r < 0 && r == -ULS_ERR_LOGIN_USED) {
    FAIL(S_ERR_DUPLICATED_LOGIN);
  }
  if (r < 0 || other_user_id <= 0) {
    FAIL(S_ERR_DB_ERROR);
  }

  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, params.contest_id, params.group_id, 0, NULL);

cleanup:
  xfree(xml_text); xml_text = 0;
  meta_destroy_fields(&meta_ss_op_param_USER_CREATE_ONE_ACTION_methods, &params);
  return retval;
}

static unsigned char **create_many_sorted_logins;
static int create_many_sort_func(const void *p1, const void *p2)
{
  return strcmp(*(const unsigned char**) p1, *(const unsigned char **) p2);
}

int
super_serve_op_USER_CREATE_MANY_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r, i;
  opcap_t caps = 0;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  int serial_count = 0, cur_serial;
  unsigned char **login_strs = 0;
  unsigned char buf[1024];
  unsigned char **reg_password_strs = 0;
  unsigned char **cnts_password_strs = 0;
  unsigned char **cnts_name_strs = 0;
  int other_user_id = 0;

  struct ss_op_param_USER_CREATE_MANY_ACTION params;
  memset(&params, 0, sizeof(params));
  retval = ss_parse_params(phr, &meta_ss_op_param_USER_CREATE_MANY_ACTION_methods, &params);
  if (retval < 0) goto cleanup;

  if (params.contest_id > 0) {
    cnts = 0;
    if (contests_get(params.contest_id, &cnts) < 0 || !cnts) {
      params.contest_id = 0;
    }
  }
  cnts = 0;
  if (params.reg_cnts_create) {
    if (contests_get(params.other_contest_id_1, &cnts) < 0 || !cnts) {
      FAIL(S_ERR_INV_CONTEST);
    }
  } else {
    params.other_contest_id_1 = 0;
  }

  if (params.group_create) {
    r = userlist_clnt_list_all_users(phr->userlist_clnt, ULS_LIST_GROUP_USERS,
                                     params.other_group_id, &xml_text);
    if (r < 0) FAIL(S_ERR_INV_GROUP_ID);
  } else {
    params.other_group_id = 0;
  }

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }
  if (cnts) {
    if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_REG) < 0) {
      FAIL(S_ERR_PERM_DENIED);
    }
  }

  if (params.first_serial < 0 || params.first_serial >= 1000000000) FAIL(S_ERR_INV_FIRST_SERIAL);
  if (params.last_serial < 0 || params.last_serial >= 1000000000) FAIL(S_ERR_INV_LAST_SERIAL);
  if (params.first_serial > params.last_serial) FAIL(S_ERR_INV_RANGE);
  serial_count = params.last_serial - params.first_serial + 1;
  if (serial_count > 1000) FAIL(S_ERR_INV_RANGE);
  if (!params.login_template || !*params.login_template) FAIL(S_ERR_INV_LOGIN_TEMPLATE);

  int printf_arg_types[10];
  memset(printf_arg_types, 0, sizeof(printf_arg_types));
  int printf_arg_count = parse_printf_format(params.login_template, 10, printf_arg_types);
  if (printf_arg_count != 1) FAIL(S_ERR_INV_LOGIN_TEMPLATE);
  if ((printf_arg_types[0] & ~PA_FLAG_MASK) != PA_INT) FAIL(S_ERR_INV_LOGIN_TEMPLATE);

  if (create_many_sorted_logins) {
    xfree(create_many_sorted_logins);
    create_many_sorted_logins = 0;
  }
  XCALLOC(create_many_sorted_logins, serial_count);
  XCALLOC(login_strs, serial_count);
  for (i = 0, cur_serial = params.first_serial; cur_serial <= params.last_serial; ++i, ++cur_serial) {
    snprintf(buf, sizeof(buf), params.login_template, cur_serial);
    if (strlen(buf) > 1000) FAIL(S_ERR_INV_LOGIN_TEMPLATE);
    login_strs[i] = xstrdup(buf);
    create_many_sorted_logins[i] = login_strs[i];
  }
  /* check login uniqueness */
  qsort(create_many_sorted_logins, serial_count, sizeof(create_many_sorted_logins), create_many_sort_func);
  for (i = 1; i < serial_count; ++i) {
    if (!strcmp(create_many_sorted_logins[i - 1], create_many_sorted_logins[i]))
      FAIL(S_ERR_INV_LOGIN_TEMPLATE);
  }

  XCALLOC(reg_password_strs, serial_count);
  if (!params.reg_random) {
    if (!params.reg_password_template || !*params.reg_password_template) FAIL(S_ERR_INV_REG_PASSWORD_TEMPLATE);
    memset(printf_arg_types, 0, sizeof(printf_arg_types));
    printf_arg_count = parse_printf_format(params.reg_password_template, 10, printf_arg_types);
    if (printf_arg_count != 0 && printf_arg_count != 1)
      FAIL(S_ERR_INV_REG_PASSWORD_TEMPLATE);
    if (printf_arg_count == 1 && (printf_arg_types[0] & ~PA_FLAG_MASK) != PA_INT)
      FAIL(S_ERR_INV_REG_PASSWORD_TEMPLATE);
    for (i = 0, cur_serial = params.first_serial; cur_serial <= params.last_serial; ++i, ++cur_serial) {
      snprintf(buf, sizeof(buf), params.reg_password_template, cur_serial);
      if (strlen(buf) > 1000) FAIL(S_ERR_INV_REG_PASSWORD_TEMPLATE);
      reg_password_strs[i] = xstrdup(buf);
    }
  }

  XCALLOC(cnts_password_strs, serial_count);
  if (cnts && !cnts->disable_team_password && !params.cnts_use_reg_passwd
      && !params.cnts_null_passwd && !params.cnts_random_passwd) {
    if (!params.cnts_password_template || !*params.cnts_password_template) FAIL(S_ERR_INV_CNTS_PASSWORD_TEMPLATE);
    memset(printf_arg_types, 0, sizeof(printf_arg_types));
    printf_arg_count = parse_printf_format(params.cnts_password_template, 10, printf_arg_types);
    if (printf_arg_count != 0 && printf_arg_count != 1)
      FAIL(S_ERR_INV_CNTS_PASSWORD_TEMPLATE);
    if (printf_arg_count == 1 && (printf_arg_types[0] & ~PA_FLAG_MASK) != PA_INT)
      FAIL(S_ERR_INV_CNTS_PASSWORD_TEMPLATE);
    for (i = 0, cur_serial = params.first_serial; cur_serial <= params.last_serial; ++i, ++cur_serial) {
      snprintf(buf, sizeof(buf), params.cnts_password_template, cur_serial);
      if (strlen(buf) > 1000) FAIL(S_ERR_INV_CNTS_PASSWORD_TEMPLATE);
      cnts_password_strs[i] = xstrdup(buf);
    }
  }

  XCALLOC(cnts_name_strs, serial_count);
  if (cnts) {
    if (!params.cnts_name_template || !*params.cnts_name_template) FAIL(S_ERR_INV_CNTS_NAME_TEMPLATE);
    memset(printf_arg_types, 0, sizeof(printf_arg_types));
    printf_arg_count = parse_printf_format(params.cnts_name_template, 10, printf_arg_types);
    if (printf_arg_count != 0 && printf_arg_count != 1)
      FAIL(S_ERR_INV_CNTS_NAME_TEMPLATE);
    if (printf_arg_count == 1 && (printf_arg_types[0] & ~PA_FLAG_MASK) != PA_INT)
      FAIL(S_ERR_INV_CNTS_NAME_TEMPLATE);
    for (i = 0, cur_serial = params.first_serial; cur_serial <= params.last_serial; ++i, ++cur_serial) {
      snprintf(buf, sizeof(buf), params.cnts_name_template, cur_serial);
      if (strlen(buf) > 1000) FAIL(S_ERR_INV_CNTS_NAME_TEMPLATE);
      cnts_name_strs[i] = xstrdup(buf);
    }
  }

  for (i = 0, cur_serial = params.first_serial; cur_serial <= params.last_serial; ++i, ++cur_serial) {
    struct userlist_pk_create_user_2 up;
    memset(&up, 0, sizeof(up));
    up.random_password_flag = params.reg_random;
    up.use_sha1_flag = params.reg_sha1;
    up.is_privileged_flag = params.field_1;
    up.is_invisible_flag = params.field_2;
    up.is_banned_flag = params.field_3;
    up.is_locked_flag = params.field_4;
    up.show_login_flag = params.field_5;
    up.show_email_flag = params.field_6;
    up.read_only_flag = params.field_7;
    up.never_clean_flag = params.field_8;
    up.simple_registration_flag = params.field_9;
    up.contest_id = params.other_contest_id_1;
    up.cnts_status = params.cnts_status;
    up.cnts_is_invisible_flag = params.is_invisible;
    up.cnts_is_banned_flag = params.is_banned;
    up.cnts_is_locked_flag = params.is_locked;
    up.cnts_is_incomplete_flag = params.is_incomplete;
    up.cnts_is_disqualified_flag = params.is_disqualified;
    up.cnts_use_reg_passwd_flag = params.cnts_use_reg_passwd;
    up.cnts_set_null_passwd_flag = params.cnts_null_passwd;
    up.cnts_random_password_flag = params.cnts_random_passwd;
    up.cnts_use_sha1_flag = params.cnts_sha1;
    up.group_id = params.other_group_id;
    other_user_id = 0;
    r = userlist_clnt_create_user_2(phr->userlist_clnt, ULS_CREATE_USER_2, &up,
                                    login_strs[i], NULL,
                                    reg_password_strs[i], cnts_password_strs[i],
                                    cnts_name_strs[i], &other_user_id);
    if (r < 0 && r == -ULS_ERR_LOGIN_USED) {
      FAIL(S_ERR_DUPLICATED_LOGIN);
    }
    if (r < 0 || other_user_id <= 0) {
      FAIL(S_ERR_DB_ERROR);
    }
  }

  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, params.contest_id, params.group_id, 0, NULL);

cleanup:
  if (login_strs) {
    for (i = 0; i < serial_count; ++i) xfree(login_strs[i]);
    xfree(login_strs); login_strs = 0;
  }
  if (reg_password_strs) {
    for (i = 0; i < serial_count; ++i) xfree(reg_password_strs[i]);
    xfree(reg_password_strs); reg_password_strs = 0;
  }
  if (cnts_password_strs) {
    for (i = 0; i < serial_count; ++i) xfree(cnts_password_strs[i]);
    xfree(cnts_password_strs); cnts_password_strs = 0;
  }
  if (cnts_name_strs) {
    for (i = 0; i < serial_count; ++i) xfree(cnts_name_strs[i]);
    xfree(cnts_name_strs); cnts_name_strs = 0;
  }
  xfree(create_many_sorted_logins); create_many_sorted_logins = 0;
  xfree(xml_text); xml_text = 0;
  meta_destroy_fields(&meta_ss_op_param_USER_CREATE_MANY_ACTION_methods, &params);
  return retval;
}

int
super_serve_op_USER_CREATE_FROM_CSV_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r;
  opcap_t caps = 0;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct csv_file *csv_parsed = 0;
  const unsigned char *csv_text = 0;
  unsigned char *recoded_csv_text = 0;
  const unsigned char *separator = 0;
  unsigned char *login_str = 0;
  unsigned char *email_str = 0;
  unsigned char *reg_password_str = 0;
  unsigned char *cnts_password_str = 0;
  unsigned char *cnts_name_str = 0;

  struct ss_op_param_USER_CREATE_FROM_CSV_ACTION params;
  memset(&params, 0, sizeof(params));
  retval = ss_parse_params(phr, &meta_ss_op_param_USER_CREATE_FROM_CSV_ACTION_methods, &params);
  if (retval < 0) goto cleanup;

  if (params.contest_id > 0) {
    cnts = 0;
    if (contests_get(params.contest_id, &cnts) < 0 || !cnts) {
      params.contest_id = 0;
    }
  }
  cnts = 0;
  if (params.reg_cnts_create) {
    if (contests_get(params.other_contest_id_1, &cnts) < 0 || !cnts) {
      FAIL(S_ERR_INV_CONTEST);
    }
  } else {
    params.other_contest_id_1 = 0;
  }

  if (params.group_create) {
    r = userlist_clnt_list_all_users(phr->userlist_clnt, ULS_LIST_GROUP_USERS,
                                     params.other_group_id, &xml_text);
    if (r < 0) FAIL(S_ERR_INV_GROUP_ID);
  } else {
    params.other_group_id = 0;
  }

  if (get_global_caps(phr, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_USER) < 0) {
    FAIL(S_ERR_PERM_DENIED);
  }
  if (cnts) {
    if (get_contest_caps(phr, cnts, &caps) < 0 || opcaps_check(caps, OPCAP_CREATE_REG) < 0) {
      FAIL(S_ERR_PERM_DENIED);
    }
  }

  if (ss_cgi_param(phr, "csv_file", &csv_text) <= 0 || !csv_text) {
    FAIL(S_ERR_INV_CSV_FILE);
  }

  if (params.charset && *params.charset) {
    int charset_id = charset_get_id(params.charset);
    if (charset_id < 0) FAIL(S_ERR_INV_CHARSET);
    if (charset_id > 0) {
      recoded_csv_text = charset_decode_to_heap(charset_id, csv_text);
      if (!recoded_csv_text) FAIL(S_ERR_INV_CHARSET);
      csv_text = recoded_csv_text;
    }
  }

  separator = params.separator;
  if (!separator || !*separator) separator = ";";
  if (strlen(separator) != 1) FAIL(S_ERR_INV_SEPARATOR);
  csv_parsed = csv_parse(csv_text, log_f, separator[0]);
  if (!csv_parsed) FAIL(S_ERR_INV_CSV_FILE);

  if (!csv_parsed->u) {
    fprintf(log_f, "CSV file is empty\n");
    FAIL(S_ERR_INV_CSV_FILE);
  }
  if (csv_parsed->u > 10000) {
    fprintf(log_f, "CSV file is too big\n");
    FAIL(S_ERR_INV_CSV_FILE);
  }

  // columns: login, email, reg_password (regpassword, password), cnts_password (cntspassword), name (cnts_name, cntsname)
  int login_idx = -1, email_idx = -1, reg_password_idx = -1, cnts_password_idx = -1, cnts_name_idx = -1;
  int column_count = csv_parsed->v[0].u;
  int failed = 0;
  for (int col = 0; col < column_count; ++col) {
    unsigned char *txt = fix_string(csv_parsed->v[0].v[col]);
    if (!txt || !*txt) {
      fprintf(log_f, "unidentified column %d, skipped\n", col + 1);
    } else if (!strcasecmp(txt, "login")) {
      if (login_idx >= 0) {
        fprintf(log_f, "dupicated column 'login'\n");
        failed = 1;
      } else {
        login_idx = col;
      }
    } else if (!strcasecmp(txt, "email")) {
      if (email_idx >= 0) {
        fprintf(log_f, "dupicated column 'email'\n");
        failed = 1;
      } else {
        email_idx = col;
      }
    } else if (!strcasecmp(txt, "password") || !strcasecmp(txt, "reg_password") || !strcasecmp(txt, "regpassword")) {
      if (reg_password_idx >= 0) {
        fprintf(log_f, "dupicated column 'reg_password'\n");
        failed = 1;
      } else {
        reg_password_idx = col;
      }
    } else if (!strcasecmp(txt, "cnts_password") || !strcasecmp(txt, "cntspassword")) {
      if (cnts_password_idx >= 0) {
        fprintf(log_f, "dupicated column 'cnts_password'\n");
        failed = 1;
      } else {
        cnts_password_idx = col;
      }
    } else if (!strcasecmp(txt, "cntsname") || !strcasecmp(txt, "name")) {
      if (cnts_name_idx >= 0) {
        fprintf(log_f, "dupicated column 'cnts_name'\n");
        failed = 1;
      } else {
        cnts_name_idx = col;
      }
    } else {
      fprintf(log_f, "unidentified column %d (%s), skipped\n", col + 1, txt);
    }
    xfree(txt); txt = 0;
  }
  if (login_idx < 0) {
    fprintf(log_f, "missing column 'login'\n");
    failed = 1;
  }
  if (params.send_email && email_idx < 0) {
    fprintf(log_f, "missing column 'email'\n");
    failed = 1;
  }
  if (!params.reg_random && reg_password_idx < 0) {
    fprintf(log_f, "missing column 'reg_password'\n");
    failed = 1;
  }
  if (params.reg_random) {
    reg_password_idx = -1;
  }
  if (cnts && !cnts->disable_team_password && !params.cnts_use_reg_passwd
      && !params.cnts_null_passwd && !params.cnts_random_passwd
      && cnts_password_idx < 0) {
    fprintf(log_f, "missing column 'cnts_password'\n");
    failed = 1;
  }
  if (!cnts || cnts->disable_team_password || params.cnts_use_reg_passwd
      || params.cnts_null_passwd || params.cnts_random_passwd) {
    cnts_password_idx = -1;
  }
  if (!cnts) {
    cnts_name_idx = -1;
  }
  if (failed) FAIL(S_ERR_INV_CSV_FILE);

  // dry run
  for (int row = 1; row < csv_parsed->u; ++row) {
    if (csv_parsed->v[row].u != column_count) {
      fprintf(log_f, "row %d contains %d column, but %d columns expected\n",
              row + 1, csv_parsed->v[row].u, column_count);
      failed = 1;
      continue;
    }
    unsigned char *txt = fix_string(csv_parsed->v[row].v[login_idx]);
    int user_id = 0, r = 0;
    r = userlist_clnt_lookup_user(phr->userlist_clnt, txt, 0, &user_id, NULL);
    xfree(txt); txt = 0;
    if (r < 0 && r != -ULS_ERR_INVALID_LOGIN) {
      FAIL(S_ERR_DB_ERROR);
    }
    if (r >= 0 && user_id > 0) {
      fprintf(log_f, "row %d: login '%s' already exists\n", row + 1, txt);
      failed = 1;
    }
    if (email_idx >= 0) {
      txt = fix_string(csv_parsed->v[row].v[email_idx]);
      if (txt && *txt && !is_valid_email_address(txt)) {
        fprintf(log_f, "row %d: invalid email address\n", row + 1);
        failed = 1;
      } else if (params.send_email) {
        if (!txt || !*txt) {
          fprintf(log_f, "row %d: email is not specified\n", row + 1);
          failed = 1;
        }
      }
      xfree(txt); txt = 0;
    }
    if (reg_password_idx >= 0) {
      txt = fix_string(csv_parsed->v[row].v[reg_password_idx]);
      if (!txt || !*txt) {
        fprintf(log_f, "row %d: reg_password is not specified\n", row + 1);
        failed = 1;
      }
      xfree(txt); txt = 0;
    }
    if (cnts_password_idx >= 0) {
      txt = fix_string(csv_parsed->v[row].v[cnts_password_idx]);
      if (!txt || !*txt) {
        fprintf(log_f, "row %d: cnts_password is not specified\n", row + 1);
        failed = 1;
      }
      xfree(txt); txt = 0;
    }
  }
  if (failed) FAIL(S_ERR_INV_CSV_FILE);

  for (int row = 1; row < csv_parsed->u; ++row) {
    login_str = 0;
    email_str = 0;
    reg_password_str = 0;
    cnts_password_str = 0;
    cnts_name_str = 0;

    login_str = fix_string(csv_parsed->v[row].v[login_idx]);
    if (email_idx >= 0) email_str = fix_string(csv_parsed->v[row].v[email_idx]);
    if (reg_password_idx >= 0) reg_password_str = fix_string(csv_parsed->v[row].v[reg_password_idx]);
    if (cnts_password_idx >= 0) cnts_password_str = fix_string(csv_parsed->v[row].v[cnts_password_idx]);
    if (cnts_name_idx >= 0) cnts_name_str = fix_string(csv_parsed->v[row].v[cnts_name_idx]);

    struct userlist_pk_create_user_2 up;
    int other_user_id = 0;
    memset(&up, 0, sizeof(up));
    up.random_password_flag = params.reg_random;
    up.use_sha1_flag = params.reg_sha1;
    up.is_privileged_flag = params.field_1;
    up.is_invisible_flag = params.field_2;
    up.is_banned_flag = params.field_3;
    up.is_locked_flag = params.field_4;
    up.show_login_flag = params.field_5;
    up.show_email_flag = params.field_6;
    up.read_only_flag = params.field_7;
    up.never_clean_flag = params.field_8;
    up.simple_registration_flag = params.field_9;
    up.contest_id = params.other_contest_id_1;
    up.cnts_status = params.cnts_status;
    up.cnts_is_invisible_flag = params.is_invisible;
    up.cnts_is_banned_flag = params.is_banned;
    up.cnts_is_locked_flag = params.is_locked;
    up.cnts_is_incomplete_flag = params.is_incomplete;
    up.cnts_is_disqualified_flag = params.is_disqualified;
    up.cnts_use_reg_passwd_flag = params.cnts_use_reg_passwd;
    up.cnts_set_null_passwd_flag = params.cnts_null_passwd;
    up.cnts_random_password_flag = params.cnts_random_passwd;
    up.cnts_use_sha1_flag = params.cnts_sha1;
    up.group_id = params.other_group_id;
    r = userlist_clnt_create_user_2(phr->userlist_clnt, ULS_CREATE_USER_2, &up,
                                    login_str, email_str,
                                    reg_password_str, cnts_password_str,
                                    cnts_name_str, &other_user_id);
    if (r < 0 && r == -ULS_ERR_LOGIN_USED) {
      FAIL(S_ERR_DUPLICATED_LOGIN);
    }
    if (r < 0 || other_user_id <= 0) {
      FAIL(S_ERR_DB_ERROR);
    }

    xfree(login_str); login_str = 0;
    xfree(email_str); email_str = 0;
    xfree(reg_password_str); reg_password_str = 0;
    xfree(cnts_password_str); cnts_password_str = 0;
    xfree(cnts_name_str); cnts_name_str = 0;
  }

  ss_redirect_2(out_f, phr, SSERV_OP_USER_BROWSE_PAGE, params.contest_id, params.group_id, 0, NULL);

cleanup:
  xfree(login_str); login_str = 0;
  xfree(email_str); email_str = 0;
  xfree(reg_password_str); reg_password_str = 0;
  xfree(cnts_password_str); cnts_password_str = 0;
  xfree(cnts_name_str); cnts_name_str = 0;
  csv_parsed = csv_free(csv_parsed);
  xfree(recoded_csv_text); recoded_csv_text = 0;
  xfree(xml_text); xml_text = 0;
  meta_destroy_fields(&meta_ss_op_param_USER_CREATE_FROM_CSV_ACTION_methods, &params);
  return retval;
}

int
super_serve_op_USER_SAVE_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const struct userlist_user_info *ui = 0;
  unsigned char *other_login_str = 0;
  unsigned char *email_str = 0;
  const unsigned char *s = 0;
  unsigned char param_name[64];
  opcap_t gcaps = 0;
  opcap_t caps = 0;
  int changed_ids[USERLIST_NM_LAST];
  int deleted_ids[USERLIST_NM_LAST];
  const unsigned char *changed_strs[USERLIST_NM_LAST];
  int new_cnts_read_only = 0;
  int field_id;

  unsigned char *info_fields[USERLIST_NC_LAST];
  memset(info_fields, 0, sizeof(info_fields));

  unsigned char *member_fields[USERLIST_NM_LAST];
  memset(member_fields, 0, sizeof(member_fields));

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0)
    FAIL(S_ERR_INV_USER_ID);
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts)
      FAIL(S_ERR_INV_CONTEST);
  }

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  get_global_caps(phr, &gcaps);
  if (cnts) {
    get_contest_caps(phr, cnts, &caps);
  } else {
    caps = gcaps;
  }

  if (userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, contest_id, &xml_text) < 0) {
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);

  if (ss_cgi_param(phr, "other_login", &s) <= 0 || !s) FAIL(S_ERR_UNSPEC_LOGIN);
  other_login_str = fix_string(s);
  if (ss_cgi_param(phr, "email", &s) <= 0) FAIL(S_ERR_INV_VALUE);
  email_str = fix_string(s);
  if (email_str && *email_str && !is_valid_email_address(email_str))
    FAIL(S_ERR_INV_EMAIL);

  static const int global_checkbox_ids[] =
  {
    USERLIST_NN_IS_PRIVILEGED,
    USERLIST_NN_IS_INVISIBLE,
    USERLIST_NN_IS_BANNED,
    USERLIST_NN_IS_LOCKED,
    USERLIST_NN_SHOW_LOGIN,
    USERLIST_NN_SHOW_EMAIL,
    USERLIST_NN_READ_ONLY,
    USERLIST_NN_NEVER_CLEAN,
    USERLIST_NN_SIMPLE_REGISTRATION,
    0,
  };
  int global_checkbox_vals[USERLIST_NN_LAST];
  memset(global_checkbox_vals, 0, sizeof(global_checkbox_vals));
  for (int i = 0; (field_id = global_checkbox_ids[i]); ++i) {
    snprintf(param_name, sizeof(param_name), "field_%d", field_id);
    int val = 0;
    ss_cgi_param_int_opt(phr, param_name, &val, 0);
    if (val != 1) val = 0;
    global_checkbox_vals[field_id] = val;
  }

  int changed_count = 0;
  if (strcmp(u->login, other_login_str) != 0) {
    ++changed_count;
  }
  if (strcmp(u->email, email_str) != 0) {
    ++changed_count;
  }
  for (int i = 0; global_checkbox_ids[i]; ++i) {
    const void *ptr = userlist_get_user_field_ptr(u, global_checkbox_ids[i]);
    if (ptr) {
      int ival = *(const int*) ptr;
      if (ival != global_checkbox_vals[i]) {
        ++changed_count;
      }
    }
  }
  if (changed_count > 0) {
    int bit = 0;
    if (is_globally_privileged(phr, u)
        || (cnts && is_contest_privileged(cnts, u))
        || global_checkbox_vals[USERLIST_NN_IS_PRIVILEGED] != u->is_privileged) {
      bit = OPCAP_PRIV_EDIT_USER;
    } else {
      bit = OPCAP_EDIT_USER;
    }
    if (opcaps_check(gcaps, bit) < 0) FAIL(S_ERR_PERM_DENIED);

    changed_count = 0;
    if (strcmp(u->login, other_login_str) != 0) {
      changed_ids[changed_count] = USERLIST_NN_LOGIN;
      changed_strs[changed_count] = other_login_str;
      ++changed_count;
    }
    if (strcmp(u->email, email_str) != 0) {
      changed_ids[changed_count] = USERLIST_NN_EMAIL;
      changed_strs[changed_count] = email_str;
      ++changed_count;
    }
    for (int i = 0; (field_id = global_checkbox_ids[i]); ++i) {
      const void *ptr = userlist_get_user_field_ptr(u, field_id);
      if (ptr) {
        int ival = *(const int*) ptr;
        if (ival != global_checkbox_vals[field_id]) {
          changed_ids[changed_count] = field_id;
          changed_strs[changed_count] = ival?"1":"0";
          ++changed_count;
        }
      }
    }

    if (userlist_clnt_edit_field_seq(phr->userlist_clnt, ULS_EDIT_FIELD_SEQ,
                                     other_user_id, contest_id, 0, 0, changed_count,
                                     NULL, changed_ids, changed_strs) < 0) {
      FAIL(S_ERR_DB_ERROR);
    }
  }

  static const int info_field_ids[] =
  {
    // USERLIST_NC_CNTS_READ_ONLY,
    USERLIST_NC_NAME,
    USERLIST_NC_INST,
    USERLIST_NC_INST_EN,
    // 105
    USERLIST_NC_INSTSHORT,
    USERLIST_NC_INSTSHORT_EN,
    USERLIST_NC_INSTNUM,
    USERLIST_NC_FAC,
    USERLIST_NC_FAC_EN,
    // 110
    USERLIST_NC_FACSHORT,
    USERLIST_NC_FACSHORT_EN,
    USERLIST_NC_HOMEPAGE,
    USERLIST_NC_CITY,
    USERLIST_NC_CITY_EN,
    // 115
    USERLIST_NC_COUNTRY,
    USERLIST_NC_COUNTRY_EN,
    USERLIST_NC_REGION,
    USERLIST_NC_AREA,
    USERLIST_NC_ZIP,
    // 120
    USERLIST_NC_STREET,
    USERLIST_NC_LOCATION,
    USERLIST_NC_SPELLING,
    USERLIST_NC_PRINTER_NAME,
    USERLIST_NC_EXAM_ID,
    // 125
    USERLIST_NC_EXAM_CYPHER,
    USERLIST_NC_LANGUAGES,
    USERLIST_NC_PHONE,
    USERLIST_NC_FIELD0,
    USERLIST_NC_FIELD1,
    // 130
    USERLIST_NC_FIELD2,
    USERLIST_NC_FIELD3,
    USERLIST_NC_FIELD4,
    USERLIST_NC_FIELD5,
    USERLIST_NC_FIELD6,
    // 135
    USERLIST_NC_FIELD7,
    USERLIST_NC_FIELD8,
    USERLIST_NC_FIELD9,
    0,
  };
  int info_null_fields[USERLIST_NC_LAST];
  memset(info_null_fields, 0, sizeof(info_null_fields));
  for (int i = 0; (field_id = info_field_ids[i]); ++i) {
    snprintf(param_name, sizeof(param_name), "field_null_%d", field_id);
    int val = 0;
    ss_cgi_param_int_opt(phr, param_name, &val, 0);
    if (val != 1) val = 0;
    info_null_fields[field_id] = val;
  }

  snprintf(param_name, sizeof(param_name), "field_%d", USERLIST_NC_CNTS_READ_ONLY);
  ss_cgi_param_int_opt(phr, param_name, &new_cnts_read_only, 0);
  if (new_cnts_read_only != 1) new_cnts_read_only = 0;

  int cnts_read_only = 0;
  if (ui && ui->cnts_read_only) cnts_read_only = 1;

  for (int i = 0; (field_id = info_field_ids[i]); ++i) {
    if (info_null_fields[field_id]) continue;
    snprintf(param_name, sizeof(param_name), "field_%d", field_id);
    s = 0;
    if (ss_cgi_param(phr, param_name, &s) < 0) FAIL(S_ERR_INV_VALUE);
    if (!s) s = "";
    info_fields[field_id] = fix_string(s);
  }

  if (u) ui = u->cnts0;
  int is_changed = 0;
  for (int i = 0; (field_id = info_field_ids[i]); ++i) {
    if (info_null_fields[field_id]) {
      if (ui && !userlist_is_empty_user_info_field(ui, field_id)) is_changed = 1;
    } else if (info_fields[field_id]) {
      if (!ui || !userlist_is_equal_user_info_field(ui, field_id, info_fields[field_id])) is_changed = 1;
    }
  }

  int deleted_count = 0;
  changed_count = 0;
  if (is_changed) {
    if (cnts_read_only && new_cnts_read_only) FAIL(S_ERR_DATA_READ_ONLY);
    if (cnts_read_only != new_cnts_read_only) {
      changed_ids[changed_count] = USERLIST_NC_CNTS_READ_ONLY;
      changed_strs[changed_count] = new_cnts_read_only?"1":"0";
      ++changed_count;
    }
    for (int i = 0; (field_id = info_field_ids[i]); ++i) {
      if (info_null_fields[field_id]) {
        if (ui && !userlist_is_empty_user_info_field(ui, field_id)) {
          deleted_ids[deleted_count] = field_id;
          ++deleted_count;
        }
      } else if (info_fields[field_id]) {
        if (!ui || !userlist_is_equal_user_info_field(ui, field_id, info_fields[field_id])) {
          changed_ids[changed_count] = field_id;
          changed_strs[changed_count] = info_fields[field_id];
          ++changed_count;
        }
      }
    }
  }

  if (deleted_count > 0 || changed_count > 0) {
    if (is_globally_privileged(phr, u)) {
      if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0 || opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0)
        FAIL(S_ERR_PERM_DENIED);
    } else if (is_contest_privileged(cnts, u)) {
      if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
    } else {
      if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
    }

    if (userlist_clnt_edit_field_seq(phr->userlist_clnt, ULS_EDIT_FIELD_SEQ,
                                     other_user_id, contest_id, 0, deleted_count, changed_count,
                                     deleted_ids, changed_ids, changed_strs) < 0) {
      FAIL(S_ERR_DB_ERROR);
    }
  }

  static const int member_field_ids[] = 
  {
    USERLIST_NM_STATUS,
    USERLIST_NM_GENDER,
    USERLIST_NM_GRADE,
    USERLIST_NM_FIRSTNAME,
    /* 205 */
    USERLIST_NM_FIRSTNAME_EN,
    USERLIST_NM_MIDDLENAME,
    USERLIST_NM_MIDDLENAME_EN,
    USERLIST_NM_SURNAME,
    USERLIST_NM_SURNAME_EN,
    /* 210 */
    USERLIST_NM_GROUP,
    USERLIST_NM_GROUP_EN,
    USERLIST_NM_EMAIL,
    USERLIST_NM_HOMEPAGE,
    USERLIST_NM_OCCUPATION,
    /* 215 */
    USERLIST_NM_OCCUPATION_EN,
    USERLIST_NM_DISCIPLINE,
    USERLIST_NM_INST,
    USERLIST_NM_INST_EN,
    USERLIST_NM_INSTSHORT,
    /* 220 */
    USERLIST_NM_INSTSHORT_EN,
    USERLIST_NM_FAC,
    USERLIST_NM_FAC_EN,
    USERLIST_NM_FACSHORT,
    USERLIST_NM_FACSHORT_EN,
    /* 225 */
    USERLIST_NM_PHONE,
    USERLIST_NM_CREATE_TIME,
    USERLIST_NM_LAST_CHANGE_TIME,
    USERLIST_NM_BIRTH_DATE,
    USERLIST_NM_ENTRY_DATE,
    /* 230 */
    USERLIST_NM_GRADUATION_DATE,
    0,
  };
  int member_null_fields[USERLIST_NM_LAST];

  if (ui && ui->members) {
    for (int role = 0; role < CONTEST_LAST_MEMBER; ++role) {
      int role_cnt = userlist_members_count(ui->members, role);
      if (role_cnt <= 0) continue;
      for (int pers = 0; pers < role_cnt; ++pers) {
        const struct userlist_member *m;
        if (!(m = (const struct userlist_member*) userlist_members_get_nth(ui->members, role, pers)))
          continue;

        memset(member_null_fields, 0, sizeof(member_null_fields));
        memset(member_fields, 0, sizeof(member_fields));

        for (int i = 0; (field_id = member_field_ids[i]); ++i) {
          snprintf(param_name, sizeof(param_name), "field_null_%d_%d", field_id, m->serial);
          int val = 0;
          ss_cgi_param_int_opt(phr, param_name, &val, 0);
          if (val != 1) val = 0;
          member_null_fields[field_id] = val;
        }

        for (int i = 0; (field_id = member_field_ids[i]); ++i) {
          if (member_null_fields[field_id]) continue;
          snprintf(param_name, sizeof(param_name), "field_%d_%d", field_id, m->serial);
          int r = ss_cgi_param(phr, param_name, &s);
          if (!r || !s) continue;
          if (r < 0) FAIL(S_ERR_INV_VALUE);
          if (!s) s = "";
          member_fields[field_id] = fix_string(s);
        }

        is_changed = 0;
        for (int i = 0; (field_id = member_field_ids[i]); ++i) {
          if (member_null_fields[field_id]) {
            if (!userlist_is_empty_member_field(m, field_id)) is_changed = 1;
          } else if (member_fields[field_id]) {
            if (!userlist_is_equal_member_field(m, field_id, member_fields[field_id])) is_changed = 1;
          }
        }

        deleted_count = 0;
        changed_count = 0;
        if (is_changed) {
          if (cnts_read_only && new_cnts_read_only) FAIL(S_ERR_DATA_READ_ONLY);

          for (int i = 0; (field_id = member_field_ids[i]); ++i) {
            if (member_null_fields[field_id]) {
              if (!userlist_is_empty_member_field(m, field_id)) {
                deleted_ids[deleted_count] = field_id;
                ++deleted_count;
              }
            } else if (member_fields[field_id]){
              if (!userlist_is_equal_member_field(m, field_id, member_fields[field_id])) {
                changed_ids[changed_count] = field_id;
                changed_strs[changed_count] = member_fields[field_id];
                ++changed_count;
              }
            }
          }
        }

        if (deleted_count > 0 || changed_count > 0) {
          if (is_globally_privileged(phr, u)) {
            if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0 || opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0)
              FAIL(S_ERR_PERM_DENIED);
          } else if (is_contest_privileged(cnts, u)) {
            if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
          } else {
            if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
          }

          if (userlist_clnt_edit_field_seq(phr->userlist_clnt, ULS_EDIT_FIELD_SEQ,
                                           other_user_id, contest_id, m->serial, deleted_count, changed_count,
                                           deleted_ids, changed_ids, changed_strs) < 0) {
            FAIL(S_ERR_DB_ERROR);
          }
        }

        for (int i = USERLIST_NM_FIRST; i < USERLIST_NM_LAST; ++i) {
          xfree(member_fields[i]);
        }
        memset(member_fields, 0, sizeof(member_fields));
      }
    }
  }

  int next_user_id = 0;
  int next_op = SSERV_OP_USER_DETAIL_PAGE;
  if (phr->opcode == SSERV_OP_USER_SAVE_AND_PREV_ACTION) {
    userlist_clnt_get_prev_user_id(phr->userlist_clnt, ULS_PREV_USER, contest_id, group_id, other_user_id,
                                   NULL, &next_user_id);
  } else if (phr->opcode == SSERV_OP_USER_SAVE_AND_NEXT_ACTION) {
    userlist_clnt_get_prev_user_id(phr->userlist_clnt, ULS_NEXT_USER, contest_id, group_id, other_user_id,
                                   NULL, &next_user_id);
  } else {
    next_op = SSERV_OP_USER_BROWSE_PAGE;
  }
  if (next_user_id <= 0) next_op = SSERV_OP_USER_BROWSE_PAGE;

  ss_redirect_2(out_f, phr, next_op, contest_id, group_id, next_user_id, NULL);

cleanup:
  for (int i = 0; i < USERLIST_NC_LAST; ++i) {
    xfree(info_fields[i]); info_fields[i] = 0;
  }
  for (int i = USERLIST_NM_FIRST; i < USERLIST_NM_LAST; ++i) {
    xfree(member_fields[i]); member_fields[i] = 0;
  }
  xfree(email_str); email_str = 0;
  xfree(other_login_str); other_login_str = 0;
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  return retval;
}

int
super_serve_op_USER_CANCEL_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0;
  const struct contest_desc *cnts = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "other_user_id", &other_user_id, 0);
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts)
      FAIL(S_ERR_INV_CONTEST);
  }
  if (other_user_id <= 0) phr->opcode = SSERV_OP_USER_CANCEL_ACTION;

  int next_user_id = 0;
  int next_op = SSERV_OP_USER_DETAIL_PAGE;
  if (phr->opcode == SSERV_OP_USER_CANCEL_AND_PREV_ACTION) {
    userlist_clnt_get_prev_user_id(phr->userlist_clnt, ULS_PREV_USER, contest_id, group_id, other_user_id,
                                   NULL, &next_user_id);
  } else if (phr->opcode == SSERV_OP_USER_CANCEL_AND_NEXT_ACTION) {
    userlist_clnt_get_prev_user_id(phr->userlist_clnt, ULS_NEXT_USER, contest_id, group_id, other_user_id,
                                   NULL, &next_user_id);
  } else {
    next_op = SSERV_OP_USER_BROWSE_PAGE;
  }
  if (next_user_id <= 0) next_op = SSERV_OP_USER_BROWSE_PAGE;

  ss_redirect_2(out_f, phr, next_op, contest_id, group_id, next_user_id, NULL);

cleanup:
  return retval;
}

int
super_serve_op_USER_CREATE_MEMBER_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0, role = 0;
  const struct contest_desc *cnts = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  opcap_t gcaps = 0, caps = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "other_user_id", &other_user_id, 0);
  ss_cgi_param_int_opt(phr, "role", &role, -1);

  if (contest_id <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(contest_id, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);
  if (group_id < 0) group_id = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  get_global_caps(phr, &gcaps);
  get_contest_caps(phr, cnts, &caps);
  caps = (caps | gcaps) & ((1L << OPCAP_EDIT_USER) | (1L << OPCAP_PRIV_EDIT_USER));
  if (!caps) FAIL(S_ERR_PERM_DENIED);

  if (userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, contest_id, &xml_text) < 0) {
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);
  --role;
  if (role < 0 || role >= USERLIST_MB_LAST) FAIL(S_ERR_INV_VALUE);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  int max_count = 0;
  if (role == CONTEST_M_CONTESTANT) {
    if (cnts->personal) {
      max_count = 1;
    } else if (cnts->members[role]) {
      max_count = cnts->members[role]->max_count;
    }
  } else if (role == CONTEST_M_RESERVE) {
    if (cnts->personal) {
      max_count = 0;
    } else if (cnts->members[role]) {
      max_count = cnts->members[role]->max_count;
    }
  } else {
    if (cnts->members[role]) {
      max_count = cnts->members[role]->max_count;
    }
  }

  int cur_count = 0;
  if (u && u->cnts0 && u->cnts0->members) {
    cur_count = userlist_members_count(u->cnts0->members, role);
  }
  if (cur_count >= max_count) FAIL(S_ERR_TOO_MANY_MEMBERS);

  if (userlist_clnt_create_member(phr->userlist_clnt, other_user_id, contest_id, role) < 0)
    FAIL(S_ERR_DB_ERROR);

  ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, contest_id, group_id, other_user_id, NULL);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  return retval;
}

int
super_serve_op_USER_DELETE_MEMBER_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int other_user_id = 0, contest_id = 0, group_id = 0, serial = 0;
  const struct contest_desc *cnts = 0;
  unsigned char contest_id_str[128];
  unsigned char group_id_str[128];
  opcap_t gcaps = 0, caps = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  struct userlist_member *m = 0;
  int role = 0, num = 0;
  unsigned char buf[1024];
  unsigned char hbuf[1024];

  if (ss_cgi_param_int(phr, "other_user_id", &other_user_id) < 0 || other_user_id <= 0) {
    FAIL(S_ERR_INV_USER_ID);
  }
  if (ss_cgi_param_int(phr, "serial", &serial) < 0 || serial <= 0) {
    FAIL(S_ERR_INV_SERIAL);
  }
  if (ss_cgi_param_int(phr, "contest_id", &contest_id) < 0 || contest_id <= 0) {
    FAIL(S_ERR_INV_CONTEST);
  }
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) {
      FAIL(S_ERR_INV_CONTEST);
    }
  }
  contest_id_str[0] = 0;
  if (contest_id > 0) {
    snprintf(contest_id_str, sizeof(contest_id_str), "&amp;contest_id=%d", contest_id);
  }
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  if (group_id < 0) group_id = 0;
  group_id_str[0] = 0;
  if (group_id > 0) {
    snprintf(group_id_str, sizeof(group_id_str), "&amp;group_id=%d", group_id);
  }

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  get_global_caps(phr, &gcaps);
  get_contest_caps(phr, cnts, &caps);
  caps = (caps | gcaps) & ((1L << OPCAP_EDIT_USER) | (1L << OPCAP_PRIV_EDIT_USER));
  if (!caps) FAIL(S_ERR_PERM_DENIED);

  if (userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, contest_id, &xml_text) < 0) {
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  m = 0;
  if (u->cnts0 && u->cnts0->members) {
    m = userlist_get_member_nc(u->cnts0->members, serial, &role, &num);
  }
  if (!m) FAIL(S_ERR_INV_SERIAL);
  if (role < 0 || role >= USERLIST_MB_LAST || num < 0) FAIL(S_ERR_INV_SERIAL);

  snprintf(buf, sizeof(buf), "serve-control: %s, delete the member '%s'::%d (%d) of user %d, contest %d",
           phr->html_name, member_string[role], num + 1, serial,
           other_user_id, contest_id);
  ss_write_html_header(out_f, phr, buf, 0, NULL);
  fprintf(out_f, "<h1>%s</h1>\n<br/>\n", buf);

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, other_user_id, NULL);

  print_user_info(log_f, out_f, phr, cnts, u, u->cnts0, m, role, num);

  fprintf(out_f, "<p>%s[%s]</a>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DETAIL_PAGE,
                        other_user_id, contest_id_str, group_id_str),
          "Cancel");

  fprintf(out_f, "&nbsp;%s[%s]</a></p>",
          html_hyperref(hbuf, sizeof(hbuf), phr->session_id, phr->self_url,
                        NULL, "action=%d&amp;op=%d&amp;other_user_id=%d&amp;serial=%d%s%s",
                        SSERV_CMD_HTTP_REQUEST, SSERV_OP_USER_DELETE_MEMBER_ACTION,
                        other_user_id, m->serial, contest_id_str, group_id_str),
          "Delete");

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  return retval;
}

int
super_serve_op_USER_DELETE_MEMBER_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0, serial = 0;
  const struct contest_desc *cnts = 0;
  opcap_t gcaps = 0, caps = 0;
  unsigned char *xml_text = 0;
  struct userlist_user *u = 0;
  const struct userlist_member *m = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "other_user_id", &other_user_id, 0);
  ss_cgi_param_int_opt(phr, "serial", &serial, 0);

  if (contest_id <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(contest_id, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);
  if (group_id < 0) group_id = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  get_global_caps(phr, &gcaps);
  get_contest_caps(phr, cnts, &caps);
  caps = (caps | gcaps) & ((1L << OPCAP_EDIT_USER) | (1L << OPCAP_PRIV_EDIT_USER));
  if (!caps) FAIL(S_ERR_PERM_DENIED);

  if (userlist_clnt_get_info(phr->userlist_clnt, ULS_PRIV_GET_USER_INFO,
                             other_user_id, contest_id, &xml_text) < 0) {
    FAIL(S_ERR_DB_ERROR);
  }
  if (!(u = userlist_parse_user_str(xml_text))) FAIL(S_ERR_DB_ERROR);

  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  m = 0;
  if (u->cnts0 && u->cnts0->members) {
    m = userlist_get_member_nc(u->cnts0->members, serial, NULL, NULL);
  }
  if (!m) FAIL(S_ERR_INV_SERIAL);

  if (userlist_clnt_delete_info(phr->userlist_clnt, ULS_PRIV_DELETE_MEMBER,
                                other_user_id, contest_id, serial) < 0)
    FAIL(S_ERR_DB_ERROR);

  ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, contest_id, group_id, other_user_id, NULL);

cleanup:
  userlist_free(&u->b); u = 0;
  xfree(xml_text); xml_text = 0;
  return retval;
}

int
super_serve_op_USER_CREATE_REG_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  struct userlist_user *u = 0;

  struct ss_op_param_USER_CREATE_REG_ACTION params;
  memset(&params, 0, sizeof(params));
  retval = ss_parse_params(phr, &meta_ss_op_param_USER_CREATE_REG_ACTION_methods, &params);
  if (retval < 0) goto cleanup;

  const struct contest_desc *cnts = 0;
  if (params.contest_id > 0) {
    if (contests_get(params.contest_id, &cnts) < 0 || !cnts)
      params.contest_id = 0;
  } else {
    params.contest_id = 0;
  }
  cnts = 0;
  if (params.other_contest_id_1 <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(params.other_contest_id_1, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);

  if (params.group_id < 0) params.group_id = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_CREATE_REG;
  if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_CREATE_REG;
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (opcaps_check(caps, OPCAP_CREATE_REG) < 0 && opcaps_check(caps, OPCAP_PRIV_CREATE_REG) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (!(u = get_user_info(phr, params.other_user_id, cnts->id))) FAIL(S_ERR_DB_ERROR);
  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_CREATE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_CREATE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_CREATE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if (params.status < 0 || params.status >= USERLIST_REG_LAST) params.status = USERLIST_REG_PENDING;

  int flags = 0;
  if (params.is_invisible) flags |= USERLIST_UC_INVISIBLE;
  if (params.is_banned) flags |= USERLIST_UC_BANNED;
  if (params.is_locked) flags |= USERLIST_UC_LOCKED;
  if (params.is_incomplete) flags |= USERLIST_UC_INCOMPLETE;
  if (params.is_disqualified) flags |= USERLIST_UC_DISQUALIFIED;

  if (userlist_clnt_register_contest(phr->userlist_clnt,
                                     ULS_PRIV_REGISTER_CONTEST,
                                     params.other_user_id, cnts->id, 0, 0) < 0)
    FAIL(S_ERR_DB_ERROR);
  if (userlist_clnt_change_registration(phr->userlist_clnt, params.other_user_id,
                                        cnts->id, params.status, 4, flags) < 0)
    FAIL(S_ERR_DB_ERROR);

  ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, params.contest_id, params.group_id, params.other_user_id, NULL);

cleanup:
  meta_destroy_fields(&meta_ss_op_param_USER_CREATE_REG_ACTION_methods, &params);
  userlist_free(&u->b); u = 0;
  return retval;
}

int
super_serve_op_USER_EDIT_REG_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  struct userlist_user *u = 0;

  struct ss_op_param_USER_EDIT_REG_ACTION params;
  memset(&params, 0, sizeof(params));
  retval = ss_parse_params(phr, &meta_ss_op_param_USER_EDIT_REG_ACTION_methods, &params);
  if (retval < 0) goto cleanup;

  const struct contest_desc *cnts = 0;
  if (params.contest_id > 0) {
    if (contests_get(params.contest_id, &cnts) < 0 || !cnts)
      params.contest_id = 0;
  } else {
    params.contest_id = 0;
  }
  cnts = 0;
  if (params.other_contest_id <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(params.other_contest_id, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);

  if (params.group_id < 0) params.group_id = 0;

  if (params.next_op != SSERV_OP_USER_DETAIL_PAGE) params.next_op = SSERV_OP_USER_BROWSE_PAGE;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_EDIT_REG;
  if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_EDIT_REG;
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (opcaps_check(caps, OPCAP_CREATE_REG) < 0 && opcaps_check(caps, OPCAP_PRIV_CREATE_REG) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (!(u = get_user_info(phr, params.other_user_id, cnts->id))) FAIL(S_ERR_DB_ERROR);
  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if (params.status < 0 || params.status >= USERLIST_REG_LAST) params.status = USERLIST_REG_PENDING;

  int flags = 0;
  if (params.is_invisible) flags |= USERLIST_UC_INVISIBLE;
  if (params.is_banned) flags |= USERLIST_UC_BANNED;
  if (params.is_locked) flags |= USERLIST_UC_LOCKED;
  if (params.is_incomplete) flags |= USERLIST_UC_INCOMPLETE;
  if (params.is_disqualified) flags |= USERLIST_UC_DISQUALIFIED;

  if (userlist_clnt_change_registration(phr->userlist_clnt, params.other_user_id,
                                        cnts->id, params.status, 4, flags) < 0)
    FAIL(S_ERR_DB_ERROR);

  if (params.next_op == SSERV_OP_USER_DETAIL_PAGE) {
    ss_redirect_2(out_f, phr, params.next_op, params.contest_id, params.group_id, params.other_user_id, NULL);
  } else {
    ss_redirect_2(out_f, phr, params.next_op, params.contest_id, params.group_id, 0, NULL);
  }

cleanup:
  meta_destroy_fields(&meta_ss_op_param_USER_EDIT_REG_ACTION_methods, &params);
  userlist_free(&u->b); u = 0;
  return retval;
}

int
super_serve_op_USER_DELETE_REG_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  struct userlist_user *u = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0, other_contest_id = 0, next_op = 0;
  const struct contest_desc *cnts = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "other_user_id", &other_user_id, 0);
  ss_cgi_param_int_opt(phr, "other_contest_id", &other_contest_id, 0);
  ss_cgi_param_int_opt(phr, "next_op", &next_op, 0);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts)
      contest_id = 0;
  }
  if (group_id < 0) group_id = 0;
  if (next_op != SSERV_OP_USER_DETAIL_PAGE) next_op = SSERV_OP_USER_BROWSE_PAGE;

  cnts = 0;
  if (other_contest_id <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(other_contest_id, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  if (opcaps_check(gcaps, OPCAP_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_DELETE_REG;
  if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) >= 0) gcaps |= 1LL << OPCAP_PRIV_DELETE_REG;
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0 && opcaps_check(caps, OPCAP_DELETE_REG) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (!(u = get_user_info(phr, other_user_id, cnts->id))) FAIL(S_ERR_DB_ERROR);
  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_DELETE_REG) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if (userlist_clnt_change_registration(phr->userlist_clnt, u->id, cnts->id, -2, 0, 0) < 0)
    FAIL(S_ERR_DB_ERROR);

  if (next_op == SSERV_OP_USER_DETAIL_PAGE) {
    ss_redirect_2(out_f, phr, next_op, contest_id, group_id, other_user_id, NULL);
  } else {
    ss_redirect_2(out_f, phr, next_op, contest_id, group_id, 0, NULL);
  }

cleanup:
  userlist_free(&u->b); u = 0;
  return retval;
}

static const unsigned char clearable_fields[] =
{
  [USERLIST_NN_IS_PRIVILEGED] = 1,
  [USERLIST_NN_IS_INVISIBLE] = 1,
  [USERLIST_NN_IS_BANNED] = 1,
  [USERLIST_NN_IS_LOCKED] = 1,
  [USERLIST_NN_SHOW_LOGIN] = 1,
  [USERLIST_NN_SHOW_EMAIL] = 1,
  [USERLIST_NN_READ_ONLY] = 1,
  [USERLIST_NN_NEVER_CLEAN] = 1,
  [USERLIST_NN_SIMPLE_REGISTRATION] = 1,
  [USERLIST_NN_EMAIL] = 1,

  [USERLIST_NC_CNTS_READ_ONLY] = 1,
  [USERLIST_NC_NAME] = 1,
  [USERLIST_NC_INST] = 1,
  [USERLIST_NC_INST_EN] = 1,
  [USERLIST_NC_INSTSHORT] = 1,
  [USERLIST_NC_INSTSHORT_EN] = 1,
  [USERLIST_NC_INSTNUM] = 1,
  [USERLIST_NC_FAC] = 1,
  [USERLIST_NC_FAC_EN] = 1,
  [USERLIST_NC_FACSHORT] = 1,
  [USERLIST_NC_FACSHORT_EN] = 1,
  [USERLIST_NC_HOMEPAGE] = 1,
  [USERLIST_NC_CITY] = 1,
  [USERLIST_NC_CITY_EN] = 1,
  [USERLIST_NC_COUNTRY] = 1,
  [USERLIST_NC_COUNTRY_EN] = 1,
  [USERLIST_NC_REGION] = 1,
  [USERLIST_NC_AREA] = 1,
  [USERLIST_NC_ZIP] = 1,
  [USERLIST_NC_STREET] = 1,
  [USERLIST_NC_LOCATION] = 1,
  [USERLIST_NC_SPELLING] = 1,
  [USERLIST_NC_PRINTER_NAME] = 1,
  [USERLIST_NC_EXAM_ID] = 1,
  [USERLIST_NC_EXAM_CYPHER] = 1,
  [USERLIST_NC_LANGUAGES] = 1,
  [USERLIST_NC_PHONE] = 1,
  [USERLIST_NC_FIELD0] = 1,
  [USERLIST_NC_FIELD1] = 1,
  [USERLIST_NC_FIELD2] = 1,
  [USERLIST_NC_FIELD3] = 1,
  [USERLIST_NC_FIELD4] = 1,
  [USERLIST_NC_FIELD5] = 1,
  [USERLIST_NC_FIELD6] = 1,
  [USERLIST_NC_FIELD7] = 1,
  [USERLIST_NC_FIELD8] = 1,
  [USERLIST_NC_FIELD9] = 1,

  [USERLIST_NM_STATUS] = 1,
  [USERLIST_NM_GENDER] = 1,
  [USERLIST_NM_GRADE] = 1,
  [USERLIST_NM_FIRSTNAME] = 1,
  [USERLIST_NM_FIRSTNAME_EN] = 1,
  [USERLIST_NM_MIDDLENAME] = 1,
  [USERLIST_NM_MIDDLENAME_EN] = 1,
  [USERLIST_NM_SURNAME] = 1,
  [USERLIST_NM_SURNAME_EN] = 1,
  [USERLIST_NM_GROUP] = 1,
  [USERLIST_NM_GROUP_EN] = 1,
  [USERLIST_NM_EMAIL] = 1,
  [USERLIST_NM_HOMEPAGE] = 1,
  [USERLIST_NM_OCCUPATION] = 1,
  [USERLIST_NM_OCCUPATION_EN] = 1,
  [USERLIST_NM_DISCIPLINE] = 1,
  [USERLIST_NM_INST] = 1,
  [USERLIST_NM_INST_EN] = 1,
  [USERLIST_NM_INSTSHORT] = 1,
  [USERLIST_NM_INSTSHORT_EN] = 1,
  [USERLIST_NM_FAC] = 1,
  [USERLIST_NM_FAC_EN] = 1,
  [USERLIST_NM_FACSHORT] = 1,
  [USERLIST_NM_FACSHORT_EN] = 1,
  [USERLIST_NM_PHONE] = 1,
  [USERLIST_NM_BIRTH_DATE] = 1,
  [USERLIST_NM_ENTRY_DATE] = 1,
  [USERLIST_NM_GRADUATION_DATE] = 1,
};

int
super_serve_op_USER_CLEAR_FIELD_ACTION(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  int contest_id = 0, group_id = 0, other_user_id = 0, field_id = 0, member_id = 0;
  const struct contest_desc *cnts = 0;
  struct userlist_user *u = 0;
  const struct userlist_member *m = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  ss_cgi_param_int_opt(phr, "other_user_id", &other_user_id, 0);
  ss_cgi_param_int_opt(phr, "field_id", &field_id, 0);
  ss_cgi_param_int_opt(phr, "member_id", &member_id, 0);

  if (contest_id <= 0) FAIL(S_ERR_INV_CONTEST);
  if (contests_get(contest_id, &cnts) < 0 || !cnts) FAIL(S_ERR_INV_CONTEST);
  if (group_id < 0) group_id = 0;

  if (phr->priv_level <= 0) FAIL(S_ERR_PERM_DENIED);
  opcap_t gcaps = 0, caps = 0;
  get_global_caps(phr, &gcaps);
  get_contest_caps(phr, cnts, &caps);
  caps |= gcaps;
  if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0 && opcaps_check(caps, OPCAP_EDIT_USER) < 0)
    FAIL(S_ERR_PERM_DENIED);

  if (!(u = get_user_info(phr, other_user_id, cnts->id))) FAIL(S_ERR_DB_ERROR);
  if (is_globally_privileged(phr, u)) {
    if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else if (is_contest_privileged(cnts, u)) {
    if (opcaps_check(caps, OPCAP_PRIV_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  } else {
    if (opcaps_check(caps, OPCAP_EDIT_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  }

  if (field_id < USERLIST_NN_FIRST || field_id >= USERLIST_NM_LAST)
    FAIL(S_ERR_INV_VALUE);
  if (!clearable_fields[field_id])
    FAIL(S_ERR_INV_VALUE);

  if (field_id >= USERLIST_NM_FIRST && field_id < USERLIST_NM_LAST) {
    if (u->cnts0 && u->cnts0->members) {
      m = userlist_get_member_nc(u->cnts0->members, field_id, NULL, NULL);
    }
    if (!m) {
      member_id = 0;
    }
  } else {
    member_id = 0;
  }

  if (userlist_clnt_delete_field(phr->userlist_clnt, ULS_DELETE_FIELD,
                                 other_user_id, contest_id, member_id,
                                 field_id) < 0) {
    FAIL(S_ERR_DB_ERROR);
  }

  ss_redirect_2(out_f, phr, SSERV_OP_USER_DETAIL_PAGE, contest_id, group_id, other_user_id, NULL);

cleanup:
  userlist_free(&u->b); u = 0;
  return retval;
}

int
super_serve_op_USER_SEL_VIEW_PASSWD_PAGE(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0, r = 0;
  int contest_id = 0, group_id = 0;
  unsigned char *marked_str = 0;
  bitset_t marked = BITSET_INITIALIZER;
  const struct contest_desc *cnts = 0;
  opcap_t gcaps = 0, caps = 0;
  unsigned char *xml_text = 0;
  struct userlist_list *users = 0;
  unsigned char buf[1024];
  const unsigned char *s = 0;
  const unsigned char *cl = 0;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int user_id = 0, serial;
  const struct userlist_user *u = 0;
  const struct userlist_contest *reg = 0;
  const struct userlist_user_info *ui = 0;
  int allowed, passwd_method;
  const unsigned char *passwd;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);

  if (contest_id < 0) contest_id = 0;
  if (contest_id > 0) {
    if (contests_get(contest_id, &cnts) < 0 || !cnts) contest_id = 0;
  }
  if (group_id < 0) group_id = 0;

  /* check permissions */
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_VIEW_PASSWD_PAGE:
    get_global_caps(phr, &gcaps);
    if (cnts) get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    break;
  case SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_PAGE:
    if (!cnts) FAIL(S_ERR_INV_CONTEST);
    if (cnts->disable_team_password) FAIL(S_ERR_INV_CONTEST);
    get_global_caps(phr, &gcaps);
    get_contest_caps(phr, cnts, &caps);
    caps |= gcaps;
    break;
  default:
    abort();
  }
  if (opcaps_check(caps, OPCAP_GET_USER) < 0) FAIL(S_ERR_PERM_DENIED);
  if (opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) < 0 && opcaps_check(caps, OPCAP_EDIT_PASSWD) < 0) FAIL(S_ERR_PERM_DENIED);

  if (!phr->userlist_clnt) FAIL(S_ERR_DB_ERROR);
  r = userlist_clnt_list_users_2(phr->userlist_clnt, ULS_LIST_ALL_USERS_4,
                                 contest_id, group_id, marked_str, 0, 0,
                                 &xml_text);

  if (r < 0) FAIL(S_ERR_DB_ERROR);
  users = userlist_parse_str(xml_text);
  if (!users) FAIL(S_ERR_DB_ERROR);

  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_VIEW_PASSWD_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, view registration passwords", phr->html_name);
    break;
  case SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_PAGE:
    snprintf(buf, sizeof(buf), "serve-control: %s, view contest passwords in contest %d", phr->html_name, contest_id);
    break;
  default:
    abort();
  }

  ss_write_html_header(out_f, phr, buf, 0, NULL);

  fprintf(out_f, "<h1>%s</h1>\n", buf);
  if (cnts && cnts->name) {
    fprintf(out_f, "<h2>Contest %d: %s</h2>\n", cnts->id, ARMOR(cnts->name));
  } else {
    fprintf(out_f, "<br/>\n");
  }

  print_top_navigation_links(log_f, out_f, phr, contest_id, group_id, 0, marked_str);

  cl = " class=\"b1\"";
  fprintf(out_f, "<table%s>", cl);
  fprintf(out_f, "<tr>");
  fprintf(out_f, "<th%s>%s</th><th%s>%s</th><th%s>%s</th>",
          cl, "NN", cl, "User ID", cl, "Login");
  s = "Registration password";
  if (phr->opcode == SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_PAGE) s = "Contest password";
  fprintf(out_f, "<th%s>%s</th>", cl, s);
  if (cnts) {
    fprintf(out_f, "<th%s>%s</th><th%s>%s</th><th%s>%s</th><th%s>%s</th><th%s>%s</th>",
            cl, "Name", cl, "Status", cl, "Flags", cl, "Location", cl, "Printer name");
  }
  fprintf(out_f, "</tr>\n");
  for (user_id = 1, serial = 0; user_id < marked.size; ++user_id) {
    if (!bitset_get(&marked, user_id)) continue;
    if (user_id >= users->user_map_size) continue;
    if (!(u = users->user_map[user_id])) continue;
    ui = u->cnts0;
    reg = 0;
    if (cnts) {
      reg = userlist_get_user_contest(u, contest_id);
      if (!reg) continue;
    }
    fprintf(out_f, "<tr><td%s>%d</td>", cl, ++serial);
    fprintf(out_f, "<td%s>%d</td>", cl, user_id);
    fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(u->login));
    fprintf(out_f, "<td%s>", cl);
    allowed = 0;
    passwd_method = -1;
    passwd = 0;
    switch (phr->opcode) {
    case SSERV_OP_USER_SEL_VIEW_PASSWD_PAGE:
      if (is_globally_privileged(phr, u)) {
        if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) >= 0) allowed = 1;
      } else if (cnts && is_contest_privileged(cnts, u)) {
        if (opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) >= 0) allowed = 1;
      } else {
        if (opcaps_check(caps, OPCAP_EDIT_PASSWD) >= 0) allowed = 1;
      }
      if (allowed) {
        passwd_method = u->passwd_method;
        passwd = u->passwd;
      }
      break;
    case SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_PAGE:
      if (is_globally_privileged(phr, u)) {
        if (opcaps_check(gcaps, OPCAP_PRIV_EDIT_PASSWD) >= 0) allowed = 1;
      } else if (is_contest_privileged(cnts, u)) {
        if (opcaps_check(caps, OPCAP_PRIV_EDIT_PASSWD) >= 0) allowed = 1;
      } else {
        if (opcaps_check(caps, OPCAP_EDIT_PASSWD) >= 0) allowed = 1;
      }
      if (allowed && ui) {
        passwd_method = ui->team_passwd_method;
        passwd = ui->team_passwd;
      }
      break;
    default:
      break;
    }
    if (!allowed) {
      fprintf(out_f, "<i>hidden</i>");
    } else if (passwd_method < 0 || !passwd) {
      fprintf(out_f, "<i>null</i>");
    } else if (passwd_method == USERLIST_PWD_SHA1) {
      fprintf(out_f, "<i>changed</i>");
    } else if (passwd_method == USERLIST_PWD_PLAIN) {
      fprintf(out_f, "<tt>%s</tt>", ARMOR(passwd));
    } else {
      fprintf(out_f, "<i>unknown</i>");
    }
    fprintf(out_f, "</td>");
    if (cnts) {
      s = u->login;
      if (ui && ui->name && *ui->name) s = ui->name;
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(s));
      if (cnts && reg) {
        r = reg->status;
        if (r < 0 || r >= USERLIST_REG_LAST) r = USERLIST_REG_LAST;
        fprintf(out_f, "<td%s>%s</td>", cl, reg_status_strs[r]);
      } else {
        fprintf(out_f, "<td%s>&nbsp;</td>", cl);
      }
      fprintf(out_f, "<td%s>", cl);
      s = "";
      if (is_privileged(phr, cnts, u)) {
        fprintf(out_f, "%s%s", s, "privileged");
        s = ", ";
      }
      if (cnts && reg) {
        if ((reg->flags & USERLIST_UC_INVISIBLE)) {
          fprintf(out_f, "%s%s", s, "invisible");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_BANNED)) {
          fprintf(out_f, "%s%s", s, "banned");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_LOCKED)) {
          fprintf(out_f, "%s%s", s, "locked");
          s = ", ";
        }
        if ((reg->flags & USERLIST_UC_DISQUALIFIED)) {
          fprintf(out_f, "%s%s", s, "disqualified");
          s = ", ";
        }
      }
      if (!*s) fprintf(out_f, "&nbsp;");
      fprintf(out_f, "</td>");
      s = "";
      if (ui && ui->location) s = ui->location;
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(s));
      s = "";
      if (ui && ui->printer_name) s = ui->printer_name;
      fprintf(out_f, "<td%s>%s</td>", cl, ARMOR(s));
    }
    fprintf(out_f, "</tr>\n");
  }
  fprintf(out_f, "</table>\n");

  ss_write_html_footer(out_f);

cleanup:
  html_armor_free(&ab);
  userlist_free(&users->b); users = 0;
  xfree(xml_text);
  bitset_free(&marked);
  xfree(marked_str);
  return retval;
}

int
super_serve_op_USER_SEL_VIEW_PASSWD_REDIRECT(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int contest_id = 0, group_id = 0, next_op = 0;
  bitset_t marked = BITSET_INITIALIZER;
  unsigned char *marked_str = 0;

  ss_cgi_param_int_opt(phr, "contest_id", &contest_id, 0);
  ss_cgi_param_int_opt(phr, "group_id", &group_id, 0);
  marked_str = collect_marked_set(phr, &marked);
  switch (phr->opcode) {
  case SSERV_OP_USER_SEL_VIEW_PASSWD_REDIRECT:
    next_op = SSERV_OP_USER_SEL_VIEW_PASSWD_PAGE;
    break;
  case SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_REDIRECT:
    next_op = SSERV_OP_USER_SEL_VIEW_CNTS_PASSWD_PAGE;
    break;
  default:
    abort();
  }
  ss_redirect_2(out_f, phr, next_op, contest_id, group_id, 0, marked_str);

  xfree(marked_str);
  bitset_free(&marked);
  return 0;
}

int
super_serve_op_browse_groups(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  return retval;
}

int
super_serve_op_set_group_filter(
        FILE *log_f,
        FILE *out_f,
        struct super_http_request_info *phr)
{
  int retval = 0;
  return retval;
}
