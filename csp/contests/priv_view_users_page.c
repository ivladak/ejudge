/* === string pool === */

static const unsigned char csp_str0[29] = "\n<h2>Registered users</h2>\n\n";
static const unsigned char csp_str1[297] = "\n<table class=\"b1\"><tr><th class=\"b1\">NN</th><th class=\"b1\">Id</th><th class=\"b1\">Login</th><th class=\"b1\">Name</th><th class=\"b1\">Status</th><th class=\"b1\">Flags</th><th class=\"b1\">Reg. date</th><th class=\"b1\">Login date</th><th class=\"b1\">No. of submits</th><th class=\"b1\">Size of submits</th>\n";
static const unsigned char csp_str2[28] = "\n<th class=\"b1\">Score</th>\n";
static const unsigned char csp_str3[34] = "\n<th class=\"b1\">Select</th></tr>\n";
static const unsigned char csp_str4[5] = "\n<tr";
static const unsigned char csp_str5[18] = ">\n<td class=\"b1\">";
static const unsigned char csp_str6[7] = "</td>\n";
static const unsigned char csp_str7[2] = "\n";
static const unsigned char csp_str8[6] = "\n    ";
static const unsigned char csp_str9[17] = "\n<td class=\"b1\">";
static const unsigned char csp_str10[8] = "</td>\n\n";
static const unsigned char csp_str11[29] = "\n<td class=\"b1\">&nbsp;</td>\n";
static const unsigned char csp_str12[21] = "</td><td class=\"b1\">";
static const unsigned char csp_str13[55] = "\n<td class=\"b1\">&nbsp;</td><td class=\"b1\">&nbsp;</td>\n";
static const unsigned char csp_str14[51] = "\n<td class=\"b1\"><input type=\"checkbox\" name=\'user_";
static const unsigned char csp_str15[16] = "\'/></td>\n</tr>\n";
static const unsigned char csp_str16[50] = "\n</table>\n\n<h2>Users range</h2>\n\n<table>\n<tr><td>";
static const unsigned char csp_str17[82] = ":</td><td><input type=\"text\" name=\"first_user_id\" size=\"16\" /></td></tr>\n<tr><td>";
static const unsigned char csp_str18[126] = "</td><td><input type=\"text\" name=\"last_user_id\" size=\"16\" /></td></tr>\n</table>\n\n<h2>Available actions</h2>\n\n<table>\n<tr><td>";
static const unsigned char csp_str19[10] = "</td><td>";
static const unsigned char csp_str20[20] = "</td></tr>\n<tr><td>";
static const unsigned char csp_str21[12] = "</td></tr>\n";
static const unsigned char csp_str22[10] = "\n<tr><td>";
static const unsigned char csp_str23[3] = "\n\n";
static const unsigned char csp_str24[16] = "\n</table>\n\n<h2>";
static const unsigned char csp_str25[10] = "</h2>\n<p>";
static const unsigned char csp_str26[115] = ":<br>\n<p><textarea name=\"disq_comment\" rows=\"5\" cols=\"60\">\n</textarea></p>\n\n<table class=\"b0\"><tr>\n<td class=\"b0\">";
static const unsigned char csp_str27[26] = "</td>\n</tr></table>\n\n<h2>";
static const unsigned char csp_str28[79] = "</h2>\n<table>\n<tr><td><input type=\"text\" size=\"32\" name=\"add_login\"/></td><td>";
static const unsigned char csp_str29[78] = "</td></tr>\n<tr><td><input type=\"text\" size=\"32\" name=\"add_user_id\"/></td><td>";
static const unsigned char csp_str30[21] = "</td></tr>\n</table>\n";
static const unsigned char csp_str31[9] = "\n\n<hr/>\n";
static const unsigned char csp_str32[18] = "\n</body>\n</html>\n";


#line 2 "priv_view_users_page.csp"
/* $Id$ */

#include "new-server.h"
#include "misctext.h"
#include "contests.h"
#include "userlist_proto.h"
#include "userlist_clnt.h"
#include "userlist.h"
#include "runlog.h"
#include "l10n.h"
#include "mischtml.h"
#include "prepare.h"
#include "xml_utils.h"
#include "external_action.h"
#include "copyright.h"

#include "reuse/xalloc.h"

#include <stdio.h>

#include <libintl.h>
#define _(x) gettext(x)

int csp_view_priv_view_users_page(PageInterface *pg, FILE *log_f, FILE *out_f, struct http_request_info *phr);

static PageInterfaceOps priv_view_users_page_ops =
{
    NULL, // destroy
    NULL, // execute
    csp_view_priv_view_users_page, // render
};

static PageInterface priv_view_users_page_iface =
{
    &priv_view_users_page_ops,
};

PageInterface *
csp_get_priv_view_users_page(void)
{
    return &priv_view_users_page_iface;
}
int csp_view_priv_view_users_page(PageInterface *pg, FILE *log_f, FILE *out_f, struct http_request_info *phr)
{

#line 48 "priv_view_users_page.csp"
const struct contest_desc *cnts = phr->cnts;
  struct contest_extra *extra = phr->extra;
  int r;
  unsigned char *xml_text = 0;
  struct userlist_list *users = 0;
  const struct userlist_user *u = 0;
  const struct userlist_contest *uc = 0;
  int uid;
  int row = 1, serial = 1;
  struct html_armor_buffer ab = HTML_ARMOR_INITIALIZER;
  int details_allowed = 0;
  unsigned char b1[1024];
  int new_contest_id = cnts->id;
  const struct section_global_data *global = extra->serve_state->global;
  int *run_counts = 0;
  size_t *run_sizes = 0;
  unsigned char hbuf[1024];
  const unsigned char *sep;

static const unsigned char * const form_row_attrs[]=
{
  " bgcolor=\"#d0d0d0\"",
  " bgcolor=\"#e0e0e0\"",
};

  if (cnts->user_contest_num > 0) new_contest_id = cnts->user_contest_num;
  if (ns_open_ul_connection(phr->fw_state) < 0) {
    ns_html_err_ul_server_down(out_f, phr, 1, 0);
    return 0;
  }
  if ((r = userlist_clnt_list_all_users(ul_conn, ULS_LIST_ALL_USERS,
                                        phr->contest_id, &xml_text)) < 0) {
    ns_html_err_internal_error(out_f, phr, 1,
                                      "list_all_users failed: %s",
                                      userlist_strerror(-r));
    return 0;
  }
  users = userlist_parse_str(xml_text);
  xfree(xml_text); xml_text = 0;
  if (!users) {
    ns_html_err_internal_error(out_f, phr, 1, "XML parsing failed");
    return 0;
  }

  if (users->user_map_size > 0) {
    XCALLOC(run_counts, users->user_map_size);
    XCALLOC(run_sizes, users->user_map_size);
    run_get_all_statistics(extra->serve_state->runlog_state,
                           users->user_map_size, run_counts, run_sizes);
  }

  if (opcaps_check(phr->caps, OPCAP_GET_USER) >= 0) details_allowed = 1;

  l10n_setlocale(phr->locale_id);
  ns_header(out_f, extra->header_txt, 0, 0, 0, 0, phr->locale_id, cnts,
            phr->client_key,
            "%s [%s, %d, %s]: %s", ns_unparse_role(phr->role), phr->name_arm,
            phr->contest_id, extra->contest_arm, _("Users page"));
fwrite(csp_str0, 1, 28, out_f);
fputs("<form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"", out_f);
fputs(phr->self_url, out_f);
fputs("\">", out_f);
fputs(phr->hidden_vars, out_f);
fwrite(csp_str1, 1, 296, out_f);

#line 111 "priv_view_users_page.csp"
if (global->memoize_user_results > 0) {
fwrite(csp_str2, 1, 27, out_f);

#line 113 "priv_view_users_page.csp"
}
fwrite(csp_str3, 1, 33, out_f);

#line 115 "priv_view_users_page.csp"
for (uid = 1; uid < users->user_map_size; uid++) {
    if (!(u = users->user_map[uid])) continue;
    if (!(uc = userlist_get_user_contest(u, new_contest_id))) continue;
fwrite(csp_str4, 1, 4, out_f);
fputs((form_row_attrs[row ^= 1]), out_f);
fwrite(csp_str5, 1, 17, out_f);
fprintf(out_f, "%d", (int)(serial++));
fwrite(csp_str6, 1, 6, out_f);

#line 122 "priv_view_users_page.csp"
snprintf(b1, sizeof(b1), "uid == %d", uid);
fwrite(csp_str7, 1, 1, out_f);
fwrite(csp_str8, 1, 5, out_f);
fwrite(csp_str9, 1, 16, out_f);
fputs("<a href=\"", out_f);
sep = ns_url_2(out_f, phr, NEW_SRV_ACTION_MAIN_PAGE);
fputs(sep, out_f); sep = "&amp;";
fputs("filter_expr=", out_f);
url_armor_string(hbuf, sizeof(hbuf), (b1));
fputs(hbuf, out_f);
(void) sep;
fputs("\">", out_f);
fprintf(out_f, "%d", (int)(uid));
fputs("</a>", out_f);
fwrite(csp_str10, 1, 7, out_f);

#line 129 "priv_view_users_page.csp"
if (details_allowed) {
fwrite(csp_str9, 1, 16, out_f);
fputs("<a href=\"", out_f);
sep = ns_url_2(out_f, phr, NEW_SRV_ACTION_VIEW_USER_INFO);
fputs(sep, out_f); sep = "&amp;";
fputs("user_id=", out_f);
fprintf(out_f, "%d", (int)(uid));
(void) sep;
fputs("\">", out_f);
fputs(html_armor_buf(&ab, (u->login)), out_f);
fputs("</a>", out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 131 "priv_view_users_page.csp"
} else {
fwrite(csp_str9, 1, 16, out_f);
fputs(html_armor_buf(&ab, (u->login)), out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 133 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 134 "priv_view_users_page.csp"
if (u->cnts0 && u->cnts0->name && *u->cnts0->name) {
fwrite(csp_str9, 1, 16, out_f);
fputs(html_armor_buf(&ab, (u->cnts0->name)), out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 136 "priv_view_users_page.csp"
} else {
fwrite(csp_str11, 1, 28, out_f);

#line 138 "priv_view_users_page.csp"
}
fwrite(csp_str9, 1, 16, out_f);
fputs((userlist_unparse_reg_status(uc->status)), out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 140 "priv_view_users_page.csp"
if ((uc->flags & USERLIST_UC_ALL)) {
      r = 0;
fwrite(csp_str9, 1, 16, out_f);

#line 143 "priv_view_users_page.csp"
if ((uc->flags & USERLIST_UC_BANNED))
        fprintf(out_f, "%s%s", r++?",":"", "banned");
      if ((uc->flags & USERLIST_UC_INVISIBLE))
        fprintf(out_f, "%s%s", r++?",":"", "invisible");
      if ((uc->flags & USERLIST_UC_LOCKED))
        fprintf(out_f, "%s%s", r++?",":"", "locked");
      if ((uc->flags & USERLIST_UC_INCOMPLETE))
        fprintf(out_f, "%s%s", r++?",":"", "incomplete");
      if ((uc->flags & USERLIST_UC_DISQUALIFIED))
        fprintf(out_f, "%s%s", r++?",":"", "disqualified");
fwrite(csp_str6, 1, 6, out_f);

#line 154 "priv_view_users_page.csp"
} else {
fwrite(csp_str11, 1, 28, out_f);

#line 156 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 157 "priv_view_users_page.csp"
if (uc->create_time > 0) {
fwrite(csp_str9, 1, 16, out_f);
fputs(xml_unparse_date((uc->create_time)), out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 159 "priv_view_users_page.csp"
} else {
fwrite(csp_str11, 1, 28, out_f);

#line 161 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 162 "priv_view_users_page.csp"
if (u->cnts0 && u->cnts0->last_login_time > 0) {
fwrite(csp_str9, 1, 16, out_f);
fputs(xml_unparse_date((u->cnts0->last_login_time)), out_f);
fwrite(csp_str6, 1, 6, out_f);

#line 164 "priv_view_users_page.csp"
} else {
fwrite(csp_str11, 1, 28, out_f);

#line 166 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 167 "priv_view_users_page.csp"
if (run_counts[uid] > 0) {
fwrite(csp_str9, 1, 16, out_f);
fprintf(out_f, "%d", (int)(run_counts[uid]));
fwrite(csp_str12, 1, 20, out_f);
fprintf(out_f, "%zu", (size_t)(run_sizes[uid]));
fwrite(csp_str6, 1, 6, out_f);

#line 169 "priv_view_users_page.csp"
} else {
fwrite(csp_str13, 1, 54, out_f);

#line 171 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 172 "priv_view_users_page.csp"
if (global->memoize_user_results > 0) {
fwrite(csp_str9, 1, 16, out_f);
fprintf(out_f, "%d", (int)(serve_get_user_result_score(extra->serve_state, uid)));
fwrite(csp_str6, 1, 6, out_f);

#line 174 "priv_view_users_page.csp"
}
fwrite(csp_str14, 1, 50, out_f);
fprintf(out_f, "%d", (int)(uid));
fwrite(csp_str15, 1, 15, out_f);

#line 177 "priv_view_users_page.csp"
}
fwrite(csp_str16, 1, 49, out_f);
fputs(_("First User_Id"), out_f);
fwrite(csp_str17, 1, 81, out_f);
fputs(_("Last User_Id (incl.)"), out_f);
fwrite(csp_str18, 1, 125, out_f);
fputs(ns_aref(hbuf, sizeof(hbuf), phr, NEW_SRV_ACTION_MAIN_PAGE, 0), out_f);
fputs(_("Back"), out_f);
fputs("</a>", out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Return to the main page"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_REMOVE_REGISTRATIONS, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Remove the selected users from the list"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_PENDING, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the registration status of the selected users to PENDING"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_OK, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the registration status of the selected users to OK"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_REJECTED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the registration status of the selected users to REJECTED"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_INVISIBLE, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the INVISIBLE flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_CLEAR_INVISIBLE, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Clear the INVISIBLE flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_BANNED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the BANNED flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_CLEAR_BANNED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Clear the BANNED flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_LOCKED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the LOCKED flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_CLEAR_LOCKED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Clear the LOCKED flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_INCOMPLETE, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Set the INCOMPLETE flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_CLEAR_INCOMPLETE, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Clear the INCOMPLETE flag for the selected users"), out_f);
fwrite(csp_str20, 1, 19, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_CLEAR_DISQUALIFIED, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Clear the DISQUALIFIED flag for the selected users"), out_f);
fwrite(csp_str21, 1, 11, out_f);

#line 204 "priv_view_users_page.csp"
if (global->is_virtual) {
fwrite(csp_str22, 1, 9, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_FORCE_START_VIRTUAL, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Force virtual contest start for the selected users"), out_f);
fwrite(csp_str21, 1, 11, out_f);

#line 206 "priv_view_users_page.csp"
}
fwrite(csp_str23, 1, 2, out_f);

#line 208 "priv_view_users_page.csp"
if (global->user_exam_protocol_header_txt) {
fwrite(csp_str22, 1, 9, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_PRINT_SELECTED_USER_PROTOCOL, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Print the user examination protocols for the selected users"), out_f);
fwrite(csp_str21, 1, 11, out_f);

#line 210 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 211 "priv_view_users_page.csp"
if (global->full_exam_protocol_header_txt) {
fwrite(csp_str22, 1, 9, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_PRINT_SELECTED_USER_FULL_PROTOCOL, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Print the user full examination protocols for the selected users"), out_f);
fwrite(csp_str21, 1, 11, out_f);

#line 213 "priv_view_users_page.csp"
}
fwrite(csp_str7, 1, 1, out_f);

#line 214 "priv_view_users_page.csp"
if (global->full_exam_protocol_header_txt) {
fwrite(csp_str22, 1, 9, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_PRINT_SELECTED_UFC_PROTOCOL, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Print the user full cyphered examination protocols for the selected users"), out_f);
fwrite(csp_str21, 1, 11, out_f);

#line 216 "priv_view_users_page.csp"
}
fwrite(csp_str24, 1, 15, out_f);
fputs(_("Disqualify selected users"), out_f);
fwrite(csp_str25, 1, 9, out_f);
fputs(_("Disqualification explanation"), out_f);
fwrite(csp_str26, 1, 114, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_SET_DISQUALIFIED, NULL), out_f);
fwrite(csp_str27, 1, 25, out_f);
fputs(_("Add new user"), out_f);
fwrite(csp_str28, 1, 78, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_ADD_BY_LOGIN, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Add a new user specifying his/her login"), out_f);
fwrite(csp_str29, 1, 77, out_f);
fputs(ns_submit_button(hbuf, sizeof(hbuf), 0, NEW_SRV_ACTION_USERS_ADD_BY_USER_ID, NULL), out_f);
fwrite(csp_str19, 1, 9, out_f);
fputs(_("Add a new user specifying his/her User Id"), out_f);
fwrite(csp_str30, 1, 20, out_f);
fputs("</form>", out_f);
fwrite(csp_str31, 1, 8, out_f);
write_copyright_short(out_f);
fwrite(csp_str32, 1, 17, out_f);

#line 240 "priv_view_users_page.csp"
l10n_setlocale(0);

  if (users) userlist_free(&users->b);
  html_armor_free(&ab);
  xfree(run_counts);
  xfree(run_sizes);
fwrite(csp_str7, 1, 1, out_f);
  return 0;
}